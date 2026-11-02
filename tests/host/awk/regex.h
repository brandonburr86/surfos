/* For awk's files on the host: libsurf's regular expressions, under the surf_ names of tests/host/text_names.h, which
   have the offsets and REG_STARTEND that awk counts on. */
#define regcomp  surf_regcomp
#define regexec  surf_regexec
#define regerror surf_regerror
#define regfree  surf_regfree
#include "../../../user/include/regex.h"

/* the test counts what awk gives regexec() to search */
int counted_regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t *pmatch, int eflags);
#undef regexec
#define regexec counted_regexec
