/* Hand-built HID descriptors exercise the actual parser and report decoder. */
#include <sys/hidpointer.h>
#include "touchpad_06cb_ce2d.h"
int printf(const char *format, ...);
static unsigned checks, failures;
static void check(bool ok, const char *what) {
    checks++;
    if(!ok) { failures++; printf("FAIL: %s\n", what); }
}
static const u8 mouse[] = {
    0x05,1, 0x09,2, 0xa1,1, 0x09,1, 0xa1,0,
    0x05,9, 0x19,1, 0x29,3, 0x15,0, 0x25,1, 0x75,1, 0x95,3, 0x81,2,
    0x75,5, 0x95,1, 0x81,1,
    0x05,1, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7f,
    0x75,8, 0x95,2, 0x81,6, 0xc0,0xc0
};
static u8 desc[1024];
static u_int used;
static void add(const u8 *p, u_int n) { u_int i; for(i=0;i<n;i++) desc[used++]=p[i]; }
#define ADD(...) do { const u8 p[] = { __VA_ARGS__ }; add(p,sizeof(p)); } while(0)
static void copy_mouse(void) { used=0; add(mouse,sizeof(mouse)); }
static void test_mouse(void) {
    struct hid_pointer_layout l={0};
    struct hid_pointer_sample s = {99,98,97,96};
    const u8 p[] = {5,0x81,0x7f}, shortp[] = {0,0};
    check(hid_pointer_parse(mouse,sizeof(mouse),&l)==HID_POINTER_OK && l.valid && l.has_pointer,
          "standard unnumbered mouse descriptor recognized");
    check(l.count==1 && l.report[0].input_bits==24 && !l.numbered,
          "padding contributes to whole report length");
    check(hid_pointer_decode(&l,p,sizeof(p),&s)==HID_POINTER_OK &&
          s.dx==-127 && s.dy==127 && s.buttons==5 && s.button_mask==7,
          "signed movement and three independent buttons decoded");
    check(hid_pointer_decode(&l,shortp,sizeof(shortp),&s)==HID_POINTER_SHORT &&
          s.dx==-127 && s.buttons==5, "short input cannot synthesize button releases");
}
static void test_bit_fields(void) {
    struct hid_pointer_field f={3,12,-2048,2047};
    u8 p[]={0x57,0x95,0x6a};
    i64 v=99;
    check(hid_pointer_patch(&f,p,sizeof(p),-1023)==HID_POINTER_OK &&
          (p[0]&7)==7 && (p[1]&0x80)==0x80 && p[2]==0x6a,
          "bit patch preserves both adjacent fields and trailing bytes");
    check(hid_pointer_extract(&f,p,sizeof(p),&v)==HID_POINTER_OK && v==-1023,
          "signed bit-packed field crosses byte boundary");
    check(hid_pointer_patch(&f,p,1,0)==HID_POINTER_SHORT && p[2]==0x6a,
          "short feature buffer rejected before changes");
    check(hid_pointer_patch(&f,p,sizeof(p),2048)==HID_POINTER_RANGE,
          "feature value outside logical range rejected");
    f.bit_offset=0; f.bit_size=32; f.minimum=-2147483648LL; f.maximum=2147483647LL;
    { const u8 a[]={0,0,0,0x80};
      check(hid_pointer_extract(&f,a,4,&v)==HID_POINTER_OK && v==-2147483648LL,
            "32-bit sign extension does not overflow"); }
    f.minimum=0; f.maximum=4294967295LL;
    { const u8 a[]={255,255,255,255};
      check(hid_pointer_extract(&f,a,4,&v)==HID_POINTER_OK && v==4294967295LL,
            "unsigned 32-bit feature data retains all bits"); }
}
static void test_ids_and_mode(void) {
    struct hid_pointer_layout l={0};
    struct hid_pointer_sample s={0};
    u8 p[]={7,0xab,0xf9,0x0f,0,0x48};
    u8 feature[]={9,0xad,0x5a};
    i64 v=-1;
    used=0;
    /* Vendor application consumes bytes in report 7 before the mouse fields. */
    ADD(0x06,0,0xff, 0x09,1, 0xa1,1, 0x85,7,
        0x75,8, 0x95,1, 0x81,2, 0xc0);
    ADD(0x05,1, 0x09,2, 0xa1,1, 0x09,1, 0xa1,0,
        0x15,0, 0x25,1, 0x05,9, 0x19,1, 0x29,3,
        0x75,1, 0x95,3, 0x81,2, 0x75,1, 0x95,1, 0x81,1,
        0x05,1, 0x09,0x30, 0x09,0x31,
        0x16,0,0xf8, 0x26,0xff,7, 0x75,12, 0x95,2, 0x81,6,
        0x75,4, 0x95,1, 0x81,1, 0xc0,0xc0);
    /* Configuration Feature: 3 unrelated bits, 2 mode bits, 11 padding bits. */
    ADD(0x05,0x0d, 0x09,0x0e, 0xa1,1, 0x85,9,
        0x75,3, 0x95,1, 0xb1,1, 0x09,0x52, 0x15,0, 0x25,2,
        0x75,2, 0xb1,2, 0x75,11, 0xb1,1, 0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK && l.numbered && l.count==2,
          "multiple report IDs and vendor application parsed");
    check(l.report[0].input_bits==40 && l.report[1].feature_bits==16,
          "per-ID input and feature offsets are independent");
    check(hid_pointer_decode(&l,p,sizeof(p),&s)==HID_POINTER_OK &&
          s.dx==255 && s.dy==-2048 && s.buttons==1 && s.button_mask==7,
          "vendor prefix and bit-packed signed 12-bit mouse axes decoded");
    check(l.mode.field.bit_size==2 && l.mode.field.bit_offset==3 &&
          l.mode.report_id==9 && l.mode.report_bytes==3,
          "Digitizer InputMode feature retains ID offset and complete length");
    check(hid_pointer_get_mode(&l,feature,3,&v)==HID_POINTER_OK && v==1,
          "mode feature extracted without confusing report ID with payload");
    check(hid_pointer_set_mode(&l,feature,3,0)==HID_POINTER_OK &&
          feature[0]==9 && feature[1]==0xa5 && feature[2]==0x5a,
          "mouse-mode patch preserves every unrelated feature bit");
    feature[0]=8;
    check(hid_pointer_set_mode(&l,feature,3,1)==HID_POINTER_UNRECOGNIZED && feature[1]==0xa5,
          "wrong feature ID rejected without writing");
    p[0]=9;
    check(hid_pointer_decode(&l,p,sizeof(p),&s)==HID_POINTER_UNRECOGNIZED && s.buttons==1,
          "feature-only ID is not an input report");
}
static void test_split_reports_and_push(void) {
    struct hid_pointer_layout l={0};
    struct hid_pointer_sample s={0};
    const u8 motion[]={1,0xfe,3}, buttons[]={2,2};
    used=0;
    ADD(0x05,1, 0x09,2, 0xa1,1, 0x85,1, 0x15,0x81, 0x25,0x7f,
        0x75,8, 0x95,2, 0xa4,
        0x05,9, 0x85,2, 0x15,0, 0x25,1, 0x75,1, 0x95,3,
        0x19,1, 0x29,3, 0x81,2, 0x75,5, 0x95,1, 0x81,1,
        0xb4, 0x19,0x30, 0x29,0x31, 0x81,6, 0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK,
          "global Pop restores report ID size count page and logical limits");
    check(hid_pointer_decode(&l,motion,3,&s)==HID_POINTER_OK && s.dx==-2 && s.dy==3 &&
          !s.button_mask, "motion-only ID reports unknown buttons rather than releasing them");
    check(hid_pointer_decode(&l,buttons,2,&s)==HID_POINTER_OK && !s.dx && !s.dy &&
          s.buttons==2 && s.button_mask==7, "button-only report leaves motion unchanged");
}
static void test_full_report_and_failure(void) {
    struct hid_pointer_layout l={0};
    struct hid_pointer_sample s={31,32,33,34};
    u8 p[]={3,1,2,0xee};
    copy_mouse();
    /* The same input report continues in a later vendor application. */
    ADD(0x06,0,0xff,0x09,1,0xa1,1,0x75,8,0x95,1,0x81,2,
        0x75,8,0x95,5,0x91,2,0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK &&
          l.report[0].input_bits==32 && l.report[0].output_bits==40,
          "later vendor input contributes to required length; Output has its own offset");
    check(hid_pointer_decode(&l,p,3,&s)==HID_POINTER_SHORT && s.dx==31 && s.button_mask==34,
          "complete mouse prefix is insufficient when later vendor bytes are missing");
    check(hid_pointer_decode(&l,p,4,&s)==HID_POINTER_OK && s.dx==1 && s.dy==2 && s.buttons==3,
          "full report with vendor trailer decoded");
    p[0]=0; p[2]=0x80;
    check(hid_pointer_decode(&l,p,4,&s)==HID_POINTER_OK && !s.buttons && s.dx==1 && s.dy==-127,
          "an axis below its logical minimum is clamped and the button change still counts");
    check(hid_pointer_parse(0,1,&l)==HID_POINTER_BAD_ARG &&
          hid_pointer_parse(mouse,sizeof(mouse),0)==HID_POINTER_BAD_ARG &&
          hid_pointer_decode(0,p,4,&s)==HID_POINTER_BAD_ARG &&
          hid_pointer_decode(&l,0,4,&s)==HID_POINTER_BAD_ARG,
          "null public arguments are rejected");
    check(hid_pointer_get_mode(&l,p,4,0)==HID_POINTER_UNRECOGNIZED,
          "descriptor with no mode feature does not invent one");
}
static void test_extended_usages_and_mode_only(void) {
    struct hid_pointer_layout l={0};
    struct hid_pointer_sample s={0};
    const u8 motion[]={0xfe,0xff,0,1};
    u8 feature[]={0x41,0xaa};
    i64 mode=99;
    used=0;
    ADD(0x06,0,0xff,0x0b,2,0,1,0,0xa1,1,
        0x0b,0x30,0,1,0,0x0b,0x31,0,1,0,
        0x16,0,0x80,0x26,0xff,0x7f,0x75,16,0x95,2,0x81,6,0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK &&
          hid_pointer_decode(&l,motion,4,&s)==HID_POINTER_OK && s.dx==-2 && s.dy==256,
          "32-bit Usage items override the current vendor usage page");
    used=0;
    ADD(0x05,0x0d,0x09,0x0e,0xa1,1,0x09,0x52,0x15,0,0x25,2,
        0x75,2,0x95,1,0xb1,2,0x75,14,0xb1,1,0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK && !l.has_pointer &&
          l.mode.report_bytes==2 && !l.numbered,
          "mode-only descriptor permits configuration without claiming mouse input");
    check(hid_pointer_get_mode(&l,feature,2,&mode)==HID_POINTER_OK && mode==1 &&
          hid_pointer_set_mode(&l,feature,2,0)==HID_POINTER_OK &&
          feature[0]==0x40 && feature[1]==0xaa,
          "unnumbered mode feature patch preserves unrelated data");
    check(hid_pointer_set_mode(&l,feature,1,1)==HID_POINTER_SHORT && feature[0]==0x40,
          "mode feature requires the full Feature report before any write");
    check(hid_pointer_set_mode(&l,feature,2,3)==HID_POINTER_RANGE && feature[0]==0x40 &&
          hid_pointer_decode(&l,feature,2,&s)==HID_POINTER_UNRECOGNIZED,
          "mode-only input and out-of-range mode values rejected");
    { u8 built[2]={0xee,0xee};
      check(l.mode.exclusive && hid_pointer_mode_report(&l,built,2,2)==HID_POINTER_OK &&
            built[0]==2 && built[1]==0, "unnumbered mode report: no ID byte, padding zeroed"); }
}
/* 10/15/26: Linux's handling of out-of-range axes and doubled InputModes. */
static void test_ranges_and_quirks(void) {
    struct hid_pointer_layout l={0};
    struct hid_pointer_sample s={0};
    const u8 low[]={1,0x80,0x05}, high[]={2,0x7f,0x80};
    used=0;
    /* Relative axes with a null state: an out-of-range value means no motion. */
    ADD(0x05,1, 0x09,2, 0xa1,1, 0x09,1, 0xa1,0,
        0x05,9, 0x19,1, 0x29,2, 0x15,0, 0x25,1, 0x75,1, 0x95,2, 0x81,2, 0x95,6, 0x81,1,
        0x05,1, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7f, 0x75,8, 0x95,2, 0x81,0x46, 0xc0,0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK && l.report[0].x.null_state &&
          l.report[0].y.null_state, "Null State recorded on the axes");
    check(hid_pointer_decode(&l,low,sizeof(low),&s)==HID_POINTER_OK && s.buttons==1 && !s.dx && s.dy==5,
          "null-state axis below its range reports no motion; the other axis and button count");
    check(hid_pointer_decode(&l,high,sizeof(high),&s)==HID_POINTER_OK && s.buttons==2 && s.dx==127 && !s.dy,
          "a value in range next to a null one is kept");
    used=0;
    /* InputMode declared twice in one report: the first is used, as Linux does. */
    ADD(0x05,0x0d, 0x09,0x0e, 0xa1,1, 0x85,4, 0x09,0x52, 0x15,0, 0x25,10, 0x75,8, 0x95,1, 0xb1,2,
        0x09,0x52, 0xb1,2, 0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK && l.mode.report_id==4 &&
          l.mode.field.bit_offset==0 && l.mode.report_bytes==3 && !l.mode.exclusive,
          "a second InputMode is skipped; the report holds two fields, so it is read first");
}
/* 10/15/26: the laptop's actual descriptor (tests/host/touchpad_06cb_ce2d.h). */
static void test_synaptics_ce2d(void) {
    struct hid_pointer_layout l={0};
    struct hid_pointer_sample s={0};
    const u8 move[]={2,1,0x05,0xfb}, right[]={2,2,0,0};
    const u8 contact[]={3,0x03,0x10,0x02,0x20,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                        0,0,0x34,0x12,1,1};
    static const u8 vendor[62]={11,1,2,3};  /* Report 11: 61 vendor bytes. */
    u8 feature[4]={0xee,0xee,0xee,0xee};
    i64 mode=99;
    check(hid_pointer_parse(synaptics_ce2d_descriptor,sizeof(synaptics_ce2d_descriptor),&l)==HID_POINTER_OK &&
          l.valid && l.has_pointer && l.numbered,
          "Synaptics 06cb:ce2d: the laptop's 665-byte descriptor parses");
    check(l.report[0].id==2 && l.report[0].input_bits==24 &&
          l.report[0].button[0].bit_offset==0 && l.report[0].button[1].bit_offset==1 &&
          !l.report[0].button[2].bit_size && l.report[0].x.bit_offset==8 &&
          l.report[0].x.bit_size==8 && l.report[0].x.minimum==-127 && l.report[0].y.bit_offset==16,
          "Synaptics 06cb:ce2d: mouse report 2 has two buttons and 8-bit relative X/Y");
    check(l.mode.report_id==4 && l.mode.report_bytes==2 && l.mode.field.bit_offset==0 &&
          l.mode.field.bit_size==8 && l.mode.field.minimum==0 && l.mode.field.maximum==10 &&
          l.mode.exclusive,
          "Synaptics 06cb:ce2d: InputMode is report 4's only field, 8 bits, 0..10");
    check(hid_pointer_decode(&l,move,sizeof(move),&s)==HID_POINTER_OK &&
          s.dx==5 && s.dy==-5 && s.buttons==1 && s.button_mask==3,
          "Synaptics 06cb:ce2d: a mouse report moves and presses the left button");
    check(hid_pointer_decode(&l,right,sizeof(right),&s)==HID_POINTER_OK &&
          !s.dx && !s.dy && s.buttons==2 && s.button_mask==3,
          "Synaptics 06cb:ce2d: the second button is the right button");
    s=(struct hid_pointer_sample){41,42,43,44};
    check(hid_pointer_decode(&l,contact,sizeof(contact),&s)==HID_POINTER_UNRECOGNIZED &&
          hid_pointer_decode(&l,vendor,sizeof(vendor),&s)==HID_POINTER_UNRECOGNIZED &&
          s.dx==41 && s.button_mask==44,
          "Synaptics 06cb:ce2d: touchpad contacts and vendor reports never move the pointer");
    check(hid_pointer_mode_report(&l,feature,1,0)==HID_POINTER_SHORT && feature[0]==0xee,
          "mode report needs room for the whole report before writing");
    check(hid_pointer_mode_report(&l,feature,sizeof(feature),11)==HID_POINTER_RANGE && feature[0]==0xee,
          "mode report refuses a value outside InputMode's logical range");
    check(hid_pointer_mode_report(&l,feature,sizeof(feature),0)==HID_POINTER_OK &&
          feature[0]==4 && feature[1]==0 && feature[2]==0xee,
          "Synaptics 06cb:ce2d: mouse mode is report 4 with one zero byte, built without reading");
    check(hid_pointer_mode_report(&l,feature,sizeof(feature),3)==HID_POINTER_OK &&
          feature[0]==4 && feature[1]==3 && hid_pointer_get_mode(&l,feature,2,&mode)==HID_POINTER_OK &&
          mode==3, "built mode report reads back through the same layout");
}
static void test_mode_report_sharing(void) {
    struct hid_pointer_layout l={0};
    u8 feature[3]={0xee,0xee,0xee};
    used=0;
    /* InputMode beside a Data field (Device Index, 0x0d:0x53): not built. */
    ADD(0x05,0x0d,0x09,0x0e,0xa1,1,0x85,5,0x15,0,0x25,10,0x75,8,0x95,1,
        0x09,0x52,0xb1,2,0x09,0x53,0xb1,2,0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK && l.mode.report_id==5 &&
          l.mode.report_bytes==3 && !l.mode.exclusive,
          "InputMode sharing its report with another Data field is not exclusive");
    check(hid_pointer_mode_report(&l,feature,3,0)==HID_POINTER_UNSUPPORTED &&
          feature[0]==0xee && feature[1]==0xee && feature[2]==0xee,
          "shared mode report is never built from zeros");
    used=0;
    /* The same, but the second field in one Main item with InputMode. */
    ADD(0x05,0x0d,0x09,0x0e,0xa1,1,0x85,5,0x15,0,0x25,10,0x75,8,0x95,2,
        0x09,0x52,0x09,0x53,0xb1,2,0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK && !l.mode.exclusive,
          "a second field of InputMode's own Main item also shares the report");
    used=0;
    /* Constant padding and an Output field of the same ID do not count. */
    ADD(0x05,0x0d,0x09,0x0e,0xa1,1,0x85,5,0x15,0,0x25,10,0x75,4,0x95,1,
        0xb1,1,0x09,0x52,0xb1,2,0x09,0x53,0x91,2,0xc0);
    check(hid_pointer_parse(desc,used,&l)==HID_POINTER_OK && l.mode.exclusive &&
          l.mode.report_bytes==2 && l.mode.field.bit_offset==4,
          "padding and other report types leave InputMode exclusive");
    check(hid_pointer_mode_report(&l,feature,3,2)==HID_POINTER_OK &&
          feature[0]==5 && feature[1]==0x20 && feature[2]==0xee,
          "built mode report zeroes padding and places InputMode after it");
}
static void rejected(enum hid_pointer_error expected, const char *what) {
    struct hid_pointer_layout l={0};
    l.count=999;
    check(hid_pointer_parse(desc,used,&l)==expected && l.count==999,what);
}
static void test_invalid(void) {
    struct hid_pointer_layout l={0};
    struct hid_pointer_field f={0xffffffff,32,0,4294967295LL};
    i64 v=99;
    u8 p[]={0xaa,0xbb};
    unsigned i;
    for(i=0;i<sizeof(mouse);i++) {
        l.count=999;
        check(hid_pointer_parse(mouse,i,&l)!=HID_POINTER_OK && l.count==999,
              "every truncated descriptor rejected without replacing layout");
    }
    copy_mouse(); ADD(0xc0); rejected(HID_POINTER_MALFORMED,"unmatched EndCollection rejected");
    used=0; ADD(0xb4); rejected(HID_POINTER_MALFORMED,"global stack underflow rejected");
    used=0; for(i=0;i<9;i++) ADD(0xa4);
    rejected(HID_POINTER_LIMIT,"global stack overflow bounded");
    used=0; for(i=0;i<17;i++) ADD(0xa1,1);
    rejected(HID_POINTER_LIMIT,"collection stack overflow bounded");
    used=0; ADD(0x85,0); rejected(HID_POINTER_MALFORMED,"zero explicit ReportID rejected");
    used=0; ADD(0xfe,4,1,0); rejected(HID_POINTER_MALFORMED,"truncated long item rejected");
    used=0; ADD(0xfe,1,1,0); rejected(HID_POINTER_UNSUPPORTED,"long item not silently misparsed");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x75,32,0x97,255,255,255,255,0x81,1,0xc0);
    rejected(HID_POINTER_LIMIT,"malicious ReportCount cannot wrap bit length");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x19,0x31,0x29,0x30,0xc0);
    rejected(HID_POINTER_MALFORMED,"reversed usage range rejected");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x29,0x31,0xc0);
    rejected(HID_POINTER_MALFORMED,"usage maximum without minimum rejected");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0xa9,1,0xc0);
    rejected(HID_POINTER_UNSUPPORTED,"alternative usage sets rejected explicitly");
    copy_mouse(); ADD(0x85,1); add(mouse,sizeof(mouse));
    rejected(HID_POINTER_MALFORMED,"numbered and unnumbered report data cannot mix");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x09,0x30,0x15,0x81,0x25,0x7f,
                0x75,8,0x95,1,0x81,1,0x81,6,0xc0);
    rejected(HID_POINTER_UNSUPPORTED,"constant Input clears local Usage before next Input");
    used=0; ADD(0x06,0,0xff,0x09,1,0xa1,1,0x05,1,0x19,0x30,0x29,0x31,
                0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0);
    rejected(HID_POINTER_UNSUPPORTED,"vendor application relative axes are not a Mouse collection");
    check(hid_pointer_extract(&f,p,2,&v)==HID_POINTER_SHORT && v==99,
          "field end arithmetic cannot wrap and read out of bounds");
    check(hid_pointer_patch(&f,p,2,0)==HID_POINTER_SHORT && p[0]==0xaa && p[1]==0xbb,
          "out of bounds patch cannot change any bytes");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x19,0x30,0x29,0x31,
                0x15,0,0x25,0x7f,0x75,8,0x95,2,0x81,2,0xc0);
    rejected(HID_POINTER_UNSUPPORTED,"absolute axes are never decoded as relative movement");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x09,0x30,
                0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0);
    rejected(HID_POINTER_UNSUPPORTED,"repeated last Usage causing duplicate X is ambiguous");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x09,0x30,
                0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,6,0xc0,
                0x09,2,0xa1,1,0x09,0x31,0x81,6,0xc0);
    rejected(HID_POINTER_UNSUPPORTED,"independent pointers sharing one report are ambiguous");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x09,0x30,
                0x15,0x81,0x25,0x7f,0x75,4,0x95,1,0x81,6,0xc0);
    rejected(HID_POINTER_MALFORMED,"logical range must fit the declared field width");
    used=0; ADD(0x05,1,0x09,2,0xa1,1);
    for(i=0;i<33;i++) ADD(0x09,1);
    rejected(HID_POINTER_LIMIT,"local Usage list has a finite bound");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x75,8,0x95,1);
    for(i=1;i<=17;i++) ADD(0x85,i,0x81,1);
    ADD(0xc0);
    rejected(HID_POINTER_LIMIT,"distinct ReportID count has a finite bound");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x75,32,0x96,0,1,0x81,1,0x81,1,0xc0);
    rejected(HID_POINTER_LIMIT,"cumulative bit length is bounded across Main items");
    used=0; ADD(0x05,1,0x09,2,0xa1,1,0x19,0x30,0x29,0x31,
                0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x82,6,1,0xc0);
    rejected(HID_POINTER_UNSUPPORTED,"buffered-byte data is not a scalar axis field");
}
static void test_mutated_descriptors(void) {
    struct hid_pointer_layout l;
    struct hid_pointer_sample sample;
    u32 random=0x5324af61;
    u8 input[32]={0};
    unsigned i,j;
    bool ok=true;
    /* Corrupt both item prefixes and payloads of a real descriptor. Sanitizers
       check every resulting path; errors must leave caller-owned state alone. */
    for(i=0;i<4000;i++) {
        enum hid_pointer_error e;
        copy_mouse();
        random=random*1664525U+1013904223U;
        desc[random%used]=(u8)(random>>24);
        if(i&1) used=random%used;
        l=(struct hid_pointer_layout){0}; l.count=999;
        e=hid_pointer_parse(desc,used,&l);
        if(e!=HID_POINTER_OK) { if(l.count!=999) ok=false; continue; }
        for(j=0;j<sizeof(input);j++) {
            sample=(struct hid_pointer_sample){31,32,33,34};
            e=hid_pointer_decode(&l,input,j,&sample);
            if(e!=HID_POINTER_OK && (sample.dx!=31 || sample.dy!=32 ||
               sample.buttons!=33 || sample.button_mask!=34)) ok=false;
        }
    }
    check(ok,"mutated descriptors and all short report lengths preserve outputs on failure");
}
int main(void) {
    test_mouse(); test_bit_fields(); test_ids_and_mode(); test_split_reports_and_push();
    test_full_report_and_failure(); test_extended_usages_and_mode_only();
    test_synaptics_ce2d(); test_mode_report_sharing(); test_ranges_and_quirks();
    test_invalid(); test_mutated_descriptors();
    printf("hidpointer: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
