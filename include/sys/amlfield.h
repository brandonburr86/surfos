/*
SurfOS AML Field Layout Reader
--------------------
File: amlfield.h Date: 10/12/26
--------------------
Static declarations only: no AML execution, allocation, or physical reads.
*/
#ifndef _SYS_AMLFIELD_H
#define _SYS_AMLFIELD_H
#include <surfos/types.h>

#define AML_FIELD_MAX_TABLE (1024U * 1024U)
struct aml_field {
    u64 address;
    u32 region_length;
    u32 bit_offset;
    u32 bit_width;
};
enum aml_field_result {
    AML_FIELD_OK = 0,
    AML_FIELD_INVALID = -1,
    AML_FIELD_NOT_FOUND = -2,
    AML_FIELD_AMBIGUOUS = -3,
    AML_FIELD_UNSUPPORTED = -4
};
/* Full, checksum-validated DSDT/SSDT input, including its 36-byte header.
   Names are exactly four NameSeg bytes. Only unconditional SystemMemory
   declarations are returned. Literal integers or unique same-scope literal
   Name constants may specify a region. Such names must occur only in their
   declaration and this region's operands; even opaque mentions are refused.
   Failure clears the result. */
int aml_field_find(const u8 *table, u32 bytes, const char region[4],
                   const char field[4], struct aml_field *out);
/* 10/25/26 (robustness, L20): the SLP_TYP values for PM1a and PM1b of a sleep state
   (name "_S5_": soft off), from its Name at the root of the namespace, declared once,
   outside any If, Else or While: Package { SLP_TYPa, SLP_TYPb, ... } of integer
   constants. Same input as above; failure leaves both 0. */
int aml_sleep_type(const u8 *table, u32 bytes, const char name[4], u8 *typa, u8 *typb);
#endif
