/* <regex.h> for a program of user/bin built for the host (surf.h, beside this): the library's regular expressions, not the host's */
#ifndef TOOLHOST_REGEX_H
#define TOOLHOST_REGEX_H

#define regcomp  surf_regcomp
#define regexec  surf_regexec
#define regerror surf_regerror
#define regfree  surf_regfree
#undef RE_DUP_MAX                       /* the host's limits.h has its own */
#include "../../../user/include/regex.h"

#endif
