/*
SurfOS HID Pointer Reports
--------------------
File: hidpointer.h Date: 10/12/26
--------------------
Bounded report-descriptor parsing, independent of USB/I2C framing.
10/26/26 (stage C8 of docs/superpowers/plans/2026-10-25-app-windows.md): a report's wheel, the first
relative Generic Desktop Wheel field that can be bound, of the collection of the report's pointer
fields (where a USB boot mouse's fourth byte is one: usbmouse.h). It never changes what the parse
returns, and the decoder leaves it alone.
*/
#ifndef _SYS_HIDPOINTER_H
#define _SYS_HIDPOINTER_H
#include <surfos/types.h>

#define HID_POINTER_REPORTS 16
#define HID_POINTER_MAX_BITS 8192
#define HID_POINTER_MAX_DESCRIPTOR 16384

enum hid_pointer_error {
    HID_POINTER_OK, HID_POINTER_BAD_ARG, HID_POINTER_MALFORMED,
    HID_POINTER_LIMIT, HID_POINTER_UNSUPPORTED, HID_POINTER_UNRECOGNIZED,
    HID_POINTER_SHORT, HID_POINTER_RANGE
};
struct hid_pointer_field {
    u32 bit_offset;                  /* In the payload, excluding Report ID. */
    u8 bit_size;                     /* Zero means absent; otherwise 1..32. */
    i64 minimum, maximum;            /* Negative minimum means signed data. */
    bool null_state;                 /* Out-of-range input means "no value". */
};
struct hid_pointer_report {
    u8 id;
    u32 input_bits, output_bits, feature_bits;
    struct hid_pointer_field x, y, button[3];
    struct hid_pointer_field wheel;  /* 10/26/26: Generic Desktop Wheel (0x01:0x38), relative; a second skipped */
};
struct hid_pointer_mode {
    u8 report_id;
    u_int report_bytes;              /* Complete Feature report, including ID. */
    struct hid_pointer_field field;  /* Digitizer InputMode, usage 0x0d:0x52. */
    bool exclusive;                  /* The report's only Data field; rest padding. */
};
struct hid_pointer_layout {
    bool valid, numbered, has_pointer;
    u_int count;
    struct hid_pointer_report report[HID_POINTER_REPORTS];
    struct hid_pointer_mode mode;
};
struct hid_pointer_sample {
    int dx, dy;
    unsigned buttons, button_mask;
};

/* Failures leave output objects and report buffers unchanged. The parser does
   not support long items, alternative usage sets, or ambiguous pointer fields.
   The caller must serialize its own device/button state. */
enum hid_pointer_error hid_pointer_parse(const u8 *descriptor, u_int length,
                                         struct hid_pointer_layout *out);
/* Buffer starts with Report ID only when layout.numbered, with no I2C length
   prefix. Extra transport padding is ignored. Merge button state as
   (previous & ~sample.button_mask) | sample.buttons; absent buttons are unknown.
   10/15/26: a value outside its logical range is clamped to it, or means no
   movement for a null-state field, as Linux's hid-input and Windows do. */
enum hid_pointer_error hid_pointer_decode(const struct hid_pointer_layout *layout,
                                          const u8 *report, u_int length,
                                          struct hid_pointer_sample *out);
enum hid_pointer_error hid_pointer_extract(const struct hid_pointer_field *field,
                                           const u8 *payload, u_int length, i64 *out);
enum hid_pointer_error hid_pointer_patch(const struct hid_pointer_field *field,
                                         u8 *payload, u_int length, i64 value);
/* GET_REPORT supplies the complete Feature buffer. Set changes only InputMode;
   mode 0 selects mouse operation, subject to the descriptor's logical bounds. */
enum hid_pointer_error hid_pointer_get_mode(const struct hid_pointer_layout *layout,
                                            const u8 *report, u_int length, i64 *out);
enum hid_pointer_error hid_pointer_set_mode(const struct hid_pointer_layout *layout,
                                            u8 *report, u_int length, i64 mode);
/* 10/15/26: SET_REPORT without GET_REPORT, which some devices refuse. Builds the
   complete Feature report (report_bytes, with its ID when numbered) when mode.exclusive:
   constant padding is zero, as a host's own report would carry it. A report shared
   with other Data fields is UNSUPPORTED (read it, then hid_pointer_set_mode). */
enum hid_pointer_error hid_pointer_mode_report(const struct hid_pointer_layout *layout,
                                               u8 *report, u_int length, i64 mode);
#endif
