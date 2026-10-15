/*
SurfOS Touchpad Command
--------------------
File: touchpad.c Date: 10/12/26
--------------------
The input task owns activation; these commands never perform bus transfers.
*/
#include <stdio.h>
#include <string.h>
#include <sys/touchpad.h>
#include "shell.h"

int cmd_touchpad(int argc, char **argv) {
    int result;
    if(argc == 1 || (argc == 2 && !strcmp(argv[1], "info"))) {
        touchpad_print(); return CMD_OK;
    }
    if(argc == 2 && !strcmp(argv[1], "up")) {
        result = touchpad_start();
        printf("    touchpad: %s\n", result < 0 ? "not present on this platform" :
               result == 2 ? "restart requested; touchpad info shows progress" :
               result == 1 ? "activation requested; touchpad info shows progress" :
               "a start is already running; touchpad info shows progress");
        return result < 0 ? CMD_FAIL : CMD_OK;
    }
    if(argc == 2 && !strcmp(argv[1], "report")) {
        touchpad_report_print(); return CMD_OK;
    }
    if(argc == 2 && !strcmp(argv[1], "descriptor")) {
        touchpad_descriptor_print(); return CMD_OK;
    }
    printf("    usage: touchpad [info|up|report|descriptor]\n");
    return CMD_USAGE;
}
