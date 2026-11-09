/* <regex.h> for the Code Editor's files built on the host (tests/host/test_wcode.c, 11/05/26): libsurf's regular
   expressions under the surf_ names of tests/host/text_names.h, with which user/libsurf/regex.c is built for the host,
   so that the editor searches as it does on SurfOS (its offsets, REG_STARTEND), whatever the host's own are. */
#ifndef WCODE_HOST_REGEX_H
#define WCODE_HOST_REGEX_H

#define regcomp  surf_regcomp
#define regexec  surf_regexec
#define regerror surf_regerror
#define regfree  surf_regfree
#undef RE_DUP_MAX                       /* the host's limits.h has its own */
#include "../../../user/include/regex.h"

#endif
