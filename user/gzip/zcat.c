/*
SurfOS user program: zcat
--------------------
File: zcat.c    Date: 11/01/26
--------------------
Decompresses files to standard output: tool.c has it all.
*/

#include "tool.h"

int main(int argc, char **argv) {
    return gz_tool(argc, argv, "zcat", 1, 1);
}
