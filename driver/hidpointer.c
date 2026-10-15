/*
SurfOS HID Pointer Reports
--------------------
File: hidpointer.c Date: 10/12/26
--------------------
HID 1.11 section 6.2.2; Usage Tables Generic Desktop, Button and Digitizer pages.
https://www.usb.org/sites/default/files/hid1_11.pdf
https://www.usb.org/sites/default/files/hut1_5.pdf
*/
#include <sys/hidpointer.h>

#define GLOBAL_DEPTH 8
#define COLLECTION_DEPTH 16
#define LOCAL_USAGES 32

struct globals {
    u32 page, size, count, id;
    i64 minimum, maximum;
};
struct usage_range { u32 first, last; };
struct locals {
    struct usage_range usage[LOCAL_USAGES];
    u_int count;
    bool pending;
    u32 minimum;
};
struct scope { bool pointer; u32 owner; };
struct parser {
    struct hid_pointer_layout *layout;
    struct globals g, saved[GLOBAL_DEPTH];
    u_int globals, collections;
    struct locals local;
    struct scope scope, enclosing[COLLECTION_DEPTH];
    u32 owner, report_owner[HID_POINTER_REPORTS];
    u32 feature_data[HID_POINTER_REPORTS];  /* Non-constant Feature fields per report. */
};

static i64 signed_item(u32 value, u_int bytes) {
    u_int bits=bytes*8;
    if(bits && (value & (1U << (bits-1)))) return (i64)value-(1LL << bits);
    return value;
}
static u32 usage_value(struct parser *p, u32 value, u_int bytes) {
    return bytes==4 ? value : (p->g.page << 16) | value;
}
static enum hid_pointer_error append_usage(struct locals *l, u32 first, u32 last) {
    if(l->count==LOCAL_USAGES) return HID_POINTER_LIMIT;
    l->usage[l->count].first=first;
    l->usage[l->count++].last=last;
    return HID_POINTER_OK;
}
/* A Variable item repeats the last usage when ReportCount exceeds the list.
   Keep ranges compact: an untrusted UsageMaximum must not expand a buffer. */
