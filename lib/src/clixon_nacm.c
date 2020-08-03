/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

 * NACM code according to RFC8341 Network Configuration Access Control Model
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <assert.h>
#include <syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_string.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_netconf_lib.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_yang_module.h"
#include "clixon_datastore.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_map.h"
#include "clixon_path.h"
#include "clixon_xml_vec.h"
#include "clixon_nacm.h"

/* NACM namespace for use with xml namespace contexts and xpath */
#define NACM_NS "urn:ietf:params:xml:ns:yang:ietf-netconf-acm"

/*! Match nacm access operations according to RFC8341 3.4.4.  
 * Incoming RPC Message Validation Step 7 (c)
 *  The rule's "access-operations" leaf has the "exec" bit set or
 *  has the special value "*".
 * @param[in] mode  Primary mode, eg read, create, update, delete, exec
 * @param[in] mode2 Secondary mode, eg "write"
 * @retval 0  No match
 * @retval 1  Match
 * @note access_operations is bit-fields
 */
static int
match_access(char *access_operations,
	     char *mode,
	     char *mode2)
{
    if (access_operations==NULL)
	return 0;
    if (strcmp(access_operations,"*")==0)
	return 1;
    if (strstr(access_operations, mode)!=NULL)
	return 1;
    if (mode2 && strstr(access_operations, mode2)!=NULL)
	return 1;
    return 0;
}

/*! Match nacm single rule. Either match with access or deny. Or not match.
 * @param[in]  rpc    rpc name
 * @param[in]  module Yang module name
 * @param[in]  xrule  NACM rule XML tree
 * @param[out] cbret  Cligen buffer result. Set to an error msg if retval=0.
 * @retval -1  Error
 * @retval  0  Matching rule AND Not access and cbret set
 * @retval  1  Matching rule AND Access
 * @retval  2  No matching rule Goto step 10
 * @see RFC8341 3.4.4.  Incoming RPC Message Validation
 7.(cont) A rule matches if all of the following criteria are met: 
        *  The rule's "module-name" leaf is "*" or equals the name of
           the YANG module where the protocol operation is defined.

        *  Either (1) the rule does not have a "rule-type" defined or
           (2) the "rule-type" is "protocol-operation" and the
           "rpc-name" is "*" or equals the name of the requested
           protocol operation.

        *  The rule's "access-operations" leaf has the "exec" bit set or
           has the special value "*".
 */
static int
nacm_rule_rpc(char         *rpc,
	      char         *module,
	      cxobj        *xrule)
{
    int    retval = -1;
    char  *module_rule; /* rule module name */
    char  *rpc_rule;
    char  *access_operations;
    
    /*  7a) The rule's "module-name" leaf is "*" or equals the name of
	the YANG module where the protocol operation is defined. */
    if ((module_rule = xml_find_body(xrule, "module-name")) == NULL)
	goto nomatch;
    if (strcmp(module_rule,"*") && strcmp(module_rule,module))
	goto nomatch;
    /*  7b) Either (1) the rule does not have a "rule-type" defined or
	(2) the "rule-type" is "protocol-operation" and the
	"rpc-name" is "*" or equals the name of the requested
	protocol operation. */
    if ((rpc_rule = xml_find_body(xrule, "rpc-name")) == NULL){
	if (xml_find_body(xrule, "path") || xml_find_body(xrule, "notification-name"))
	    goto nomatch;
    }
    if (rpc_rule && (strcmp(rpc_rule, "*") && strcmp(rpc_rule, rpc)))
	goto nomatch;
    /* 7c) The rule's "access-operations" leaf has the "exec" bit set or
	has the special value "*". */
    access_operations = xml_find_body(xrule, "access-operations");
    if (!match_access(access_operations, "exec", NULL))
	goto nomatch;
    retval = 1;
 done:
    return retval;
 nomatch:
    retval = 0;
    goto done;
}

/*! Process nacm incoming RPC message validation steps
 * @param[in]  module   Yang module name
 * @param[in]  rpc      rpc name
 * @param[in]  username User name of requestor
 * @param[in]  xnacm    NACM xml tree
 * @param[out] cbret Cligen buffer result. Set to an error msg if retval=0.
 * @retval -1  Error
 * @retval  0  Not access and cbret set
 * @retval  1  Access
 * @see RFC8341 3.4.4.  Incoming RPC Message Validation
 * @see nacm_datanode_write
 * @see nacm_datanode_read
 */
