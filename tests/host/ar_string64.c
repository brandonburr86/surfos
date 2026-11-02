/* libsurf's string.c for the Mac's native sanitizer build (11/01/26, tests/archive-sanitize.sh): it copies a word at a time
   and takes a long for four bytes, which is so on SurfOS and not on a 64-bit host. Its headers come first, so that only
   its own code sees the long below. */
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#define long int
#include "../../user/libsurf/string.c"