static u32 usage_at(const struct locals *l, u32 index) {
    u_int i;
    for(i=0;i<l->count;i++) {
        u32 count=l->usage[i].last-l->usage[i].first+1;
        if(index<count) return l->usage[i].first+index;
        index-=count;
    }
    return l->count ? l->usage[l->count-1].last : 0;
}
static enum hid_pointer_error global_item(struct parser *p, u_int tag,
                                           u32 value, u_int bytes) {
    if(tag==10) {
        if(bytes) return HID_POINTER_MALFORMED;
        if(p->globals==GLOBAL_DEPTH) return HID_POINTER_LIMIT;
        p->saved[p->globals++]=p->g;
        return HID_POINTER_OK;
    }
    if(tag==11) {
        if(bytes || !p->globals) return HID_POINTER_MALFORMED;
        p->g=p->saved[--p->globals];
        return HID_POINTER_OK;
    }
    if(!bytes) return HID_POINTER_MALFORMED;
    switch(tag) {
    case 0:
        if(value>0xffff) return HID_POINTER_MALFORMED;
        p->g.page=value;
        break;
    case 1: p->g.minimum=signed_item(value,bytes); break;
    case 2: p->g.maximum=p->g.minimum<0 ? signed_item(value,bytes) : (i64)value; break;
    case 3: case 4: case 5: case 6: break; /* Physical limits and units. */
    case 7: p->g.size=value; break;
    case 8:
        if(bytes!=1 || !value) return HID_POINTER_MALFORMED;
        p->g.id=value;
        p->layout->numbered=true;
        break;
    case 9: p->g.count=value; break;
    default: return HID_POINTER_UNSUPPORTED;
    }
    return HID_POINTER_OK;
}
static enum hid_pointer_error local_item(struct parser *p, u_int tag,
                                          u32 value, u_int bytes) {
    u32 usage;
    if(!bytes) return HID_POINTER_MALFORMED;
    usage=usage_value(p,value,bytes);
    switch(tag) {
    case 0:
        if(p->local.pending) return HID_POINTER_MALFORMED;
        return append_usage(&p->local,usage,usage);
    case 1:
        if(p->local.pending) return HID_POINTER_MALFORMED;
        p->local.minimum=usage;
        p->local.pending=true;
        break;
    case 2:
        if(!p->local.pending || usage<p->local.minimum ||
           (usage>>16)!=(p->local.minimum>>16)) return HID_POINTER_MALFORMED;
        p->local.pending=false;
        return append_usage(&p->local,p->local.minimum,usage);
    case 3: case 4: case 5: case 7: case 8: case 9: break; /* Designators/strings. */
    default: return HID_POINTER_UNSUPPORTED; /* Includes alternative usage sets. */
    }
    return HID_POINTER_OK;
}
static enum hid_pointer_error field_shape(const struct hid_pointer_field *f) {
    i64 minimum, maximum;
    if(!f || !f->bit_size || f->bit_size>32 || f->minimum>f->maximum)
        return HID_POINTER_BAD_ARG;
    if(f->minimum<0) {
        minimum=-(1LL << (f->bit_size-1));
        maximum=(1LL << (f->bit_size-1))-1;
    } else {
        minimum=0;
        maximum=(1LL << f->bit_size)-1;
    }
    if(f->minimum<minimum || f->maximum>maximum) return HID_POINTER_RANGE;
    return HID_POINTER_OK;
}
static enum hid_pointer_error bind_field(struct parser *p,
        struct hid_pointer_field *field, u32 offset) {
    struct hid_pointer_field f;
    if(field->bit_size || p->g.size>32) return HID_POINTER_UNSUPPORTED;
    f.bit_offset=offset;
    f.bit_size=(u8)p->g.size;
    f.minimum=p->g.minimum;
    f.maximum=p->g.maximum;
    if(field_shape(&f)!=HID_POINTER_OK) return HID_POINTER_MALFORMED;
    *field=f;
    return HID_POINTER_OK;
}
static enum hid_pointer_error data_item(struct parser *p, u_int tag, u32 flags) {
    struct hid_pointer_report *r;
    u64 added=(u64)p->g.size*p->g.count;
    u32 *bits, offset, i;
    u_int ri;
    if(!p->collections || !p->g.size || !p->g.count) return HID_POINTER_MALFORMED;
    if(added>HID_POINTER_MAX_BITS) return HID_POINTER_LIMIT;
    for(ri=0;ri<p->layout->count;ri++)
        if(p->layout->report[ri].id==p->g.id) break;
    if(ri==p->layout->count) {
        if(ri==HID_POINTER_REPORTS) return HID_POINTER_LIMIT;
        p->layout->count++;
        p->layout->report[ri].id=(u8)p->g.id;
    }
    r=&p->layout->report[ri];
    bits=tag==8 ? &r->input_bits : tag==9 ? &r->output_bits : &r->feature_bits;
    if(added>HID_POINTER_MAX_BITS-*bits) return HID_POINTER_LIMIT;
    offset=*bits;
    *bits+=(u32)added;
    if(tag==11 && !(flags&1)) p->feature_data[ri]+=p->g.count; /* Bounded by the bit limit. */
    if((flags&3)!=2) return HID_POINTER_OK; /* Constant or Array. */
    for(i=0;i<p->g.count;i++) {
        u32 usage=usage_at(&p->local,i);
        struct hid_pointer_field *field=0;
        enum hid_pointer_error error;
        if(tag==8 && p->scope.pointer) {
            if((flags&4) && usage==0x10030) field=&r->x;
            else if((flags&4) && usage==0x10031) field=&r->y;
            else if(!(flags&4) && usage>=0x90001 && usage<=0x90003) {
                if(p->g.minimum!=0 || p->g.maximum!=1) return HID_POINTER_UNSUPPORTED;
                field=&r->button[usage-0x90001];
            }
            if(field) {
                if(p->report_owner[ri] && p->report_owner[ri]!=p->scope.owner)
                    return HID_POINTER_UNSUPPORTED;
                p->report_owner[ri]=p->scope.owner;
                p->layout->has_pointer=true;
            }
        } else if(tag==11 && !(flags&4) && usage==0xd0052) {
            /* 10/15/26: as Linux, a second InputMode (some ELAN panels
               declare two) is skipped; the first one is set. */
            if(p->layout->mode.field.bit_size) continue;
            if(p->g.minimum>0 || p->g.maximum<0) return HID_POINTER_UNSUPPORTED;
            field=&p->layout->mode.field;
            p->layout->mode.report_id=r->id;
        }
        if(!field) continue;
        /* Buffered bytes, nonlinear and wrapped values need other decoders. */
        if(flags&(0x100|0x10|8)) return HID_POINTER_UNSUPPORTED;
        error=bind_field(p,field,offset+i*p->g.size);
        if(error!=HID_POINTER_OK) return error;
        field->null_state=(flags&0x40)!=0;
    }
    return HID_POINTER_OK;
}
static enum hid_pointer_error main_item(struct parser *p, u_int tag,
                                         u32 value, u_int bytes) {
    enum hid_pointer_error error=HID_POINTER_OK;
    if(p->local.pending) return HID_POINTER_MALFORMED;
    if(tag==10) {
        u32 usage=usage_at(&p->local,0);
        if(bytes!=1) return HID_POINTER_MALFORMED;
        if(p->collections==COLLECTION_DEPTH) return HID_POINTER_LIMIT;
        p->enclosing[p->collections++]=p->scope;
        if(value==1) p->scope.pointer=false; /* New Application context. */
        if((usage==0x10001 || usage==0x10002) && !p->scope.pointer) {
            p->scope.pointer=true;
            p->scope.owner=++p->owner;
        }
    } else if(tag==12) {
        if(bytes || !p->collections) return HID_POINTER_MALFORMED;
        p->scope=p->enclosing[--p->collections];
    } else if(tag==8 || tag==9 || tag==11) {
        if(!bytes) return HID_POINTER_MALFORMED;
        error=data_item(p,tag,value);
    } else error=HID_POINTER_UNSUPPORTED;
    /* Local items apply only to the next Main item, including Constant items. */
    p->local.count=0;
    p->local.pending=false;
    return error;
}

