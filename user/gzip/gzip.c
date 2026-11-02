/*
SurfOS user program: gzip
--------------------
File: gzip.c    Date: 11/01/26
--------------------
Compresses files: tool.c has it all.
*/

#include "tool.h"

int main(int argc, char **argv) {
    return gz_tool(argc, argv, "gzip", 0, 0);
}
