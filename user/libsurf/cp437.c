/*
SurfOS user library: the console's character set
--------------------
File: cp437.c   Date: 10/11/26
--------------------
Code page 437 as the console draws it (include/sys/cp437.h), built into libsurf so that
a program can tell which characters the console shows: Undertow draws the others as
ASCII stand-ins rather than as '?'. The kernel builds the same file, driver/cp437.c;
programs declare what they use through surfos/cp437.h.
*/

#include "../../driver/cp437.c"
