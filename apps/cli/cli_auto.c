/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
 * Autocli mode support
 * The code uses two variables saved in the clixon handle and accessed via clicon_data_cvec_get,set:
 *   cli-edit-mode - This is the api-path of the current cli mode in the loaded yang context
 *   cli-edit-cvv  - These are the assigned cligen list of variables with values at the edit-mode
 *   cli-edit-filter - Label filters for this mode
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#include <dirent.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pwd.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_cli_api.h"
#include "cli_autocli.h"
#include "cli_common.h"

/*
 * CLIXON CLI parse-tree workpoint API: Essentially a mirror of the cligen_wp_set() and similar functions
 */
static char *
co2apipath(cg_obj *co)
{
    cg_callback *cb;
    cvec        *cvv;
    cg_var      *cv;
    
    if (co == NULL)
	return NULL;
    if ((cb = co->co_callbacks) == NULL)
	return NULL;
    if ((cvv = cb->cc_cvec) == NULL)
	return NULL;
    if ((cv = cvec_i(cvv, 0)) == NULL)
	return NULL;
    return cv_string_get(cv);
}

/* Append to cvv1 to cvv0
 * @note if cvv0 is non-null, the first element of cvv1 is skipped
 */
static cvec*
cvec_append(cvec *cvv0,
	    cvec *cvv1)
{
    cvec   *cvv2 = NULL;
    cg_var *cv;
    
    if (cvv0 == NULL){
	if ((cvv2 = cvec_dup(cvv1)) == NULL){
	    clicon_err(OE_UNIX, errno, "cvec_dup");
	    return NULL;
	}
    }
    else{
	if ((cvv2 = cvec_dup(cvv0)) == NULL){
	    clicon_err(OE_UNIX, errno, "cvec_dup");
	    return NULL;
	}
	cv = NULL; 	/* Append cvv1 to cvv2 */
	while ((cv = cvec_each1(cvv1, cv)) != NULL)
	    cvec_append_var(cvv2, cv);
    }
    return cvv2;
}

/*! x is element and has exactly one child which in turn has none 
 * @see child_type in clixon_json.c
 */
static int
tleaf(cxobj *x)
{
    cxobj *xc;

    if (xml_type(x) != CX_ELMNT)
	return 0;
    if (xml_child_nr_notype(x, CX_ATTR) != 1)
	return 0;
    /* From here exactly one noattr child, get it */
    xc = NULL;
    while ((xc = xml_child_each(x, xc, -1)) != NULL)
	if (xml_type(xc) != CX_ATTR)
	    break;
    if (xc == NULL)
	return -1; /* n/a */
    return (xml_child_nr_notype(xc, CX_ATTR) == 0);
}

/*! Print an XML tree structure from an auto-cli env to stdout and encode chars "<>&"
 *
 * @param[in]   xn          clicon xml tree
 * @param[in]   level       how many spaces to insert before each line
 * @param[in]   prettyprint insert \n and spaces tomake the xml more readable.
 * @param[in]   fn          Callback to make print function
 * @see clicon_xml2cbuf
 * One can use clicon_xml2cbuf to get common code, but using fprintf is
 * much faster than using cbuf and then printing that,...
 */