enum hid_pointer_error hid_pointer_parse(const u8 *descriptor, u_int length,
                                         struct hid_pointer_layout *out) {
    struct hid_pointer_layout layout={0};
    struct parser p={0};
    u_int pos=0, i;
    if(!descriptor || !out || !length) return HID_POINTER_BAD_ARG;
    if(length>HID_POINTER_MAX_DESCRIPTOR) return HID_POINTER_LIMIT;
    p.layout=&layout;
    while(pos<length) {
        u8 prefix=descriptor[pos++];
        u_int bytes=prefix&3, type=(prefix>>2)&3, tag=prefix>>4;
        u32 value=0;
        enum hid_pointer_error error;
        if(prefix==0xfe) {
            if(length-pos<2 || descriptor[pos]>length-pos-2) return HID_POINTER_MALFORMED;
            return HID_POINTER_UNSUPPORTED;
        }
        if(bytes==3) bytes=4;
        if(bytes>length-pos) return HID_POINTER_MALFORMED;
        for(i=0;i<bytes;i++) value|=(u32)descriptor[pos++] << (i*8);
        if(type==0) error=main_item(&p,tag,value,bytes);
        else if(type==1) error=global_item(&p,tag,value,bytes);
        else if(type==2) error=local_item(&p,tag,value,bytes);
        else error=HID_POINTER_UNSUPPORTED;
        if(error!=HID_POINTER_OK) return error;
    }
    if(p.collections || p.globals || p.local.count || p.local.pending)
        return HID_POINTER_MALFORMED;
    for(i=0;i<layout.count;i++) {
        if(layout.numbered && !layout.report[i].id) return HID_POINTER_MALFORMED;
        if(layout.mode.field.bit_size && layout.report[i].id==layout.mode.report_id) {
            layout.mode.report_bytes=(layout.report[i].feature_bits+7)/8+(layout.numbered?1:0);
            layout.mode.exclusive=p.feature_data[i]==1;
        }
    }
    if(!layout.has_pointer && !layout.mode.field.bit_size) return HID_POINTER_UNSUPPORTED;
    layout.valid=true;
    *out=layout;
    return HID_POINTER_OK;
}

