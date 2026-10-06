/*
SurfOS fdisk
--------------------
File: fdisk.c   Date: 10/5/26 (USB storage)
--------------------
Partition tables of whole disks, MBR or GPT. driver/parttab.c reads tables
and plans changes; driver/part.c writes them.

    fdisk <disk>                    the table, the free space, and whether the disk may be changed
    fdisk <disk> new mbr|gpt        a new, empty table (MBR is what a data stick usually has)
    fdisk <disk> add [size] [type]  size 500M, 2G or all (the default: the largest free region);
                                    type fat32 (the default), fat16, linux, efi, or an MBR byte in hex
    fdisk <disk> delete <n>
    fdisk <disk> type <n> <type>

A change is made only when bdev_may_format() allows it: a USB disk or a
ramdisk, not the SurfOS boot drive, not read-only, nothing on it mounted.
fdisk shows the disk, its table and the change, and writes only after the
disk's name has been typed back. What was written is read back and checked
before fdisk says so, and the disk's partitions are registered again.

10/6/26: the status (shell.h) is CMD_OK for a table shown or written, CMD_USAGE for
what fdisk does not understand, and CMD_FAIL otherwise: a refused disk, a change
that cannot be planned, a Cancelled question or a failed write.
*/

#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <sys/bdev.h>
#include <sys/part.h>
#include <mm/kalloc.h>
#include "shell.h"

static void usage(void) {
    printf("    fdisk <disk>                   the partition table and the free space\n");
    printf("    fdisk <disk> new mbr|gpt       a new, empty partition table\n");
    printf("    fdisk <disk> add [size] [type] a partition: size 500M, 2G or all (the\n");
    printf("                                   largest free region, the default); type\n");
    printf("                                   fat32 (the default), fat16, linux, efi,\n");
    printf("                                   or an MBR type byte in hex\n");
    printf("    fdisk <disk> delete <n>        a partition (logical: with its extended one)\n");
    printf("    fdisk <disk> type <n> <type>   a partition's type\n");
}

static const char *bus_text(u8 bus) {
    switch(bus) {
    case BDEV_BUS_USB: return "USB";
    case BDEV_BUS_ATA: return "ATA (internal)";
    case BDEV_BUS_RAM: return "RAM";
    }
    return "unknown bus";
}

static void show_disk(struct bdev *d) {
    char size[8];
    part_fmt_size(d->nblocks, size, sizeof(size));
    printf("    %s: %s, %s (%lu blocks), %s%s\n", d->name, d->model[0] ? d->model : "no model name", size,
           (u_long)d->nblocks, bus_text(d->bus), d->removable ? ", removable" : "");
}

static void type_text(const struct part_table *t, const struct part_entry *e, char *out, size_t n) {
    const char *name;
    if(t->scheme != PART_SCHEME_GPT) { snprintf(out, n, "%02x %s", e->type, part_type_name(t->scheme, e, false)); return; }
    name = part_gpt_type_name(e->type_guid, false);
    if(name) strlcpy(out, name, n);
    else part_guid_str(e->type_guid, out);          /* n is at least 37 */
}

