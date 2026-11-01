/*
SurfOS user programs: names from archives
----------------------
File: arpath.h  Date: 11/01/26
----------------------
tar and unzip take their file names from files anyone may have made. These functions make no
system calls, so the rules are tested on their own (tests/host/test_tar.c).

ar_safe_path() is the only way a name from an archive becomes a path: it refuses what is absolute
or leaves the folder, and what FAT could not hold anyway, with a reason for the message.
ar_show_name() is the only way one reaches the terminal: control characters in a name would
otherwise be commands to it. ar_glob() is --exclude's matching and unzip's: * and ?, the case of
ASCII letters ignored as FAT ignores it, a ? taking a whole UTF-8 sequence.
*/

#ifndef _ARPATH_H
#define _ARPATH_H

#include <stddef.h>

/* name as a relative path in out, without "." or repeated slashes: NULL, or why it is refused. out is ""
   for the archive's top, "." and "./" (nothing to make). */
const char *ar_safe_path(const char *name, char *out, size_t size);

/* n bytes at s are well-formed UTF-8: the rules of fs/fat.c, which refuses any other name */
int ar_utf8_valid(const char *s, size_t n);

/* name for printing, into out: control characters, invalid UTF-8 and what follows the room as \ooo and ... */
void ar_show_name(const char *name, char *out, size_t size);

/* does the pattern match the whole of s? */
int ar_glob(const char *pattern, const char *s);

/* does the pattern match s from the start of one of its components up to the end of one? As GNU tar's --exclude:
   "tmp" matches "a/tmp/b", and "*.o" matches "a/b.o". */
int ar_glob_part(const char *pattern, const char *s);

/* is member path itself or inside it? Ignoring case. A "./" at path's start and "/"s at its end do not count. */
int ar_under(const char *member, const char *path);

#endif