int
cli_xml2file(cxobj            *xn,
	     int               level,
	     int               prettyprint,
	     clicon_output_cb *fn)
{
    int    retval = -1;
    char  *name;
    char  *namespace;
    cxobj *xc;
    int    hasbody;
    int    haselement;
    char  *val;
    char  *encstr = NULL; /* xml encoded string */
    int    exist = 0;

    if (xn == NULL)
	goto ok;
    if (yang_extension_value(xml_spec(xn), "hide-show", CLIXON_AUTOCLI_NS, &exist, NULL) < 0)
	goto done;
    if (exist)
	goto ok;
    name = xml_name(xn);
    namespace = xml_prefix(xn);
    switch(xml_type(xn)){
    case CX_BODY:
	if ((val = xml_value(xn)) == NULL) /* incomplete tree */
	    break;
	if (xml_chardata_encode(&encstr, "%s", val) < 0)
	    goto done;
	(*fn)(stdout, "%s", encstr);
	break;
    case CX_ATTR:
	(*fn)(stdout, " ");
	if (namespace)
	    (*fn)(stdout, "%s:", namespace);
	(*fn)(stdout, "%s=\"%s\"", name, xml_value(xn));
	break;
    case CX_ELMNT:
	(*fn)(stdout, "%*s<", prettyprint?(level*3):0, "");
	if (namespace)
	    (*fn)(stdout, "%s:", namespace);
	(*fn)(stdout, "%s", name);
	hasbody = 0;
	haselement = 0;
	xc = NULL;
	/* print attributes only */
	while ((xc = xml_child_each(xn, xc, -1)) != NULL) {
	    switch (xml_type(xc)){
	    case CX_ATTR:
		if (cli_xml2file(xc, level+1, prettyprint, fn) <0)
		    goto done;
		break;
	    case CX_BODY:
		hasbody=1;
		break;
	    case CX_ELMNT:
		haselement=1;
		break;
	    default:
		break;
	    }
	}
	/* Check for special case <a/> instead of <a></a>:
	 * Ie, no CX_BODY or CX_ELMNT child.
	 */
	if (hasbody==0 && haselement==0) 
	    (*fn)(stdout, "/>");
	else{
	    (*fn)(stdout, ">");
	    if (prettyprint && hasbody == 0)
		(*fn)(stdout, "\n");
	    xc = NULL;
	    while ((xc = xml_child_each(xn, xc, -1)) != NULL) {
		if (xml_type(xc) != CX_ATTR)
		    if (cli_xml2file(xc, level+1, prettyprint, fn) <0)
			goto done;
	    }
	    if (prettyprint && hasbody==0)
		(*fn)(stdout, "%*s", level*3, "");
	    (*fn)(stdout, "</");
	    if (namespace)
		(*fn)(stdout, "%s:", namespace);
	    (*fn)(stdout, "%s>", name);
	}
	if (prettyprint)
	    (*fn)(stdout, "\n");
	break;
    default:
	break;
    }/* switch */
 ok:
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}

/*! Translate XML to a "pseudo-code" textual format using a callback - internal function
 * @param[in]  xn     XML object to print
 * @param[in]  fn     Callback to make print function
 * @param[in]  level  print 4 spaces per level in front of each line
 */
