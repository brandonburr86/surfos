/*
SurfOS ACPI Definition Table Export
--------------------
File: acpidump.c Date: 10/12/26
--------------------
Read-only DSDT/SSDT and bounded NVS bytes for diagnosis. No AML evaluation.
*/
#include <surfos/acpi.h>
#include <surfos/task.h>
#include <mm/kalloc.h>
#include <stdio.h>
#include <string.h>
#include "shell.h"

static int print_bytes(const u8 *data, u32 length) {
    u32 offset, j;
    for(offset = 0; offset < length && !task_interrupted(); offset += 16) {
        printf("%08x:", offset);
        for(j = 0; j < 16 && offset + j < length; j++) printf(" %02x", data[offset + j]);
        printf("\n");
    }
    return task_interrupted() ? CMD_FAIL : CMD_OK;
}

int cmd_acpidump(int argc, char **argv) {
    u8 *data;
    u32 length;
    u_int instance = 0;
    int result;
    if(argc < 2 || argc > 3 || (strcmp(argv[1], "DSDT") && strcmp(argv[1], "SSDT"))) goto usage;
    if(argc == 3) {
        const char *p = argv[2];
        if(!*p) goto usage;
        for(; *p; p++) {
            if(*p < '0' || *p > '9' || instance > (0xffffffffU - (*p - '0')) / 10) goto usage;
            instance = instance * 10 + *p - '0';
        }
    }
    if(!strcmp(argv[1], "DSDT") && instance) goto usage;
    if(task_interrupted()) return CMD_FAIL;
    result = acpi_aml_copy(argv[1], instance, &data, &length);
    if(result != ACPI_COPY_OK) {
        printf("    acpidump: %s instance %u: %s\n", argv[1], instance,
               result == ACPI_COPY_ABSENT ? "not found (acpi lists the available tables)" :
               result == ACPI_COPY_NOMEM ? "out of memory" : "invalid or unmappable firmware table");
        return CMD_FAIL;
    }
    printf("ACPI %s %u bytes\n", argv[1], length);
    result = print_bytes(data, length);
    kfree(data);
    return result;
usage:
    printf("    usage: acpidump DSDT | acpidump SSDT [zero-based instance]\n");
    return CMD_USAGE;
}

int cmd_acpinvs(int argc, char **argv) {
    u8 data[ACPI_NVS_MAX_BYTES];
    u64 address = 0;
    u32 length = 0, digit;
    const char *p;
    if(argc != 3) goto usage;
    p = argv[1];
    if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    if(!*p) goto usage;
    for(; *p; p++) {
        if(*p >= '0' && *p <= '9') digit = *p - '0';
        else if(*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if(*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else goto usage;
        if(address > (0xffffffffffffffffULL - digit) / 16) goto usage;
        address = address * 16 + digit;
    }
    p = argv[2];
    if(!*p) goto usage;
    for(; *p; p++) {
        if(*p < '0' || *p > '9') goto usage;
        digit = *p - '0';
        if(length > (ACPI_NVS_MAX_BYTES - digit) / 10) goto usage;
        length = length * 10 + digit;
    }
    if(!length) goto usage;
    if(task_interrupted()) return CMD_FAIL;
    if(acpi_nvs_copy(address, length, data) != ACPI_COPY_OK) {
        printf("    acpinvs: refused; range must be ACPI NVS below 4 GiB in profile=laptop\n");
        return CMD_FAIL;
    }
    printf("ACPI NVS %u bytes\n", length);
    return print_bytes(data, length);
usage:
    printf("    usage: acpinvs <hex-physical-address> <decimal-bytes 1..256>\n");
    return CMD_USAGE;
}
