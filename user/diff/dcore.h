/*
SurfOS user programs: which lines of two sequences changed
----------------------
File: dcore.h   Date: 11/01/26
----------------------
Myers' O(ND) algorithm in its linear-space form (E. Myers, "An O(ND) Difference Algorithm and Its Variations", 1986, section
4b). A sequence is numbers: diff's lines as hashes. Pure: no system calls.

The shortest edit is searched for from both corners of a box of lines at once, one edit at a time, until the two searches
meet; that point splits the box, and each part is done the same way. Time goes with the size of the difference, not of
the files.

The search is capped: after SEARCH_CAP edits from each corner of a box it stops, and the box is cut by what the two searches
found (dcore.c says how). The result is still a correct edit, not always the shortest. Until the cap is reached the edit is
the shortest there is. Memory is two arrays of 2 * cap + 3 ints, whatever the size of the input; time is proportional
to the cap times the lines. Boxes wait on a stack of a fixed size, the smallest part of each cut being done first, so that
no input can make it deep.
*/

#ifndef _DCORE_H
#define _DCORE_H

/* a[i] and b[j] the sequences, ma and mb bytes for each, zero at the start: those not in the common
   subsequence are set to 1. 0, or -1 when there is no memory for the search */
int dc_compare(const unsigned *a, unsigned n, const unsigned *b, unsigned m, unsigned char *ma, unsigned char *mb);
/* the same with the cap given: tests/host/test_diff.c drives both sides of it */
int dc_compare_limit(const unsigned *a, unsigned n, const unsigned *b, unsigned m, unsigned char *ma, unsigned char *mb,
                     unsigned limit);

#endif