int
cli_xml2txt(cxobj            *xn,
	    clicon_output_cb *fn,
	    int               level)
{
    cxobj *xc = NULL;
    int    children=0;
    int    retval = -1;
    int           exist = 0;

    if (xn == NULL || fn == NULL){
	clicon_err(OE_XML, EINVAL, "xn or fn is NULL");
	goto done;
    }
    if (yang_extension_value(xml_spec(xn), "hide-show", CLIXON_AUTOCLI_NS, &exist, NULL) < 0)
	goto done;
    if (exist)
	goto ok;
    xc = NULL;     /* count children (elements and bodies, not attributes) */
    while ((xc = xml_child_each(xn, xc, -1)) != NULL)
	if (xml_type(xc) == CX_ELMNT || xml_type(xc) == CX_BODY)
	    children++;
    if (!children){ /* If no children print line */
	switch (xml_type(xn)){
	case CX_BODY:
	    (*fn)(stdout, "%s;\n", xml_value(xn));
	    break;
	case CX_ELMNT:
	    (*fn)(stdout, "%*s%s;\n", 4*level, "", xml_name(xn));
	    break;
	default:
	    break;
	}
	goto ok;
    }
    (*fn)(stdout, "%*s", 4*level, "");
    (*fn)(stdout, "%s ", xml_name(xn));
    if (!tleaf(xn))
	(*fn)(stdout, "{\n");
    xc = NULL;
    while ((xc = xml_child_each(xn, xc, -1)) != NULL){
	if (xml_type(xc) == CX_ELMNT || xml_type(xc) == CX_BODY)
	    if (cli_xml2txt(xc, fn, level+1) < 0)
		break;
    }
    if (!tleaf(xn))
	(*fn)(stdout, "%*s}\n", 4*level, "");
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Enter a CLI edit mode
 * @param[in]  h    CLICON handle
 * @param[in]  cvv  Vector of variables from CLIgen command-line
 * @param[in]  argv Vector of user-supplied keywords
 * Format of argv:
 *   <api_path_fmt> Generated API PATH (This is where we are in the tree)
 *   <treename>     Name of generated cligen parse-tree, eg "datamodel"
 * Note api_path_fmt is not used in code but must be there in order to pick coorig from matching
 * code
 */
int
cli_auto_edit(clicon_handle h,
	      cvec         *cvv1,
	      cvec         *argv)
{
    int           retval = -1;
    char         *api_path_fmt;  /* xml key format */
    char         *api_path = NULL;
    cg_var       *cv;
    char         *treename;
    pt_head      *ph;  
    cg_obj       *co;
    cg_obj       *coorig;
    cvec         *cvv2 = NULL; /* cvv2 = cvv0 + cvv1 */

    if (cvec_len(argv) != 2){
	clicon_err(OE_PLUGIN, EINVAL, "Usage: %s(api_path_fmt>, <treename>)", __FUNCTION__);
	goto done;
    }
    cv = cvec_i(argv, 1);
    treename = cv_string_get(cv);
    /* Find current cligen tree */
    if ((ph = cligen_ph_find(cli_cligen(h), treename)) == NULL){
	clicon_err(OE_PLUGIN, 0, "No such parsetree header: %s", treename);
	goto done;
    }
    /* Find the matching cligen object 
     * Note, is complicated: either an instantiated tree (co_treeref_orig)
     * or actual tree (co_ref)
     */
    if ((co = cligen_co_match(cli_cligen(h))) != NULL){
	if ((coorig = co->co_treeref_orig) != NULL ||
	    (coorig = co->co_ref) != NULL)
	    cligen_ph_workpoint_set(ph, coorig);
	else {
	    clicon_err(OE_YANG, EINVAL, "No workpoint found");
	    goto done;
	}
    }
    else{
	clicon_err(OE_YANG, EINVAL, "No workpoint found");
	goto done;
    }
    if ((cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv1)) == NULL)
	goto done;
    /*  API_path format */
    if ((api_path_fmt = co2apipath(coorig)) == NULL){
	clicon_err(OE_YANG, EINVAL, "No apipath found");
	goto done;
    }
    /* get api-path and xpath */
    if (api_path_fmt2api_path(api_path_fmt, cvv2, &api_path, NULL) < 0)
	goto done;
    /* Store this as edit-mode */
    if (clicon_data_set(h, "cli-edit-mode", api_path) < 0)
	goto done;
    if (clicon_data_cvec_set(h, "cli-edit-cvv", cvv2) < 0)
	goto done;
    if (co->co_filter){
	cvec *cvv3;
	if ((cvv3 = cvec_dup(co->co_filter)) == NULL){
	    clicon_err(OE_YANG, errno, "cvec_dup");
	    goto done;
	}
	if (clicon_data_cvec_set(h, "cli-edit-filter", cvv3) < 0)
	    goto done;
    }
    retval = 0;
 done:
    if (api_path)
	free(api_path);
    return retval;
}

/*! CLI callback: Working point tree up to parent
 * @param[in]  h    CLICON handle
 * @param[in]  cvv  Vector of variables from CLIgen command-line
 * @param[in]  argv Vector of user-supplied keywords
 * Format of argv:
 *   <treename>     Name of generated cligen parse-tree, eg "datamodel"
 */
