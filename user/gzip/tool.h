/*
SurfOS user programs: gzip, gunzip and zcat
----------------------
File: tool.h    Date: 11/01/26
----------------------
The three programs are this one function (tool.c): prog names the messages, decompress and
to_stdout are what the program does before its options say anything. The status it returns is the
program's.
*/

#ifndef _GZTOOL_H
#define _GZTOOL_H

int gz_tool(int argc, char **argv, const char *prog, int decompress, int to_stdout);

#endif