static void show_table(struct bdev *d, const struct part_table *t) {
    char size[8], dev[BDEV_NAME_LEN], type[40], g[37];
    u_int i, w = 4;
    if(t->damaged) printf("    GPT partition table, damaged: neither copy is whole\n    (new mbr or new gpt replaces it)\n");
    else if(t->scheme == PART_SCHEME_NONE) printf("    no partition table\n");
    else if(t->scheme == PART_SCHEME_MBR) printf("    MBR partition table, disk signature %08lx\n", (u_long)t->disk_sig);
    else {
        part_guid_str(t->disk_guid, g);
        printf("    GPT partition table, usable blocks %lu-%lu\n    disk GUID %s\n", (u_long)t->first_usable, (u_long)t->last_usable, g);
        if(t->backup_used) printf("    (read from the backup copy: a change writes both copies again)\n");
    }
    if(t->skipped || t->full) printf("    entries were left out (see the notes): fdisk will only replace this table\n");
    if(!t->nparts) return;
    for(i = 0; i < t->nparts; i++) {
        type_text(t, &t->part[i], type, sizeof(type));
        if(strlen(type) > w) w = strlen(type);
    }
    if(t->scheme == PART_SCHEME_GPT) printf("    %3s  %-8s %9s %9s %5s  %-*s  NAME\n", "#", "DEVICE", "START", "END", "SIZE", w, "TYPE");
    else printf("    %3s  %-8s %9s %9s %5s  TYPE\n", "#", "DEVICE", "START", "END", "SIZE");
    for(i = 0; i < t->nparts; i++) {
        const struct part_entry *e = &t->part[i];
        const char *last = t->scheme == PART_SCHEME_GPT ? e->name : e->active ? "active" : "";
        struct bdev *p;
        bdev_part_name(d, e->index, dev, sizeof(dev));
        p = bdev_find(dev);
        if(e->extended || !p || p->parent != d || p->start != e->start || p->nblocks != e->count) strlcpy(dev, "-", sizeof(dev));
        part_fmt_size(e->count, size, sizeof(size));
        type_text(t, e, type, sizeof(type));
        printf("    %3u  %-8s %9lu %9lu %5s  ", e->index, dev, (u_long)e->start, (u_long)(e->start + e->count - 1), size);
        if(last[0]) printf("%-*s  %s\n", w, type, last);
        else printf("%s\n", type);
    }
}

static void show_free(const struct part_table *t) {
    struct part_region r[8];
    u_int i, n = part_free(t, r, 8);
    char size[8];
    if(t->scheme == PART_SCHEME_NONE || t->damaged) return;
    if(!n) printf("    free: none (no room for a 1M partition)\n");
    for(i = 0; i < n; i++) {
        part_fmt_size(r[i].count, size, sizeof(size));
        printf("    free: blocks %lu-%lu (%s)\n", (u_long)r[i].start, (u_long)(r[i].start + r[i].count - 1), size);
    }
}

static void show_result(struct bdev *d) {
    struct bdev *p;
    char size[8];
    u_int n = 0;
    printf("    %s now has", d->name);
    for(p = bdev_first(); p; p = p->next) {
        if(p->parent != d) continue;
        part_fmt_size(p->nblocks, size, sizeof(size));
        printf("%s %s (%s)", n++ ? "," : "", p->name, size);
    }
    printf(n ? "\n" : " no partitions\n");
}

/* the disk's name typed back, exactly */
static bool confirmed(struct bdev *d) {
    struct tty *tty = (curTask && curTask->tty) ? curTask->tty : tty_active;
    char size[8], line[BDEV_NAME_LEN + 8];
    part_fmt_size(d->nblocks, size, sizeof(size));
    printf("    Type %s to write the new partition table to %s (%s, %s): ", d->name, d->name, d->model, size);
    tty_readline(tty, line, sizeof(line));
    return !strcmp(line, d->name);
}

static int number(const char *s, u_int *n) {
    char *end;
    u_long v = strtoul(s, &end, 10);
    if(!*s || *end || v < 1 || v > 255) return -1;
    *n = (u_int)v;
    return 0;
}

/* the plan for argv[2...]: CMD_OK, or after saying what is wrong CMD_USAGE (not a change fdisk
   knows) or CMD_FAIL (a change that cannot be made) */