int
nacm_rpc(char         *rpc,
	 char         *module,
	 char         *username,
	 cxobj        *xnacm,
	 cbuf         *cbret)
{
    int     retval = -1;
    cxobj  *xrule;
    cxobj **gvec = NULL; /* groups */
    size_t  glen;
    cxobj  *rlist;
    cxobj **rlistvec = NULL; /* rule-list */
    size_t  rlistlen;
    cxobj **rvec = NULL; /* rules */
    size_t  rlen;
    int     i, j;
    char   *exec_default = NULL;
    char   *gname;
    char   *action;
    int     match= 0;
    cvec   *nsc = NULL;
    
    /* Create namespace context for with nacm namespace as default */
    if ((nsc = xml_nsctx_init(NULL, NACM_NS)) == NULL)
	goto done;
    /* 3.   If the requested operation is the NETCONF <close-session>
       protocol operation, then the protocol operation is permitted.
    */
    if (strcmp(rpc, "close-session") == 0)
	goto permit;
    /* 4.   Check all the "group" entries to see if any of them contain a
       "user-name" entry that equals the username for the session
       making the request.  (If the "enable-external-groups" leaf is
       "true", add to these groups the set of groups provided by the
       transport layer.)	       */
    if (username == NULL)
	goto step10;

    /* User's group */
    if (xpath_vec(xnacm, nsc, "groups/group[user-name='%s']", &gvec, &glen, username) < 0)
	goto done;
    /* 5. If no groups are found, continue with step 10. */
    if (glen == 0)
	goto step10;
    /* 6. Process all rule-list entries, in the order they appear in the
        configuration.  If a rule-list's "group" leaf-list does not
        match any of the user's groups, proceed to the next rule-list
        entry. */
    if (xpath_vec(xnacm, nsc, "rule-list", &rlistvec, &rlistlen) < 0)
	goto done;
    for (i=0; i<rlistlen; i++){
	rlist = rlistvec[i];
	/* Loop through user's group to find match in this rule-list */
	for (j=0; j<glen; j++){
	    gname = xml_find_body(gvec[j], "name");
	    if (xpath_first(rlist, nsc, ".[group='%s']", gname)!=NULL)
		break; /* found */
	}
	if (j==glen) /* not found */
	    continue;
	/* 7. For each rule-list entry found, process all rules, in order,
	   until a rule that matches the requested access operation is
	   found. 
	*/
	if (xpath_vec(rlist, nsc, "rule", &rvec, &rlen) < 0)
	    goto done;
	for (j=0; j<rlen; j++){
	    xrule = rvec[j];
	    if ((match = nacm_rule_rpc(rpc, module, xrule)) < 0)
		goto done;
	    if (match)
		break;
	}
	if (match)
	    break;
	if (rvec){
	    free(rvec);
	    rvec=NULL;
	}
    }
    if (match){
	if ((action = xml_find_body(xrule, "action")) == NULL)
	    goto step10;
	if (strcmp(action, "deny")==0){
	    if (netconf_access_denied(cbret, "application", "access denied") < 0)
		goto done;
	    goto deny;
	}
	else if (strcmp(action, "permit")==0)
	    goto permit;

    }
 step10:
    /*   10.  If the requested protocol operation is defined in a YANG module
        advertised in the server capabilities and the "rpc" statement
        contains a "nacm:default-deny-all" statement, then the protocol
        operation is denied. */
    /* 11.  If the requested protocol operation is the NETCONF
        <kill-session> or <delete-config>, then the protocol operation
        is denied. */
    if (strcmp(rpc, "kill-session")==0 || strcmp(rpc, "delete-config")==0){
	if (netconf_access_denied(cbret, "application", "default deny") < 0)
	    goto done;
	goto deny;
    }
    /*   12.  If the "exec-default" leaf is set to "permit", then permit the
	 protocol operation; otherwise, deny the request. */
    exec_default = xml_find_body(xnacm, "exec-default");
    if (exec_default ==NULL || strcmp(exec_default, "permit")==0)
	goto permit;
    if (netconf_access_denied(cbret, "application", "default deny") < 0)
	goto done;
    goto deny;
 permit:
    retval = 1;
 done:
    clicon_debug(1, "%s retval:%d (0:deny 1:permit)", __FUNCTION__, retval);
    if (nsc)
	xml_nsctx_free(nsc);
    if (gvec)
	free(gvec);
    if (rlistvec)
	free(rlistvec);
    if (rvec)
	free(rvec);
    return retval;
 deny: /* Here, cbret must contain a netconf error msg */
    assert(cbuf_len(cbret));
    retval = 0;
    goto done;
}

/* Local struct for keeping preparation/compiled data in NACM data path code */
struct prepvec{
    qelem_t       pv_q;
    cxobj        *pv_xrule;
    clixon_xvec  *pv_xpathvec;
};
typedef struct prepvec prepvec;

/*! Delete all Upgrade callbacks
 */
int
prepvec_free(prepvec *pv_list)
{
    prepvec *pv;

    while((pv = pv_list) != NULL) {
	DELQ(pv, pv_list, prepvec *);
	if (pv->pv_xpathvec)
	    clixon_xvec_free(pv->pv_xpathvec);
	free(pv);
    }
    return 0;
}