static enum hid_pointer_error field_buffer(const struct hid_pointer_field *f, u_int length) {
    enum hid_pointer_error error=field_shape(f);
    if(error!=HID_POINTER_OK) return error;
    if((u64)f->bit_offset+f->bit_size>(u64)length*8) return HID_POINTER_SHORT;
    return HID_POINTER_OK;
}
/* The field's bits, sign-extended when its logical minimum is negative. */
static enum hid_pointer_error field_raw(const struct hid_pointer_field *f,
                                        const u8 *payload, u_int length, i64 *out) {
    u_int i;
    u32 raw=0;
    i64 value;
    enum hid_pointer_error error;
    if(!payload || !out) return HID_POINTER_BAD_ARG;
    error=field_buffer(f,length);
    if(error!=HID_POINTER_OK) return error;
    for(i=0;i<f->bit_size;i++) {
        u64 bit=(u64)f->bit_offset+i;
        if(payload[bit/8] & (1U << (bit%8))) raw|=1U << i;
    }
    value=raw;
    if(f->minimum<0 && (raw & (1U << (f->bit_size-1)))) value-=(1LL << f->bit_size);
    *out=value;
    return HID_POINTER_OK;
}
enum hid_pointer_error hid_pointer_extract(const struct hid_pointer_field *f,
                                           const u8 *payload, u_int length, i64 *out) {
    i64 value;
    enum hid_pointer_error error=field_raw(f,payload,length,&value);
    if(error!=HID_POINTER_OK) return error;
    if(value<f->minimum || value>f->maximum) return HID_POINTER_RANGE;
    *out=value;
    return HID_POINTER_OK;
}
enum hid_pointer_error hid_pointer_patch(const struct hid_pointer_field *f,
                                         u8 *payload, u_int length, i64 value) {
    u_int i;
    enum hid_pointer_error error;
    if(!payload) return HID_POINTER_BAD_ARG;
    error=field_buffer(f,length);
    if(error!=HID_POINTER_OK) return error;
    if(value<f->minimum || value>f->maximum) return HID_POINTER_RANGE;
    for(i=0;i<f->bit_size;i++) {
        u64 bit=(u64)f->bit_offset+i;
        u8 mask=(u8)(1U << (bit%8));
        if(((u64)value >> i)&1) payload[bit/8]|=mask;
        else payload[bit/8]&=(u8)~mask;
    }
    return HID_POINTER_OK;
}
enum hid_pointer_error hid_pointer_decode(const struct hid_pointer_layout *layout,
        const u8 *report, u_int length, struct hid_pointer_sample *out) {
    struct hid_pointer_sample sample={0};
    const struct hid_pointer_report *r=0;
    u_int i, id=0;
    bool found=false;
    if(!layout || !layout->valid || layout->count>HID_POINTER_REPORTS || !report || !out)
        return HID_POINTER_BAD_ARG;
    if(layout->numbered) {
        if(!length) return HID_POINTER_SHORT;
        id=*report++; length--;
    }
    for(i=0;i<layout->count;i++)
        if(layout->report[i].id==id) { r=&layout->report[i]; break; }
    if(!r || !r->input_bits) return HID_POINTER_UNRECOGNIZED;
    if((u64)length*8<r->input_bits) return HID_POINTER_SHORT;
    for(i=0;i<5;i++) {
        const struct hid_pointer_field *f=i==0 ? &r->x : i==1 ? &r->y : &r->button[i-2];
        i64 value;
        enum hid_pointer_error error;
        if(!f->bit_size) continue;
        error=field_raw(f,report,length,&value);
        if(error!=HID_POINTER_OK) return error;
        /* Out of range: no value with a null state, else clamped (Linux
           hid-input, Windows); one bad axis must not drop a button change. */
        if(value<f->minimum || value>f->maximum)
            value=f->null_state ? (f->minimum<=0 && f->maximum>=0 ? 0 : f->minimum) :
                  value<f->minimum ? f->minimum : f->maximum;
        if(value < -2147483648LL || value>2147483647LL) return HID_POINTER_RANGE;
        if(i==0) sample.dx=(int)value;
        else if(i==1) sample.dy=(int)value;
        else {
            sample.button_mask|=1U << (i-2);
            if(value) sample.buttons|=1U << (i-2);
        }
        found=true;
    }
    if(!found) return HID_POINTER_UNRECOGNIZED;
    *out=sample;
    return HID_POINTER_OK;
}
static enum hid_pointer_error mode_buffer(const struct hid_pointer_layout *layout,
                                           const u8 *report, u_int length) {
    if(!layout || !layout->valid || !report) return HID_POINTER_BAD_ARG;
    if(!layout->mode.field.bit_size) return HID_POINTER_UNRECOGNIZED;
    if(length<layout->mode.report_bytes || (layout->numbered && !length))
        return HID_POINTER_SHORT;
    if(layout->numbered && *report!=layout->mode.report_id) return HID_POINTER_UNRECOGNIZED;
    return HID_POINTER_OK;
}
enum hid_pointer_error hid_pointer_get_mode(const struct hid_pointer_layout *layout,
                                             const u8 *report, u_int length, i64 *out) {
    enum hid_pointer_error error=mode_buffer(layout,report,length);
    u_int prefix;
    if(error!=HID_POINTER_OK) return error;
    prefix=layout->numbered?1:0;
    return hid_pointer_extract(&layout->mode.field,report+prefix,length-prefix,out);
}
enum hid_pointer_error hid_pointer_set_mode(const struct hid_pointer_layout *layout,
                                             u8 *report, u_int length, i64 value) {
    enum hid_pointer_error error=mode_buffer(layout,report,length);
    u_int prefix;
    if(error!=HID_POINTER_OK) return error;
    prefix=layout->numbered?1:0;
    return hid_pointer_patch(&layout->mode.field,report+prefix,length-prefix,value);
}
enum hid_pointer_error hid_pointer_mode_report(const struct hid_pointer_layout *layout,
                                               u8 *report, u_int length, i64 value) {
    const struct hid_pointer_field *f;
    enum hid_pointer_error error;
    u_int prefix, i;
    if(!layout || !layout->valid || !report) return HID_POINTER_BAD_ARG;
    f=&layout->mode.field;
    if(!f->bit_size) return HID_POINTER_UNRECOGNIZED;
    if(!layout->mode.exclusive) return HID_POINTER_UNSUPPORTED;
    prefix=layout->numbered?1:0;
    if(length<layout->mode.report_bytes || layout->mode.report_bytes<=prefix) return HID_POINTER_SHORT;
    /* Every check of the patch below, made before the buffer is cleared. */
    error=field_buffer(f,layout->mode.report_bytes-prefix);
    if(error!=HID_POINTER_OK) return error;
    if(value<f->minimum || value>f->maximum) return HID_POINTER_RANGE;
    for(i=0;i<layout->mode.report_bytes;i++) report[i]=0;
    if(prefix) report[0]=layout->mode.report_id;
    return hid_pointer_patch(f,report+prefix,layout->mode.report_bytes-prefix,value);
}