int
cli_auto_up(clicon_handle h,
	    cvec         *cvv,
	    cvec         *argv)
{
    int      retval = -1;
    cg_var  *cv;
    char    *treename;
    cg_obj  *co0 = NULL; /* from */
    cg_obj  *co1 = NULL; /* to (parent, or several parent steps) */
    pt_head *ph;
    cvec    *cvv0 = NULL;
    cvec    *cvv1 = NULL; /* copy */
    char    *api_path_fmt0;  /* from */
    char    *api_path_fmt1;  /* to */
    char    *api_path = NULL;
    int      i;
    int      j;
    size_t   len;
    cvec    *cvv_filter = NULL;
    
    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, EINVAL, "Usage: %s(<treename>)", __FUNCTION__);
	goto done;
    }
    cv = cvec_i(argv, 0);
    treename = cv_string_get(cv);
    if ((ph = cligen_ph_find(cli_cligen(h), treename)) == NULL){
	clicon_err(OE_PLUGIN, 0, "No such parsetree header: %s", treename);
	goto done;
    }
    if ((co0 = cligen_ph_workpoint_get(ph)) == NULL)
	goto ok;
    cvv_filter = clicon_data_cvec_get(h, "cli-edit-filter");
    /* Find parent that has a callback, XXX has edit */
    for (co1 = co_up(co0); co1; co1 = co_up(co1)){
	cg_obj *cot = NULL;
	if (co_terminal(co1, &cot)){
	    if (cot == NULL)
		break; /* found top */
	    if (cvv_filter){
		cv = NULL;
		while ((cv = cvec_each(cot->co_cvec, cv)) != NULL){
		    if (co_isfilter(cvv_filter, cv_name_get(cv)))
			break;
		}
		if (cv == NULL)
		    break; /* no filter match */
	    }
	}
    }
    cligen_ph_workpoint_set(ph, co1);
    if (co1 == NULL){
	clicon_data_set(h, "cli-edit-mode", "");
	clicon_data_cvec_del(h, "cli-edit-cvv");
	clicon_data_cvec_del(h, "cli-edit-filter");
	goto ok;
    }
    /* get before and after api-path-fmt (as generated from yang) */
    api_path_fmt0 = co2apipath(co0);
    api_path_fmt1 = co2apipath(co1);
    assert(strlen(api_path_fmt0) > strlen(api_path_fmt1));
    /* Find diff of 0 and 1 (how many variables differ?) and trunc cvv0 by that amount */
    cvv0 = clicon_data_cvec_get(h, "cli-edit-cvv");
    j=0; /* count diffs */
    len = strlen(api_path_fmt0);
    for (i=strlen(api_path_fmt1); i<len; i++)
	if (api_path_fmt0[i] == '%')
	    j++;
    cvv1 = cvec_new(0);
    for (i=0; i<cvec_len(cvv0)-j; i++){
	cv = cvec_i(cvv0, i);
	cvec_append_var(cvv1, cv);	
    }
    /* get api-path and xpath */
    if (api_path_fmt2api_path(api_path_fmt1, cvv1, &api_path, NULL) < 0)
	goto done;
    /* Store this as edit-mode */
    clicon_data_set(h, "cli-edit-mode", api_path);
    clicon_data_cvec_set(h, "cli-edit-cvv", cvv1);
 ok:
    retval = 0;
 done:
    if (api_path)
	free(api_path);
    return retval;
}

/*! CLI callback: Working point tree reset to top level
 * @param[in]  h    CLICON handle
 * @param[in]  cvv  Vector of variables from CLIgen command-line
 * @param[in]  argv Vector of user-supplied keywords
 * Format of argv:
 *   <treename>     Name of generated cligen parse-tree, eg "datamodel"
 */
int
cli_auto_top(clicon_handle h,
	     cvec         *cvv,
	     cvec         *argv)
{
    int      retval = -1;
    cg_var  *cv;
    char    *treename;
    pt_head *ph;
    
    cv = cvec_i(argv, 0);
    treename = cv_string_get(cv);
    if ((ph = cligen_ph_find(cli_cligen(h), treename)) == NULL){
	clicon_err(OE_PLUGIN, 0, "No such parsetree header: %s", treename);
	goto done;
    }
    cligen_ph_workpoint_set(ph, NULL);
    /* Store this as edit-mode */
    clicon_data_set(h, "cli-edit-mode", "");
    clicon_data_cvec_del(h, "cli-edit-cvv");
    clicon_data_cvec_del(h, "cli-edit-filter");
    retval = 0;
 done:
    return retval;
}

/*! CLI callback: Working point tree show
 * @param[in]  h    CLICON handle
 * @param[in]  cvv  Vector of variables from CLIgen command-line
 * @param[in]  argv Vector of user-supplied keywords
 * Format of argv:
 *   <treename>     Name of generated cligen parse-tree, eg "datamodel"
 *   <dbname>       "running"|"candidate"|"startup"
 *   <format>       "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 *   <pretty>       true|false: pretty-print or not
 *   <state>        true|false: pretty-print or not
 *   <prefix>       to print before cli syntax output
 * @see cli_show_auto
 */
