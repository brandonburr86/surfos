/* Pure HID-over-I2C 1.00 framing; no transport, allocation or hardware access. */
#ifndef _SYS_I2CHID_H
#define _SYS_I2CHID_H

#include <surfos/types.h>

#define I2CHID_DESCRIPTOR_LENGTH 30u
#define I2CHID_MAX_REPORT_DESCRIPTOR 4096u
#define I2CHID_MAX_INPUT 256u

struct i2chid_desc {
    u16 report_desc_len, report_desc_reg;
    u16 input_reg, max_input_len;
    u16 output_reg, max_output_len;
    u16 command_reg, data_reg;
    u16 vendor_id, product_id, version_id;
};

/* Accepts the bounded SurfOS profile: version 1.00, 1..4096 report descriptor
   bytes, 2..256 input bytes, and nonzero report/input/command/data registers.
   Output-register support is optional. Reserved descriptor bytes are ignored.
   On failure, *out is unchanged. Extra bytes after the descriptor are ignored. */
bool i2chid_parse_descriptor(const u8 *bytes, u_int available,
                             struct i2chid_desc *out);

enum i2chid_power { I2CHID_POWER_ON = 0, I2CHID_POWER_SLEEP = 1 };
enum i2chid_report_type {
    I2CHID_REPORT_INPUT = 1, I2CHID_REPORT_OUTPUT = 2, I2CHID_REPORT_FEATURE = 3
};

/* Builders return the encoded byte count, or -1 without changing out. The
   caller sends exactly that many bytes. No address byte or read is included.
   report_type is 1..3 and report_id is 0..255; zero means unnumbered.
   SET_REPORT payload excludes the report ID; the helper inserts nonzero IDs
   and the two-byte total report length. Its payload must not overlap out.
   max_output_len applies to the optional output register, not SET_REPORT.
   The caller chooses device-supported commands and report types. */
int i2chid_set_power(const struct i2chid_desc *desc, u_int state,
                     u8 *out, u_int capacity);
int i2chid_reset(const struct i2chid_desc *desc, u8 *out, u_int capacity);
int i2chid_get_report(const struct i2chid_desc *desc, u_int report_type,
                      u_int report_id, u8 *out, u_int capacity);
int i2chid_set_report(const struct i2chid_desc *desc, u_int report_type,
                      u_int report_id, const u8 *payload, u_int payload_len,
                      u8 *out, u_int capacity);

enum i2chid_input_result {
    I2CHID_INPUT_INVALID = -1, I2CHID_INPUT_RESET = 0,
    I2CHID_INPUT_EMPTY = 1, I2CHID_INPUT_DATA = 2
};

/* Validates the two-byte total length against received and max_input_len
   (2..256). Trailing transport padding is ignored. DATA returns a borrowed
   slice including any report ID. All other results clear the output slice.
   RESET is a framing result; the caller decides whether a reset was pending. */
enum i2chid_input_result i2chid_get_input(const u8 *bytes, u_int received,
                                         u_int max_input_len,
                                         const u8 **payload, u_int *payload_len);

#endif