prepvec *
prepvec_add(prepvec  **pv_listp,
	    cxobj     *xrule)
{
    prepvec *pv;

    if ((pv = malloc(sizeof(*pv))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	return NULL;
    }
    memset(pv, 0, sizeof(*pv));
    ADDQ(pv, *pv_listp);
    pv->pv_xrule = xrule;
    if ((pv->pv_xpathvec = clixon_xvec_new()) == NULL)
	return NULL;
    return pv;
}

/*! Prepare datastructures before running through XML tree
 * Save rules in a "cache"
 * These rules match:
 *  - user/group
 *  - have read access-op, etc
 * Also make instance-id lookups on top object for each rule. Assume at most one result
 */
static int
nacm_datanode_prepare(clicon_handle     h,
		      cxobj            *xt,
		      enum nacm_access  access,
		      cxobj           **gvec,
		      size_t            glen,
		      cxobj           **rlistvec,
		      size_t            rlistlen,
		      cvec             *nsc,
		      prepvec         **pv_listp)
{
    int        retval = -1;
    cxobj     *rlist;
    int        i;
    int        j;
    int        k;
    char      *gname;
    cxobj    **rvec = NULL; /* rules */
    size_t     rlen;	
    cxobj     *xrule;
    cxobj     *pathobj;
    char      *access_operations;
    char      *path0;         /* Non-canonical path */
    char      *path=NULL;     /* Canonical path */
    cvec      *nsc0 = NULL;   /* Non-canonical namespace context */
    yang_stmt *yspec;
    cxobj   **xvec = NULL;
    int       xlen = 0;
    int       ret;
    prepvec *pv;

    yspec = clicon_dbspec_yang(h);
    for (i=0; i<rlistlen; i++){ 	/* Loop through rule list */
	rlist = rlistvec[i];
	/* Loop through user's group to find match in this rule-list */
	for (j=0; j<glen; j++){
	    gname = xml_find_body(gvec[j], "name");
	    if (xpath_first(rlist, nsc, ".[group='%s']", gname)!=NULL)
		break; /* found */
	}
	if (j==glen) /* not found */
	    continue;
	/* 6. For each rule-list entry found, process all rules, in order,
	   until a rule that matches the requested access operation is
	   found. (see 6 sub rules in nacm_rule_datanode
	*/
	if (xpath_vec(rlist, nsc, "rule", &rvec, &rlen) < 0)
	    goto done;
	for (j=0; j<rlen; j++){ /* Loop through rules */
	    xrule = rvec[j];
	    /* 6c) For a "read" access operation, the rule's "access-operations"
	       leaf has the "read" bit set or has the special value "*" */
	    access_operations = xml_find_body(xrule, "access-operations");
	    switch (access){
	    case NACM_READ: 
		/* 6c) For a "read" access operation, the rule's "access-operations"
		   leaf has the "read" bit set or has the special value "*" */
		if (!match_access(access_operations, "read", NULL))
		    continue;
		break;
	    case NACM_CREATE:
		/* 6d) For a "create" access operation, the rule's "access-operations" 
		   leaf has the "create" bit set or has the special value "*". */
		if (!match_access(access_operations, "create", "write"))
		    continue;
		break;
	    case NACM_DELETE:
		/* 6e) For a "delete" access operation, the rule's "access-operations" 
		   leaf has the "delete" bit set or has the  special value "*". */
		if (!match_access(access_operations, "delete", "write"))
		    continue;
		break;
	    case NACM_UPDATE:
		/* 6f) For an "update" access operation, the rule's "access-operations"
		   leaf has the "update" bit set or has the special value "*". */ 
		if (!match_access(access_operations, "update", "write"))
		    continue;
		break;
	    default:
		clicon_err(OE_XML, EINVAL, "Access %d unupported (shouldnt happen)", access);
		goto done;
		break;
	    }
	    /*  6b) Either (1) the rule does not have a "rule-type" defined or
		(2) the "rule-type" is "data-node" and the "path" matches the
		requested data node, action node, or notification node. */    
	    if ((pathobj = xml_find_type(xrule, NULL, "path", CX_ELMNT)) == NULL){
		if (xml_find_body(xrule, "rpc-name") || xml_find_body(xrule, "notification-name"))
		    continue;
		/* Here a new xrule is found, add it */
		if (prepvec_add(pv_listp, xrule) == NULL)
		    goto done;
	    }
	    else{
		path0 = clixon_trim2(xml_body(pathobj), " \t\n");
		/* Create namespace context for with nacm namespace as default */
		if (xml_nsctx_node(pathobj, &nsc0) < 0)
		    goto done;
		/* instance-id requires canonical paths */
		if (xpath2canonical(path0, nsc0, yspec, &path, NULL) < 0)
		    goto done;
		if ((ret = clixon_xml_find_instance_id(xt, yspec, &xvec, &xlen, "%s", path)) < 0)
		    goto done;
		if (ret == 0)
		    continue;
		/* Here a new xrule is found, add it */
		if ((pv = prepvec_add(pv_listp, xrule)) == NULL)
		    goto done;
		for (k=0; k<xlen; k++){
		    if (clixon_xvec_append(pv->pv_xpathvec, xvec[k]) < 0)
			goto done;
		}
		if (xvec){
		    free(xvec);
		    xvec = NULL;
		}
		if (nsc0){
		    cvec_free(nsc0);
		    nsc0 = NULL;
		}
		if (path){
		    free(path);
		    path = NULL;
		}
	    }
	}
	if (rvec){
	    free(rvec);
	    rvec=NULL;
	}	
    }
    retval = 0;
 done:
    if (xvec)
	free(xvec);
    if (path)
	free(path);
    if (nsc0)
	cvec_free(nsc0);
    return retval;
}

/*---------------------------------------------------------------
 * Datanode write
 */

/*! Match specific rule to specific requested node
 * @param[in]  xn       XML node (requested node)
 * @param[in]  xrule    NACM rule
 * @param[in]  xp       Xpath match
 * @param[in]  yspec    YANG spec
 * @retval -1  Error
 * @retval  0  OK and rule does not match
 * @retval  1  OK and rule matches deny
 * @retval  2  OK and rule matches permit
 */
static int
nacm_data_write_xrule_xml(cxobj       *xn,
			  cxobj       *xrule,
			  clixon_xvec *xpathvec,
			  yang_stmt   *yspec)
{
    int        retval = -1;
    yang_stmt *ymod;
    char      *module_pattern; /* rule module name */
    char      *action;
    cxobj     *xp;
    int        i;

    if ((module_pattern = xml_find_body(xrule, "module-name")) == NULL)
	goto nomatch;
    /* 6a) The rule's "module-name" leaf is "*" or equals the name of
     * the YANG module where the requested data node is defined. 
     */
    if (strcmp(module_pattern, "*") != 0){
	if (ys_module_by_xml(yspec, xn, &ymod) < 0)
	    goto done;
	if (strcmp(yang_argument_get(ymod), module_pattern) != 0)
	    goto nomatch;
    }
    action = xml_find_body(xrule, "action"); /* mandatory */
    /*  6b) Either (1) the rule does not have a "rule-type" defined or
	(2) the "rule-type" is "data-node" and the "path" matches the
	Requested data node, action node, or notification node. */    
    if (xml_find_type(xrule, NULL, "path", CX_ELMNT) == NULL){
	if (strcmp(action, "deny")==0)
	    goto deny;
	goto permit;
    }
    for (i=0; i<clixon_xvec_len(xpathvec); i++){
	xp = clixon_xvec_i(xpathvec, i);
	/* Check if ancestor is xp (for every xpathvec?) */
	if (xn == xp || xml_isancestor(xn, xp)){
	    if (strcmp(action, "deny")==0)
		goto deny;
	    goto permit;
	}
    }
    goto nomatch;
 permit:
    retval = 2;       /* rule match and permit */
 done:
    return retval;
 nomatch :  /* rule does not match */
    retval = 0;
    goto done;
 deny:      /* rule match and deny */
    retval = 1;
    goto done;
}

/*! Recursive check for NACM write rules among all XML nodes
 * @param[in]  h         Clicon handle
 * @param[in]  xn        XML node (requested node)
 * @param[in]  rulevec   Precomputed rules that apply to this user group
 * @param[in]  xpathvec  Precomputed xpath results that apply to this XML tree
 * @param[in]  defpermit 0 if default deny, 1 is default permit
 * @param[in]  yspec     YANG spec
 * @param[out] cbret     Error message if retval = 0
 * @retval     1         OK and accept
 * @retval     0         Deny and cbret set
 * @retval     -1        Error
 * XXX differentiate between nomatch: default. or match deny, match accept 
 * nomatch: check write-default rules, next v
 * accept:  Hunky dory
 * deny:    Send error message
 */
static int
nacm_datanode_write_recurse(clicon_handle h,
			    cxobj        *xn,
			    prepvec      *pv_list,
			    int           defpermit,
			    yang_stmt    *yspec,
			    cbuf         *cbret)
{
    int       retval = -1;
    cxobj   *x;
    int      ret = 0;
    prepvec *pv;
    
    pv = pv_list;
    if (pv){
	do {
	    /* return values: -1:Error /0:no match /1: deny /2: permit
	     */
	    if ((ret = nacm_data_write_xrule_xml(xn, pv->pv_xrule, pv->pv_xpathvec, yspec)) < 0) 
		goto done;
	    switch(ret){
	    case 0: /* No match, continue with next rule */
		break;
	    case 1: /* Match and deny: break all traversal and send error back to client */
		if (netconf_access_denied(cbret, "application", "access denied") < 0)
		    goto done;
		goto deny;
		break;
	    case 2: /* Match and permit: break rule processing but continue recursion */
		break;
	    }
	    if (ret == 2)
		break;
	    pv = NEXTQ(prepvec *, pv);
	} while (pv && pv != pv_list);
    }
    /* If no rule match, check default rule: if deny then break traversal and send error */
    if (ret == 0 && !defpermit){
	if (netconf_access_denied(cbret, "application", "default deny") < 0)
	    goto done;
	goto deny;
    }
    /* If node should be purged, dont recurse and defer removal to caller */
    x = NULL; 	/* Recursively check XML */
    while ((x = xml_child_each(xn, x, CX_ELMNT)) != NULL) {
	if ((ret = nacm_datanode_write_recurse(h, x, pv_list,
					       defpermit, yspec, cbret)) < 0)
	    goto done;
	if (ret == 0)
	    goto deny;
    }
    retval = 1; /* accept */
 done:
    return retval;
 deny:
    retval = 0; /* deny */
    goto done;
}

/*! Make nacm datanode and module rule write access validation
 * The operations of NACM are: create, read, update, delete, exec
 *  where write is short-hand for create+delete+update
 * @param[in]  h        Clixon handle
 * @param[in]  xreq     XML requestor node (part of xt) for delete it is existing, for others it is new
 * @param[in]  xt       XML request root tree with "config" label at top.
 * @param[in]  op       NACM access of xreq
 * @param[in]  username User making access
 * @param[in]  xnacm    NACM xml tree
 * @param[out] cbret Cligen buffer result. Set to an error msg if retval=0.
 * @retval -1  Error
 * @retval  0  Not access and cbret set
 * @retval  1  Access
 * @see RFC8341 3.4.5.  Data Node Access Validation
 * @see nacm_datanode_read
 * @see nacm_rpc
 */
int
nacm_datanode_write(clicon_handle    h,
		    cxobj           *xreq,
		    cxobj           *xt,
		    enum nacm_access access,
		    char            *username,
		    cxobj           *xnacm,
		    cbuf            *cbret)
{
    int             retval = -1;
    cxobj         **gvec = NULL; /* groups */
    size_t          glen;
    cxobj         **rlistvec = NULL; /* rule-list */
    size_t          rlistlen;
    cxobj         **rvec = NULL; /* rules */
    char           *write_default = NULL;
    cvec           *nsc = NULL;
    int             ret;
    prepvec        *pv_list = NULL;

    /* Create namespace context for with nacm namespace as default */
    if ((nsc = xml_nsctx_init(NULL, NACM_NS)) == NULL)
	goto done;
    if (xnacm == NULL)
	goto permit;
    /* write-default (create, update, or delete) has default deny so should never be NULL */
    if ((write_default = xml_find_body(xnacm, "write-default")) == NULL){
	clicon_err(OE_XML, EINVAL, "No nacm write-default rule");
	goto done;
    }
    /* 3.   Check all the "group" entries to see if any of them contain a
       "user-name" entry that equals the username for the session
       making the request.  (If the "enable-external-groups" leaf is
       "true", add to these groups the set of groups provided by the
       transport layer.)	       */
    if (username == NULL)
	goto step9;
    /* User's group */
    if (xpath_vec(xnacm, nsc, "groups/group[user-name='%s']", &gvec, &glen, username) < 0)
	goto done;
    /* 4. If no groups are found, continue with step 9. */
    if (glen == 0)
	goto step9;
    /* 5. Process all rule-list entries, in the order they appear in the
        configuration.  If a rule-list's "group" leaf-list does not
        match any of the user's groups, proceed to the next rule-list
        entry. */
    if (xpath_vec(xnacm, nsc, "rule-list", &rlistvec, &rlistlen) < 0)
	goto done;
    /* First run through rules and cache rules as well as lookup objects in xt. 
     */
    if (nacm_datanode_prepare(h, xt, access, gvec, glen, rlistvec, rlistlen, nsc, &pv_list) < 0)
	goto done;
    /* Then recursivelyy traverse all requested nodes */
    if ((ret = nacm_datanode_write_recurse(h, xreq, pv_list,
					   strcmp(write_default, "deny"),
					   clicon_dbspec_yang(h),
					   cbret)) < 0)
	goto done;
    if (ret == 0) /* deny */
	goto deny;
    goto permit;
    /*  8.   At this point, no matching rule was found in any rule-list
	entry. */
 step9:   
    /* 10.  For a "write" access operation, if the requested data node is
        defined in a YANG module advertised in the server capabilities
        and the data definition statement contains a
        "nacm:default-deny-write" or a "nacm:default-deny-all"
        statement, then the access request is denied for the data node
        and all its descendants.
	XXX
    */
    /*12.  For a "write" access operation, if the "write-default" leaf is
        set to "permit", then permit the data node access request;
        otherwise, deny the request.*/
    /* write-default has default permit so should never be NULL */
    if (strcmp(write_default, "deny") == 0){
	if (netconf_access_denied(cbret, "application", "default deny") < 0)
	    goto done;
	goto deny;
    }
 permit:
    retval = 1;
 done:
    clicon_debug(1, "%s retval:%d (0:deny 1:permit)", __FUNCTION__, retval);
    if (pv_list)
	prepvec_free(pv_list);
    if (nsc)
	xml_nsctx_free(nsc);
    if (gvec)
	free(gvec);
    if (rlistvec)
	free(rlistvec);
    if (rvec)
	free(rvec);
    return retval;
 deny: /* Here, cbret must contain a netconf error msg */
    assert(cbuf_len(cbret));
    retval = 0;
    goto done;
}

/*---------------------------------------------------------------
 * Datanode read
 */

/*! Perform NACM action: mark if permit, del if deny
 * @param[in] xrule    NACM rule
 * @param[in] xn       XML node (requested node)
 * @retval    -1       Error
 * @retval    0        OK
 */
static int
nacm_data_read_action(cxobj *xrule,
		      cxobj *xn)
{
    int   retval = -1;
    char *action;

    if ((action = xml_find_body(xrule, "action")) != NULL){
	if (strcmp(action, "deny")==0)
	    xml_flag_set(xn, XML_FLAG_DEL);
	else if (strcmp(action, "permit")==0)
	    xml_flag_set(xn, XML_FLAG_MARK);
    }
    retval = 0;
    //done:
    return retval;
}

/*! Match specific rule to specific requested node
 * @param[in]  xn       XML node (requested node)
 * @param[in]  xrule    NACM rule
 * @param[in]  yspec    YANG spec
 * @retval -1  Error
 * @retval  0  OK and rule does not match
 * @retval  1  OK and rule matches
 * Two distinct cases:
 * (1) read_default is permit
 *     mark all deny rules and remove them
 * (2) read_default is deny:
 *     mark all permit rules and ancestors, remove everything else
 */
static int
nacm_data_read_xrule_xml(cxobj        *xn,
			 cxobj        *xrule,
			 clixon_xvec  *xpathvec,
			 yang_stmt    *yspec)
{
    int        retval = -1;
    yang_stmt *ymod;
    char      *module_pattern; /* rule module name */
    cxobj     *xp;
    int        i;
    
    if ((module_pattern = xml_find_body(xrule, "module-name")) == NULL)
	goto nomatch;
    /* 6a) The rule's "module-name" leaf is "*" or equals the name of
     * the YANG module where the requested data node is defined. 
     */
    if (strcmp(module_pattern, "*") != 0){
	if (ys_module_by_xml(yspec, xn, &ymod) < 0)
	    goto done;
	if (strcmp(yang_argument_get(ymod), module_pattern) != 0)
	    goto nomatch;
    }
    /*  6b) Either (1) the rule does not have a "rule-type" defined or
	(2) the "rule-type" is "data-node" and the "path" matches the
	requested data node, action node, or notification node. */    
    if (xml_find_type(xrule, NULL, "path", CX_ELMNT) == NULL){
	if (nacm_data_read_action(xrule, xn) < 0)
	    goto done;
	goto match;
    }
    for (i=0; i<clixon_xvec_len(xpathvec); i++){
	xp = clixon_xvec_i(xpathvec, i);
	/* Check if ancestor is xp (for every xpathvec?) */
	if (xn == xp || xml_isancestor(xn, xp)){
	    if (nacm_data_read_action(xrule, xn) < 0)
		goto done;
	    goto match;
	}
    }
 nomatch:
    retval = 0;
 done:
    return retval;
 match:
    retval = 1; /* match */
    goto done;
}

/*! Recursive check for NACM read rules among all XML nodes
 * @param[in]  h        Clicon handle
 * @param[in]  xn       XML node (requested node)
 * @param[in]  rulevec  Precomputed rules that apply to this user group
 * @param[in]  xpathvec Precomputed xpath results that apply to this XML tree
 * @param[in]  yspec    YANG spec
 * @retval  0  OK
 * @retval -1  Error
 */
static int
nacm_datanode_read_recurse(clicon_handle h,
			   cxobj        *xn,
			   prepvec      *pv_list,
			   yang_stmt    *yspec)
{
    int      retval = -1;
    cxobj   *x;
    cxobj   *xprev;
    int      ret;
    prepvec *pv;
    
    if (xml_spec(xn)){ /* Check this node */
	pv = pv_list;
	if (pv){
	    do {
		if ((ret = nacm_data_read_xrule_xml(xn,
						    pv->pv_xrule,
						    pv->pv_xpathvec,
						    yspec)) < 0) 
		    goto done;	    
		if (ret == 1)
		    break; /* stop at first match */		    
		pv = NEXTQ(prepvec *, pv);
	    } while (pv && pv != pv_list);
	}

#if 0 /* 6(A) in algorithm 
       * If N did not match any rule R, and default rule is deny, remove that subtree */
	if (strcmp(read_default, "deny") == 0)
	    if (xml_tree_prune_flagged_sub(xt, XML_FLAG_MARK, 1, NULL) < 0)
		goto done;
#endif
    }

    /* If node should be purged, dont recurse and defer removal to caller */
    if (xml_flag(xn, XML_FLAG_DEL) == 0){
	x = NULL; 	/* Recursively check XML */
	xprev = NULL;
	while ((x = xml_child_each(xn, x, CX_ELMNT)) != NULL) {
	    if (nacm_datanode_read_recurse(h, x, pv_list, yspec) < 0)
		goto done;
	    /* check for delayed remove */
	    if (xml_flag(x, XML_FLAG_DEL)){
		if (xml_purge(x) < 0)
		    goto done;
	    x = xprev;
	    }
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Make nacm datanode and module rule read access validation
 * Just purge nodes that fail validation (dont send netconf error message)
 * @param[in]  h        Clicon handle
 * @param[in]  xt       XML root tree with "config" label 
 * @param[in]  xrvec    Vector of requested nodes (sub-part of xt)
 * @param[in]  xrlen    Length of requsted node vector
 * @param[in]  username 
 * @param[in]  xnacm    NACM xml tree
 * @retval -1  Error
 * @retval  0  Not access and cbret set
 * @retval  1  Access
 * 3.2.4: <get> and <get-config> Operations
 * Data nodes to which the client does not have read access are silently
 * omitted, along with any descendants, from the <rpc-reply> message.
 * For NETCONF filtering purposes, the selection criteria are applied to the
 * subset of nodes that the user is authorized to read, not the entire datastore.
 * @note assume mode is internal or external, not disabled
 * @note There is unclarity on what "a data node" means wrt a read operation.
 * Suppose a tree is accessed. Is "the data node" just the top of the tree?
 * (1) Or is it all nodes, recursively, in the data-tree?
 * (2) Or is the datanode only the requested tree, NOT the whole datatree?
 * Example: 
 * - r0 default permit/deny *
 * - rule r1 to permit/deny /a
 * - rule r2 to permit/deny /a/b
 * - rule r3 to permit/deny /a/b/c
 * - rule r4 to permit/deny /d
 *
 * Some observations:
 * 1. The requested node is a set of nodes in a tree (not just the top-node)
 * 2. Any node descendants of a deny is denied (except default)
 * 3. First rule matching a node is the active rule
 * 
 * Algorithm:  Select either (A) or (B)
 *
 * 1. Select next node N in the requested node tree:
 *   2. Select next R rule in the set of applicable rules:
 *     3. If N does not match R, and remaining rules, goto 2.
 *     4. If N matches R as deny, remove that subtree
 *     5. If N matches R as accept, mark that node
 * 6(A). If N did not match any rule R, and default rule is deny, remove that subtree
 * 7. If remaining nodes, goto 1
 * 8(B) If default rule is deny, recursively remove all subtrees that are not marked
 *
 * @see RFC8341 3.4.5.  Data Node Access Validation
 * @see nacm_datanode_write
 * @see nacm_rpc
 */
int
nacm_datanode_read(clicon_handle h,
		   cxobj        *xt,
		   cxobj       **xrvec,
		   size_t        xrlen,    
		   char         *username,
		   cxobj        *xnacm)
{
    int             retval = -1;
    cxobj         **gvec = NULL; /* groups */
    size_t          glen;
    cxobj         **rlistvec = NULL; /* rule-list */
    size_t          rlistlen;
    int             i;
    char           *read_default = NULL;
    cvec           *nsc = NULL;
    prepvec        *pv_list = NULL;
    
    /* Create namespace context for with nacm namespace as default */
    if ((nsc = xml_nsctx_init(NULL, NACM_NS)) == NULL)
	goto done;
    /* 3.   Check all the "group" entries to see if any of them contain a
       "user-name" entry that equals the username for the session
       making the request.  (If the "enable-external-groups" leaf is
       "true", add to these groups the set of groups provided by the
       transport layer.)	       */
    if (username == NULL)
	goto step9;
    /* User's group */
    if (xpath_vec(xnacm, nsc, "groups/group[user-name='%s']", &gvec, &glen, username) < 0)
	goto done;
    /* 4. If no groups are found (glen=0), continue and check read-default 
          in step 11. */
    /* 5. Process all rule-list entries, in the order they appear in the
        configuration.  If a rule-list's "group" leaf-list does not
        match any of the user's groups, proceed to the next rule-list
        entry. */
    if (xpath_vec(xnacm, nsc, "rule-list", &rlistvec, &rlistlen) < 0)
	goto done;
    /* read-default has default permit so should never be NULL */
    if ((read_default = xml_find_body(xnacm, "read-default")) == NULL){
	clicon_err(OE_XML, EINVAL, "No nacm read-default rule");
	goto done;
    }
    /* First run through rules and cache rules as well as lookup objects in xt. 
     * DANGER: objects could be stale if they are removed?
     */
    if (nacm_datanode_prepare(h, xt, NACM_READ, gvec, glen, rlistvec, rlistlen, nsc, &pv_list) < 0)
	goto done;
    /* Then recursivelyy traverse all nodes */
    if (nacm_datanode_read_recurse(h, xt, pv_list, clicon_dbspec_yang(h)) < 0)
	goto done;
#if 1
    /* Step 8(B) above:
     * If default rule is deny, recursively remove all subtrees that are not marked
     */
    if (strcmp(read_default, "deny") == 0)
	if (xml_tree_prune_flagged_sub(xt, XML_FLAG_MARK, 1, NULL) < 0)
	    goto done;
#endif
    /* reset flag */
    if (xml_apply(xt, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)XML_FLAG_MARK) < 0)
	goto done;

    goto ok;
    /* 8.   At this point, no matching rule was found in any rule-list
       entry. */
 step9:
    /*    9.   For a "read" access operation, if the requested data node is
        defined in a YANG module advertised in the server capabilities
        and the data definition statement contains a
        "nacm:default-deny-all" statement, then the requested data node
        and all its descendants are not included in the reply.
    */
    for (i=0; i<xrlen; i++)     /* Loop through requested nodes, safe since vector not children */
	if (xml_purge(xrvec[i]) < 0)
	    goto done;
 ok:
    retval = 0;
 done:
    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    if (pv_list)
	prepvec_free(pv_list);
    if (nsc)
	xml_nsctx_free(nsc);
    if (gvec)
	free(gvec);
    if (rlistvec)
	free(rlistvec);
    return retval;
}


/*---------------------------------------------------------------
 * NACM pre-procesing
 */

/*! NACM intial pre- access control enforcements 
 * Initial NACM steps and common to all NACM access validation.
 * If retval=0 continue with next NACM step, eg rpc, module, 
 * etc. If retval = 1 access is OK and skip next NACM step.
 * @param[in]  h        Clicon handle
 * @param[in]  xnacm    NACM XML tree, root should be "nacm"
 * @param[in]  username User name of requestor
 * @retval -1  Error
 * @retval  0  OK but not validated. Need to do NACM step using xnacm
 * @retval  1  OK permitted. You do not need to do next NACM step
 * @code
 *   if ((ret = nacm_access(h, mode, xnacm, username)) < 0)
 *     err;
 *   if (ret == 0){
 *      // Next step NACM processing
 *      xml_free(xnacm);
 *   }
 * @endcode
 * @see RFC8341 3.4 Access Control Enforcement Procedures
 */
static int
nacm_access(clicon_handle h,
	    cxobj        *xnacm,
	    char         *username)
{
    int     retval = -1;
    char   *enabled;
    cxobj  *x;
    cvec  *nsc = NULL;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((nsc = xml_nsctx_init(NULL, NACM_NS)) == NULL)
	goto done;
    /* Do initial nacm processing common to all access validation in
     * RFC8341 3.4 */
    /* 1.   If the "enable-nacm" leaf is set to "false", then the protocol
       operation is permitted. */
    if ((x = xpath_first(xnacm, nsc, "enable-nacm")) == NULL)
	goto permit;
    enabled = xml_body(x);
    if (strcmp(enabled, "true") != 0)
	goto permit;
    /* 2.   If the requesting session is identified as a recovery session,
       then the protocol operation is permitted. NYI */
    if (username && strcmp(username, clicon_nacm_recovery_user(h)) == 0)
	goto permit;

    retval = 0; /* not permitted yet. continue with next NACM step */
 done:
    if (nsc)
	xml_nsctx_free(nsc);
    clicon_debug(1, "%s retval:%d (0:deny 1:permit)", __FUNCTION__, retval);
    return retval;
 permit:
    retval = 1;
    goto done;
}

/*! NACM intial pre- access control enforcements 
 * Initial NACM steps and common to all NACM access validation.
 * If retval=0 continue with next NACM step, eg rpc, module, 
 * etc. If retval = 1 access is OK and skip next NACM step.
 * @param[in]  h        Clicon handle
 * @param[in]  username User name of requestor
 * @param[out] xncam    NACM XML tree, set if retval=0. Free after use
 * @retval -1  Error
 * @retval  0  OK but not validated. Need to do NACM step using xnacm
 * @retval  1  OK permitted. You do not need to do next NACM step
 * @code
 *   cxobj *xnacm = NULL;
 *   if ((ret = nacm_access_pre(h, username, &xnacm)) < 0)
 *     err;
 *   if (ret == 0){
 *      // Next step NACM processing
 *      xml_free(xnacm);
 *   }
 * @endcode
 * @see RFC8341 3.4 Access Control Enforcement Procedures
 */
int
nacm_access_pre(clicon_handle  h,
		char          *username,
		cxobj        **xnacmp)
{
    int    retval = -1;
    char  *mode;
    cxobj *x;
    cxobj *xnacm0 = NULL;
    cxobj *xnacm = NULL;
    cvec  *nsc = NULL;
    
    mode = clicon_option_str(h, "CLICON_NACM_MODE");
    if (mode == NULL)
	goto permit;
    else if (strcmp(mode, "disabled")==0)
	goto permit;
    else if (strcmp(mode, "external")==0){
	if ((x = clicon_nacm_ext(h)))
	    if ((xnacm0 = xml_dup(x)) == NULL)
		goto done;
    }
    else if (strcmp(mode, "internal")==0){
	if (xmldb_get0(h, "running", YB_MODULE, nsc, "nacm", 1, &xnacm0, NULL) < 0)
	    goto done;
    }
    else{
	clicon_err(OE_XML, 0, "Invalid NACM mode: %s", mode);
	goto done;
    }
    
    if ((nsc = xml_nsctx_init(NULL, NACM_NS)) == NULL)
	goto done;
    /* If config does not exist then the operation is permitted(?) */
    if (xnacm0 == NULL)
	goto permit;
    /* If config does not exist then the operation is permitted(?) */
    if ((xnacm = xpath_first(xnacm0, nsc, "nacm")) == NULL)
	goto permit;
    if (xml_rootchild_node(xnacm0, xnacm) < 0)
	goto done;
    xnacm0 = NULL;
    /* Initial NACM steps and common to all NACM access validation. */
    if ((retval = nacm_access(h, xnacm, username)) < 0)
	goto done;
    if (retval == 0){ /* if retval == 0 then return an xml nacm tree */
	*xnacmp = xnacm;
	xnacm = NULL;
    }
 done:
    if (nsc)
	xml_nsctx_free(nsc);
    if (xnacm0)
	xml_free(xnacm0);
    else if (xnacm)
	xml_free(xnacm);
    return retval;
 permit:
    retval = 1;
    goto done;
}