int
cli_auto_show(clicon_handle h,
	      cvec         *cvv,
	      cvec         *argv)
{
    int         retval = -1;
    char       *treename;
    char       *db;
    char       *api_path = NULL;
    char       *formatstr;
    enum format_enum format;
    pt_head    *ph;
    char       *xpath = NULL;
    cxobj      *xp;
    cxobj      *xc = NULL;
    cvec       *nsc = NULL;
    yang_stmt  *yspec;
    cxobj      *xerr;
    cxobj      *xt = NULL;
    cxobj     **vec = NULL;
    size_t      veclen;
    int         i;
    int         isroot;
    int         pretty;
    char       *prefix = NULL;
    int         state;
    cg_var     *boolcv = NULL;

    if (cvec_len(argv) != 5 && cvec_len(argv) != 6){
	clicon_err(OE_PLUGIN, EINVAL, "Usage: <treename> <database> <format> <pretty> <state> [<prefix>].");
	goto done;
    }
    /* First argv argument: treename */
    treename = cv_string_get(cvec_i(argv, 0));
    /* Second argv argument: Database */
    db = cv_string_get(cvec_i(argv, 1));
    /* Third format: output format */
    formatstr = cv_string_get(cvec_i(argv, 2));
    if ((int)(format = format_str2int(formatstr)) < 0){
	clicon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
	goto done;
    }
    /* Fourth: pretty-print */
    if ((boolcv = cv_new(CGV_BOOL)) == NULL){
	clicon_err(OE_UNIX, errno, "cv_new");
	goto done;
    }
    if (cv_parse(cv_string_get(cvec_i(argv, 3)), boolcv) < 0){
	clicon_err(OE_UNIX, errno, "Parse boolean %s", cv_string_get(cvec_i(argv, 3)));
	goto done;
    }
    pretty = cv_bool_get(boolcv);
    /* Fifth: state */
    if (cv_parse(cv_string_get(cvec_i(argv, 4)), boolcv) < 0){
	clicon_err(OE_UNIX, errno, "Parse boolean %s", cv_string_get(cvec_i(argv, 4)));
	goto done;
    }
    state = cv_bool_get(boolcv);

    /* Sixth: prefix */
    if (cvec_len(argv) == 6) {     
	prefix = cv_string_get(cvec_i(argv, 5));
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    if ((ph = cligen_ph_find(cli_cligen(h), treename)) == NULL){ /* XXX not used */
	clicon_err(OE_PLUGIN, 0, "No such parsetree header: %s", treename);
	goto done;
    }
    /* Store this as edit-mode */
    if (clicon_data_get(h, "cli-edit-mode", &api_path) == 0 && strlen(api_path))
	;
    else
	api_path = "/";
    if (api_path2xpath(api_path, yspec, &xpath, &nsc, NULL) < 0)
	goto done;
    isroot = (xpath == NULL) || strcmp(xpath,"/")==0;
    if (state == 0){     /* Get configuration-only from database */
	if (clicon_rpc_get_config(h, NULL, db, xpath, nsc, &xt) < 0)
	    goto done;
    }
    else {               /* Get configuration and state from database */
	if (strcmp(db, "running") != 0){
	    clicon_err(OE_FATAL, 0, "Show state only for running database, not %s", db);
	    goto done;
	}
	if (clicon_rpc_get(h, xpath, nsc, CONTENT_ALL, -1, &xt) < 0)
	    goto done;
    }
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
	clixon_netconf_error(xerr, "Get configuration", NULL);
	goto done;
    }
    if (xpath_vec(xt, nsc, "%s", &vec, &veclen, xpath) < 0) 
	goto done;
    
    for (i=0; i<veclen; i++){
	xp = vec[i];
	/* Print configuration according to format */
	switch (format){
	case FORMAT_XML:
	    if (isroot)
		cli_xml2file(xp, 0, pretty, fprintf);
	    else{
		while ((xc = xml_child_each(xp, xc, CX_ELMNT)) != NULL)
		    cli_xml2file(xc, 0, pretty, fprintf);
	    }
	    fprintf(stdout, "\n");
	    break;
	case FORMAT_JSON:
	    if (isroot)
		xml2json_cb(stdout, xp, pretty, cligen_output);
	    else{
		while ((xc = xml_child_each(xp, xc, CX_ELMNT)) != NULL)
		    xml2json_cb(stdout, xc, pretty, cligen_output);
	    }
	    fprintf(stdout, "\n");
	    break;
	case FORMAT_TEXT:
	    if (isroot)
		cli_xml2txt(xp, cligen_output, 0); /* tree-formed text */
	    else
		while ((xc = xml_child_each(xp, xc, CX_ELMNT)) != NULL)
		    cli_xml2txt(xc, cligen_output, 0); /* tree-formed text */
	    break;
	case FORMAT_CLI:
	    if (isroot)
		xml2cli(h, stdout, xp, prefix, cligen_output);
	    else
		while ((xc = xml_child_each(xp, xc, CX_ELMNT)) != NULL)
		    xml2cli(h, stdout, xc, prefix, cligen_output);
	    break;
	case FORMAT_NETCONF:
	    fprintf(stdout, "<rpc xmlns=\"%s\" %s><edit-config><target><candidate/></target><config>",
		    NETCONF_BASE_NAMESPACE, NETCONF_MESSAGE_ID_ATTR);
	    if (pretty)
		fprintf(stdout, "\n");
	    if (isroot)
		cli_xml2file(xp, 2, pretty, fprintf);
	    else
		while ((xc = xml_child_each(xp, xc, CX_ELMNT)) != NULL)
		    cli_xml2file(xc, 2, pretty, fprintf);
	    fprintf(stdout, "</config></edit-config></rpc>]]>]]>\n");
	    break;
	} /* switch */
    }
    retval = 0;
 done:
    if (boolcv)
	cv_free(boolcv);
    if (xt)
	xml_free(xt);
    if (nsc)
	xml_nsctx_free(nsc);
    if (vec)
	free(vec);
    if (xpath)
	free(xpath);
    return retval;
}

