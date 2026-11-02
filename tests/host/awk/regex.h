/* For awk's files on the host: libsurf's regular expressions, under the surf_ names of tests/host/text_names.h, which
   have the offsets and REG_STARTEND that awk counts on. */
#define regcomp  surf_regcomp
#define regexec  surf_regexec
#define regerror surf_regerror
#define regfree  surf_regfree
#include "../../../user/include/regex.h"
