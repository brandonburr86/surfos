/*
SurfOS user program: gunzip
--------------------
File: gunzip.c  Date: 11/01/26
--------------------
Decompresses files: tool.c has it all.
*/

#include "tool.h"

int main(int argc, char **argv) {
    return gz_tool(argc, argv, "gunzip", 1, 0);
}