/*! CLI callback: set auto db item
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * Format of argv:
 *   <api-path-fmt> Generated
 */
int 
cli_auto_set(clicon_handle h,
	     cvec         *cvv,
	     cvec         *argv)
{
    int   retval = -1;
    cvec *cvv2 = NULL;
    
    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml(h, cvv2, argv, OP_REPLACE, NULL) < 0)
	goto done;
    retval = 0;
 done:
    if (cvv2)
	cvec_free(cvv2);
    return retval;
}

/*! Merge datastore xml entry
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 */
int 
cli_auto_merge(clicon_handle h,
	       cvec         *cvv,
	       cvec         *argv)
{
    int retval = -1;
    cvec *cvv2 = NULL;
    
    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml(h, cvv2, argv, OP_MERGE, NULL) < 0)
	goto done;
    retval = 0;
 done:
    if (cvv2)
	cvec_free(cvv2);
    return retval;
}

/*! Create datastore xml entry
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 */
int 
cli_auto_create(clicon_handle h,
		cvec         *cvv,
		cvec         *argv)
{
    int retval = -1;
    cvec *cvv2 = NULL;
    
    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml(h, cvv2, argv, OP_CREATE, NULL) < 0)
	goto done;
    retval = 0;
 done:
    if (cvv2)
	cvec_free(cvv2);
    return retval;
}

/*! Delete datastore xml
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 */
int 
cli_auto_del(clicon_handle h,
	     cvec         *cvv,
	     cvec         *argv)
{
    int   retval = -1;
    cvec *cvv2 = NULL;
    
    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml(h, cvv2, argv, OP_REMOVE, NULL) < 0)
	goto done;
    retval = 0;
 done:
    if (cvv2)
	cvec_free(cvv2);
    return retval;
}

struct findpt_arg{
    char   *fa_str; /* search string */
    cg_obj *fa_co;  /* result */
};

/*! Iterate through parse-tree to find first argument set by cli_generate code
 * @see cg_applyfn_t
 * @param[in]  co   CLIgen parse-tree object
 * @param[in]  arg  Argument, cast to application-specific info
 * @retval     -1   Error: break and return
 * @retval     0    OK and continue
 * @retval     1    OK and return (abort iteration)
 */
