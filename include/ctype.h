/*
BLIBC Character Types
----------------------
File: ctype.h   Date: Prior to 4/23/04
----------------------
(C)2004 Brandon Burr
*/
//Rebuilt 9/28/2026 - not in the printout, see REBUILD-NOTES.md

#ifndef _CTYPE_H
#define _CTYPE_H

#include <surfos/types.h>

//character class bits, the _ctype[] table in lib/blibc/ctype.c is built from these
#define _U  0x01 //upper case
#define _L  0x02 //lower case
#define _D  0x04 //digit
#define _C  0x08 //control
#define _P  0x10 //punctuation
#define _S  0x20 //white space (space/lf/tab)
#define _X  0x40 //hex digit
#define _SP 0x80 //hard space (0x20)

extern u_char _ctype[];

//one table entry per character 0-255 (there is no EOF slot at the front)
#define _ismask(c) (_ctype[(u_char)(c)])

#define isalnum(c)  ((_ismask(c) & (_U|_L|_D)) != 0)
#define isalpha(c)  ((_ismask(c) & (_U|_L)) != 0)
#define iscntrl(c)  ((_ismask(c) & (_C)) != 0)
#define isdigit(c)  ((_ismask(c) & (_D)) != 0)
#define isgraph(c)  ((_ismask(c) & (_P|_U|_L|_D)) != 0)
#define islower(c)  ((_ismask(c) & (_L)) != 0)
#define isprint(c)  ((_ismask(c) & (_P|_U|_L|_D|_SP)) != 0)
#define ispunct(c)  ((_ismask(c) & (_P)) != 0)
#define isspace(c)  ((_ismask(c) & (_S)) != 0)
#define isupper(c)  ((_ismask(c) & (_U)) != 0)
#define isxdigit(c) ((_ismask(c) & (_D|_X)) != 0)

#define isascii(c) (((u_char)(c)) <= 0x7F)
#define toascii(c) (((u_char)(c)) & 0x7F)

#define tolower(c) (isupper(c) ? ((c) - 'A' + 'a') : (c))
#define toupper(c) (islower(c) ? ((c) - 'a' + 'A') : (c))

#endif