static int make_plan(struct bdev *d, const struct part_table *t, int argc, char **argv, struct part_plan *plan) {
    struct part_io io;
    struct part_kind kind;
    char err[120];
    u32 count = 0;
    u_int index;
    int i, r;
    bool sized = false, typed = false;

    part_io_init(&io, d, true);
    if(!strcmp(argv[2], "new") && argc == 4 && (!strcmp(argv[3], "mbr") || !strcmp(argv[3], "gpt"))) {
        r = part_plan_new(&io, d->nblocks, argv[3][0] == 'm' ? PART_SCHEME_MBR : PART_SCHEME_GPT, plan, err, sizeof(err));
    } else if(!strcmp(argv[2], "add") && argc <= 5) {
        part_parse_type("fat32", &kind);
        for(i = 3; i < argc; i++) {                 /* a size and a type, in either order */
            if(!sized && part_parse_size(argv[i], &count) == 0) sized = true;
            else if(!typed && part_parse_type(argv[i], &kind) == 0) typed = true;
            else {
                printf("    fdisk: '%s' is neither a size (500M, 2G, all) nor a type\n", argv[i]);
                printf("    (fat32, fat16, linux, efi, or an MBR type byte in hex)\n");
                return CMD_USAGE;
            }
        }
        r = part_plan_add(&io, t, count, &kind, plan, err, sizeof(err));
    } else if(!strcmp(argv[2], "delete") && argc == 4 && number(argv[3], &index) == 0) {
        r = part_plan_delete(&io, t, index, plan, err, sizeof(err));
    } else if(!strcmp(argv[2], "type") && argc == 5 && number(argv[3], &index) == 0) {
        if(part_parse_type(argv[4], &kind) != 0) {
            printf("    fdisk: no type '%s' (fat32, fat16, linux, efi, or an MBR type byte in hex)\n", argv[4]);
            return CMD_USAGE;
        }
        r = part_plan_type(&io, t, index, &kind, plan, err, sizeof(err));
    } else {
        usage();
        return CMD_USAGE;
    }
    if(!r) return CMD_OK;
    printf("    fdisk: %s: %s\n", d->name, err);
    return CMD_FAIL;
}

int cmd_fdisk(int argc, char **argv) {
    struct part_table *t = NULL;
    struct part_plan plan;
    struct part_io io;
    struct bdev *d;
    char why[120];
    int status = CMD_FAIL;

    if(argc < 2) { usage(); return CMD_USAGE; }
    d = bdev_find(argv[1]);
    if(!d) { printf("    fdisk: no block device '%s' (lsblk lists them)\n", argv[1]); return CMD_FAIL; }
    if(d->parent) { printf("    fdisk: %s is a partition; fdisk works on whole disks (%s)\n", d->name, d->parent->name); return CMD_FAIL; }
    if(argc > 2 && bdev_may_format(d, why, sizeof(why)) != 0) {     /* the one safety rule, before anything else */
        printf("    fdisk: %s will not be changed:\n  %s\n", d->name, why);
        return CMD_FAIL;
    }
    t = (struct part_table *)kalloc(sizeof(struct part_table));
    plan.buf = (u8 *)kalloc(PART_PLAN_BUF);
    if(!t || !plan.buf) { printf("    fdisk: out of memory\n"); goto out; }
    part_io_init(&io, d, true);
    if(part_read(&io, d->nblocks, t) != 0) { printf("    fdisk: block 0 of %s cannot be read\n", d->name); goto out; }
    if(argc == 2) {
        show_disk(d);
        show_table(d, t);
        show_free(t);
        if(bdev_may_format(d, why, sizeof(why)) == 0) printf("    fdisk and mkfs may change %s\n", d->name);
        else printf("    fdisk and mkfs will not change %s:\n  %s\n", d->name, why);
        status = CMD_OK;
        goto out;
    }
    status = make_plan(d, t, argc, argv, &plan);
    if(status != CMD_OK) goto out;
    status = CMD_FAIL;
    show_disk(d);
    show_table(d, t);
    printf("    change: %s\n", plan.what);
    if(!confirmed(d)) { printf("    Cancelled\n"); goto out; }
    if(part_commit(d, t, &plan, why, sizeof(why)) != 0) {
        printf("    fdisk: %s\n", why);
        show_result(d);
        goto out;
    }
    printf("    %s: written, flushed and verified\n", d->name);
    show_result(d);
    status = CMD_OK;
out:
    kfree(plan.buf);
    kfree(t);
    return status;
}
