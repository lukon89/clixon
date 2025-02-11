# Contributing clixon code

The clixon project welcomes contributions from the community.

## Licensing

A contribution must follow the [CLIXON
licensing](https://github.com/clicon/clixon/blob/master/LICENSE.md)
with the dual licensing: either Apache License, Version 2.0 or
GNU General Public License Version 3.

Note especially, the contribution license agreement (CLA) is described in the CLA section of the Apache License, Version 2.0.

## C style

Clixon uses 4-char indentation, a la emacs "cc-mode".

### Function declarations

Functions in C code are written as follows:
```
  static int
  myfn(int           par1,
       my_structure *par2)
  {
      int           retval = -1;
      my_structure *ms;

      ms = NULL;
```

Notes:

1. The return type of the function and all qualifers on first line (`static int`)
2. Function name and first parameter on second line, thereafter each parameter on own line
3. Each parameter indented to match the "longest" (`my_structure`)
4. Pointer declarations written: `type *p`, not: `type* p`.
5. All local variables in a function declared at top of function, not inline with C-statements.
6. Local variables can be initialized with scalars or constants, not eg malloc or functions with return values that need to be  checked for errors
7. There is a single empty line between local variable declarations and the first function statement.

Function signatures are declared in include files or in forward declaration using "one-line" syntax, unless very long:
```
  static int myfn(int par1, my_structure *par2);
```

### Errors

Errors are typically declared as follows:
```
    if (myfn(0) < 0){
       clicon_err(OE_UNIX, EINVAL, "myfn");
       goto done;
    }
```

All function returns that have return values must be checked

Default return values form a function are:

- `0`  OK
- `-1` Fatal Error

In some cases, Clixon uses three-value returns as follows:

- `1`  OK
- `0`  Invalid (and usually a cxobj* return value)
- `-1` Fatal error

### Return values

Clixon uses goto:s only to get a single point of exit functions as follows::
```
  {
      int retval = -1;
  
      ...
      retval = 0;
    done:
      return retval
  }
```

Notes:

1. Use only a single return statement in a function
2. Do not use of goto:s in other ways

### Comments

Use `/* */`. Use `//` only for temporal comments.

Do not use `======`, `>>>>>` or `<<<<<<` in comments since git merge conflict uses that.

## Documentation

How to document the code:
```
  /*! This is a small comment on one line
   *
   * This is a detailed description
   * spanning several lines.
   *
   * @param[in]     src     This is a description of the first parameter
   * @param[in,out] dest    This is a description of the second parameter
   * @retval        0       This is a description of the return value
   * @retval       -1       This is a description of another return value
   *
   * Example usage:
   * @code
   *   fn(a, &b);
   * @endcode
   *
   * @see                   See also this function
   */
```

## Testing

For a new feature, it is important to write (or extend) [a clixon test](https://github.com/clicon/clixon/blob/master/test/README.md), including some functionality tests and preferably some negative tests. Tests are then run automatically as regression on commit [by github actions](https://github.com/clicon/clixon/actions/).

These tests are also the basis for more extensive CI tests run by the project which
include:
- [Memory tests](https://github.com/clicon/clixon/tree/master/test#memory-leak-test), using Valgrind. Running the .mem.sh for cli, backend,netconf and restconf is mandatory.
- [Vagrant tests on other operating systems](https://github.com/clicon/clixon/tree/master/test/vagrant). Other OS:s include ubuntu, centos and freebsd
- [CI on other platforms](https://github.com/clicon/clixon/tree/master/test/cicd). Other platforms include x86-64, 32-bit i686, and armv71
- [Coverage tests](https://app.codecov.io/gh/clicon/clixon)
- [Fuzzing](https://github.com/clicon/clixon/tree/master/test/fuzz) Fuzzing are run occasionally using AFL