static int
cli_auto_findpt(cg_obj *co,
		void   *arg)
{
    struct findpt_arg *fa = (struct findpt_arg *)arg;
    cvec              *cvv;

    if (co->co_callbacks && (cvv = co->co_callbacks->cc_cvec))
	if (strcmp(fa->fa_str, cv_string_get(cvec_i(cvv, 0))) == 0){
	    fa->fa_co = co;
	    return 1;
	}
    return 0;
}

/*! Enter edit mode
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  argv Vector of args to function in command. 
 * Format of argv:
 *   <api_path_fmt> Generated API PATH FORMAT (print-like for variables)
 *   <vars>*        List of static variables that can be used as values for api_path_fmt
 * In this example all static variables are added and dynamic variables are appended
 * But this can be done differently
 * Example: 
 *   api_path_fmt=/a/b=%s,%s/c
 *   cvv: "cmd 42", 42
 *   argv: 99
 *   api_path: /a/b=42,99/c
 * @see cli_auto_edit
 */
int 
cli_auto_sub_enter(clicon_handle h,
		   cvec         *cvv,
		   cvec         *argv)
{
    int           retval = -1;
    char         *api_path_fmt;    /* Contains wildcards as %.. */
    char         *api_path = NULL;
    char         *treename;
    cvec         *cvv1 = NULL;
    cvec         *cvv2 = NULL;
    int           i;
    cg_var       *cv = NULL;
    pt_head      *ph;
    struct findpt_arg fa = {0,};
    
    if (cvec_len(argv) < 2){
	clicon_err(OE_PLUGIN, EINVAL, "Usage: %s(<tree> <api_path_fmt> (,vars)*)", __FUNCTION__);
	goto done;
    }
    /* First argv argument: treename */
    cv = cvec_i(argv, 0);
    treename = cv_string_get(cv);
    /* Second argv argument: API_path format */
    cv = cvec_i(argv, 1);
    api_path_fmt = cv_string_get(cv);

    /* if api_path_fmt contains print like % statements, 
     * values must be assigned, either dynaically from cvv (cli command line) or statically 
     * in code here.
     * This is done by constructing a cvv1 here which suits your needs
     * In this example all variables from cvv are appended with all given static variables in 
     * argv, but this can be done differently
     */
    /* Create a cvv with variables to add to api-path */
    if ((cvv1 = cvec_new(0)) == NULL){ 
	clicon_err(OE_UNIX, errno, "cvec_new");
	goto done;
    }
    /* Append static variables (skip first treename) */
    for (i=1; i<cvec_len(argv); i++){
	if (cvec_append_var(cvv1, cvec_i(argv, i)) < 0)
	    goto done;
    }
    /* Append dynamic variables from the command line (skip first contains whole command line) */
    for (i=1; i<cvec_len(cvv); i++){
	if (cvec_append_var(cvv1, cvec_i(cvv, i)) < 0)
	    goto done;
    }
    if (api_path_fmt2api_path(api_path_fmt, cvv1, &api_path, NULL) < 0)
	goto done;
    /* Assign the variables */
    if ((cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv1)) == NULL)
	goto done;
    /* Store this as edit-mode */
    if (clicon_data_set(h, "cli-edit-mode", api_path) < 0)
	goto done;
    if (clicon_data_cvec_set(h, "cli-edit-cvv", cvv2) < 0)
	goto done;
    /* Find current cligen tree */
    if ((ph = cligen_ph_find(cli_cligen(h), treename)) == NULL){
	clicon_err(OE_PLUGIN, ENOENT, "No such parsetree header: %s", treename);
	goto done;
    }
    /* Find the point in the generated clispec tree where workpoint should be set */
    fa.fa_str = api_path_fmt;
    if (pt_apply(cligen_ph_parsetree_get(ph), cli_auto_findpt, INT32_MAX, &fa) < 0)
	goto done;
    if (fa.fa_co == NULL){
	clicon_err(OE_PLUGIN, ENOENT, "No such cligen object found %s", api_path);
	goto done;
    }
    cligen_ph_workpoint_set(ph, fa.fa_co);
    retval = 0;
 done:
    if (api_path)
	free(api_path);
    if (cvv1)
	cvec_free(cvv1);
    return retval;
}
