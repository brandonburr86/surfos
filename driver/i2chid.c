/* SurfOS HID-over-I2C wire helpers, 10/2026.
   Protocol layout references (this implementation is original):
   https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/required-hid-descriptors
   https://github.com/torvalds/linux/blob/v6.12/drivers/hid/i2c-hid/i2c-hid-core.c
   All accesses are bytewise: descriptor and extended command fields may be
   unaligned. No mutable global state or transport side effects. */
#include <sys/i2chid.h>

static u16 le16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}
static void put16(u8 *p, u16 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

bool i2chid_parse_descriptor(const u8 *bytes, u_int available,
                             struct i2chid_desc *out) {
    struct i2chid_desc d;
    if(!bytes || !out || available < I2CHID_DESCRIPTOR_LENGTH ||
       le16(bytes) != I2CHID_DESCRIPTOR_LENGTH || le16(bytes + 2) != 0x0100)
        return false;
    d.report_desc_len = le16(bytes + 4);
    d.report_desc_reg = le16(bytes + 6);
    d.input_reg = le16(bytes + 8);
    d.max_input_len = le16(bytes + 10);
    d.output_reg = le16(bytes + 12);
    d.max_output_len = le16(bytes + 14);
    d.command_reg = le16(bytes + 16);
    d.data_reg = le16(bytes + 18);
    d.vendor_id = le16(bytes + 20);
    d.product_id = le16(bytes + 22);
    d.version_id = le16(bytes + 24);
    if(!d.report_desc_len || d.report_desc_len > I2CHID_MAX_REPORT_DESCRIPTOR ||
       d.max_input_len < 2 || d.max_input_len > I2CHID_MAX_INPUT ||
       !d.report_desc_reg || !d.input_reg || !d.command_reg || !d.data_reg)
        return false;
    *out = d;
    return true;
}

static int fixed_command(const struct i2chid_desc *desc, u8 opcode, u8 value,
                          u8 *out, u_int capacity) {
    if(!desc || !desc->command_reg || !out || capacity < 4) return -1;
    put16(out, desc->command_reg);
    out[2] = value;
    out[3] = opcode;
    return 4;
}
int i2chid_set_power(const struct i2chid_desc *desc, u_int state,
                     u8 *out, u_int capacity) {
    if(state != I2CHID_POWER_ON && state != I2CHID_POWER_SLEEP) return -1;
    return fixed_command(desc, 0x08, (u8)state, out, capacity);
}
int i2chid_reset(const struct i2chid_desc *desc, u8 *out, u_int capacity) {
    return fixed_command(desc, 0x01, 0, out, capacity);
}

static bool report_args(const struct i2chid_desc *desc, u_int type, u_int id,
                         const u8 *out) {
    return desc && desc->command_reg && desc->data_reg && out &&
           type >= I2CHID_REPORT_INPUT && type <= I2CHID_REPORT_FEATURE && id <= 255;
}

/* Arguments and whole output capacity are checked before entering here. */
static u_int report_command(const struct i2chid_desc *desc, u_int type, u_int id,
                             u8 opcode, u8 *out) {
    u_int position = 4;
    put16(out, desc->command_reg);
    out[2] = (u8)((type << 4) | (id >= 15 ? 15 : id));
    out[3] = opcode;
    if(id >= 15) out[position++] = (u8)id;
    put16(out + position, desc->data_reg);
    return position + 2;
}

int i2chid_get_report(const struct i2chid_desc *desc, u_int report_type,
                      u_int report_id, u8 *out, u_int capacity) {
    u_int length = report_id >= 15 ? 7 : 6;
    if(!report_args(desc, report_type, report_id, out) || capacity < length) return -1;
    return (int)report_command(desc, report_type, report_id, 0x02, out);
}
int i2chid_set_report(const struct i2chid_desc *desc, u_int report_type,
                      u_int report_id, const u8 *payload, u_int payload_len,
                      u8 *out, u_int capacity) {
    u_int report_len, total, position, i;
    if(!report_args(desc, report_type, report_id, out) || (!payload && payload_len))
        return -1;
    /* The report's LE16 length includes itself and any nonzero report ID,
       but excludes the command and both register addresses. Check before add. */
    if(payload_len > 65535u - 2u - (report_id != 0)) return -1;
    report_len = 2u + (report_id != 0) + payload_len;
    total = (report_id >= 15 ? 7u : 6u) + report_len;
    if(capacity < total) return -1;
    position = report_command(desc, report_type, report_id, 0x03, out);
    put16(out + position, (u16)report_len);
    position += 2;
    if(report_id) out[position++] = (u8)report_id;
    for(i = 0; i < payload_len; i++) out[position + i] = payload[i];
    return (int)total;
}

enum i2chid_input_result i2chid_get_input(const u8 *bytes, u_int received,
                                         u_int max_input_len,
                                         const u8 **payload, u_int *payload_len) {
    u_int length;
    if(payload) *payload = 0;
    if(payload_len) *payload_len = 0;
    if(!payload || !payload_len || !bytes || received < 2 || max_input_len < 2 ||
       max_input_len > I2CHID_MAX_INPUT) return I2CHID_INPUT_INVALID;
    length = le16(bytes);
    if(!length) return I2CHID_INPUT_RESET;
    if(length < 2 || length > received || length > max_input_len)
        return I2CHID_INPUT_INVALID;
    if(length == 2) return I2CHID_INPUT_EMPTY;
    *payload = bytes + 2;
    *payload_len = length - 2;
    return I2CHID_INPUT_DATA;
}
