/*
SurfOS ring0 Shell (v0.002)
(C)2004 Brandon Burr, command table 10/2026 (roadmap U1)

A line is split into words; the first one selects a command from the table below,
the rest arrive as argv. `help` is generated from the same table.
*/

#include <blibc_common.h>
#include <stdarg.h>

#include <asm/io.h>
#include <surfos/keyboard.h>
#include <surfos/types.h>
#include <surfos/kernel.h>
#include <surfos/timer.h>
#include <surfos/ktimer.h>
#include <sys/parport.h>
#include <sys/serial.h>
#include <sys/pci.h>
#include <sys/driver.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <fs/bcache.h>
#include <surfos/process.h>
#include <net/net.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/dhcp.h>
#include <surfos/task.h>
#include <surfos/console.h>
#include <surfos/system.h>
#include <surfos/klog.h>
#include <surfos/interrupt.h>
#include <surfos/multiboot.h>

#include <mm/kalloc.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/pmm.h>

#include "shell.h"

char *prompt = "SurfOS*> ";

static const struct command commands[];
static u_int ncommands;

/**** small commands ****/

static void cmd_help(int argc, char **argv) {
    u_int i;
    if(argc > 1) {
        for(i = 0; i < ncommands; i++) {
            if(!strcmp(commands[i].name, argv[1])) {
                printf("    %s %s\n    %s\n", commands[i].name, commands[i].args, commands[i].help);
                return;
            }
        }
        printf("    no such command: %s\n", argv[1]);
        return;
    }
    printf("\n    SurfOS ring0 Debug Shell v0.009\n");
    printf("    -------------------------------\n");
    for(i = 0; i < ncommands; i++) {
        printf("    %-9s %-14s %s\n", commands[i].name, commands[i].args, commands[i].help);
    }
    printf("    -------------------------------\n");
    printf("    (C)2004 Brandon Burr.\n\n");
}

static void cmd_clear(int argc, char **argv) { clearScreen(); }

static void cmd_echo(int argc, char **argv) {
    int i;
    for(i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    printf("\n");
}

static void cmd_reboot(int argc, char **argv) {
    printf("\n    Please wait... rebooting...");
    reboot();
}

static void cmd_tick(int argc, char **argv) {
    printf("\n    Ticks: %lu\n\n", getticks());
}

static void cmd_uptime(int argc, char **argv) {
    u_long ms = uptime_ms();
    printf("    up %lu.%02lu s (%lu ticks at %i Hz), %u tasks, %u timers pending\n",
           ms / 1000, (ms % 1000) / 10, getticks(), HZ, task_count(), ktimers_count());
}

static void cmd_date(int argc, char **argv) {
    time_t t;
    get_time(&t);
    printf("    %04u-%02u-%02u %02u:%02u:%02u (CMOS clock)\n", t.year, t.month, t.date, t.hour, t.minute, t.second);
}

static void cmd_sleep(int argc, char **argv) {
    u_long ms = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000;
    u_long t0 = getticks();
    sleep_ms(ms);
    printf("    slept %lu ms (%lu ticks)\n", ms, getticks() - t0);
}

static void cmd_ps(int argc, char **argv) { print_tasks(); }

static void cmd_kill(int argc, char **argv) {
    surf_task *t;
    u_long pid;
    if(argc < 2) { printf("    usage: kill <pid>\n"); return; }
    pid = strtoul(argv[1], NULL, 10);
    t = task_find(pid);
    if(!t) { printf("    no task with pid %lu\n", pid); return; }
    if(t == idle_task || t == init_task_ptr) { printf("    not that one\n"); return; }
    printf("    killing '%s' (pid %lu)\n", t->name, pid);
    kill_task(t);
    if(t == curTask) yield();
}

static void cmd_memstat(int argc, char **argv) { printMemInfo(); }

static void cmd_heaptest(int argc, char **argv) { heaptest(); }

static void cmd_dmesg(int argc, char **argv) { klog_dump(); }

static void cmd_irqstat(int argc, char **argv) {
    int i;
    printf("    IRQ  count\n");
    for(i = 0; i < NR_IRQS; i++) if(irq_count[i]) printf("    %3i  %lu\n", i, irq_count[i]);
    printf("    spurious: %lu\n", spurious_irq_count);
}

static void cmd_bootinfo(int argc, char **argv) { mb_print(); }
static void cmd_lspci(int argc, char **argv) { pci_print(); }
static void cmd_drivers(int argc, char **argv) { print_drivers(); }

static void cmd_lpstat(int argc, char **argv) { printParStatus(); }

static void cmd_lsblk(int argc, char **argv) { bdev_print(); }

/**** network ****/

static void cmd_ifconfig(int argc, char **argv) {
    struct netdev *dev;
    ipaddr_t ip, mask, gw = 0, dns = 0;
    if(argc == 1) { netdev_print(); return; }
    dev = netdev_find(argv[1]);
    if(!dev) { printf("    no interface '%s'\n", argv[1]); return; }
    if(argc < 4 || !ip_parse(argv[2], &ip) || !ip_parse(argv[3], &mask) || (argc > 4 && !ip_parse(argv[4], &gw)) ||
       (argc > 5 && !ip_parse(argv[5], &dns))) {
        printf("    usage: ifconfig <if> <address> <netmask> [gateway] [dns]\n");
        return;
    }
    mutex_lock(&net_lock);
    dev->ip = ip;
    dev->netmask = mask;
    dev->gateway = gw;
    if(dns) dev->dns = dns;
    arp_announce(dev);
    mutex_unlock(&net_lock);
    netdev_print();
}

static void cmd_arp(int argc, char **argv) { arp_print(); }

static void cmd_dhcp(int argc, char **argv) {
    struct netdev *dev = argc > 1 ? netdev_find(argv[1]) : netdev_default();
    if(!dev) { printf("    no interface\n"); return; }
    if(dhcp_configure(dev, 8000) == 0) netdev_print();
    else printf("    dhcp: no lease\n");
}

static void cmd_nslookup(int argc, char **argv) {
    ipaddr_t ip;
    char a[16];
    if(argc < 2) { printf("    usage: nslookup <name>\n"); return; }
    if(!netdev_default() || !netdev_default()->dns) { printf("    no DNS server configured (ifconfig ... dns, or dhcp)\n"); return; }
    if(dns_resolve(argv[1], &ip, 3000) == 0) printf("    %s has address %s\n", argv[1], ipfmt(ip, a));
    else printf("    %s: not found\n", argv[1]);
}

static void cmd_ping(int argc, char **argv) {
    ipaddr_t ip;
    char a[16];
    int count = argc > 2 ? atoi(argv[2]) : 4, seq, got = 0, rtt;
    u8 ttl = 0;
    if(argc < 2) { printf("    usage: ping <address|name> [count]\n"); return; }
    if(!netdev_default() || !netdev_default()->ip) { printf("    ping: no address configured (ifconfig or dhcp first)\n"); return; }
    if(!ip_parse(argv[1], &ip) && dns_resolve(argv[1], &ip, 3000) != 0) { printf("    ping: cannot resolve %s\n", argv[1]); return; }
    printf("    PING %s: 56 data bytes\n", ipfmt(ip, a));
    for(seq = 1; seq <= count; seq++) {
        rtt = icmp_ping(ip, (u16)curTask->pid, (u16)seq, 56, 2000, &ttl);
        if(rtt >= 0) { got++; printf("    64 bytes from %s: icmp_seq=%d ttl=%u time=%d ms\n", a, seq, ttl, rtt); }
        else printf("    request timeout for icmp_seq %d\n", seq);
        if(seq < count) sleep_ms(rtt > 0 && rtt < 1000 ? 1000 - rtt : 1000);
    }
    printf("    %d packets transmitted, %d received, %d%% packet loss\n", count, got, count ? (count - got) * 100 / count : 0);
}

static void tcpecho_thread(void *arg) {
    u16 port = (u16)(u_long)arg;
    struct tcp_socket *l = tcp_listen(port), *c;
    char buf[1024], a[16];
    if(!l) { printf("tcpecho: port %u is busy\n", port); return; }
    printf("tcpecho: listening on port %u\n", port);
    for(;;) {
        u16 rp;
        u_long total = 0;
        int n;
        c = tcp_accept(l, 3600000);
        if(!c) continue;
        printf("tcpecho: connection from %s:%u\n", ipfmt(tcp_peer(c, &rp), a), rp);
        while((n = tcp_recv(c, buf, sizeof(buf), 60000)) > 0) {
            if(tcp_send(c, buf, n) != n) break;
            total += n;
        }
        tcp_close(c);
        printf("tcpecho: closed, %lu bytes echoed\n", total);
    }
}

static void cmd_tcpecho(int argc, char **argv) {
    u16 port = argc > 1 ? (u16)strtoul(argv[1], NULL, 10) : 7;
    kthread_create("tcpecho", tcpecho_thread, (void *)(u_long)port, PL_NORMAL, TF_DETACHED);
    printf("    tcpecho server started on port %u (background task)\n", port);
}

static void cmd_httpget(int argc, char **argv) {
    ipaddr_t ip;
    struct tcp_socket *s;
    const char *path = argc > 3 ? argv[3] : "/";
    char req[300], buf[513], a[16];
    u_long total = 0, t0, shown = 0;
    u16 port;
    int n;
    if(argc < 3) { printf("    usage: httpget <host> <port> [path]\n"); return; }
    if(!ip_parse(argv[1], &ip) && dns_resolve(argv[1], &ip, 3000) != 0) { printf("    httpget: cannot resolve %s\n", argv[1]); return; }
    port = (u16)strtoul(argv[2], NULL, 10);
    t0 = getticks();
    s = tcp_connect(ip, port, 5000);
    if(!s) { printf("    httpget: connection to %s:%u failed\n", ipfmt(ip, a), port); return; }
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: SurfOS/0.007\r\nConnection: close\r\n\r\n", path, argv[1]);
    if(tcp_send(s, req, strlen(req)) != (int)strlen(req)) { printf("    httpget: send failed\n"); tcp_close(s); return; }
    while((n = tcp_recv(s, buf, sizeof(buf) - 1, 10000)) > 0) {
        int i;
        total += n;
        for(i = 0; i < n && shown < 4096; i++, shown++) {
            u_char c = (u_char)buf[i];
            if(c == '\n' || c == '\t' || (c >= 32 && c < 127)) printf("%c", c);
            else if(c != '\r') printf(".");
        }
    }
    tcp_close(s);
    printf("\n    %lu bytes received in %lu ms%s\n", total, (getticks() - t0) * (1000 / HZ), n < 0 ? " (timed out)" : "");
}

static void cmd_netstat(int argc, char **argv) {
    netdev_print();
    udp_print();
    tcp_print();
}

/**** processes ****/

/* "hello" means /initrd/bin/hello unless the name has a slash in it */
static void program_path(const char *name, char *path, size_t size) {
    if(strchr(name, '/')) strlcpy(path, name, size);
    else snprintf(path, size, "/initrd/bin/%s", name);
}

static void run_program(int argc, char **argv, bool background) {
    char path[VFS_PATH_MAX];
    int pid, status = 0;
    program_path(argv[0], path, sizeof(path));
    pid = process_spawn(path, argc, argv, curTask->con, background ? TF_DETACHED : 0);
    if(pid < 0) { printf("    %s: %s\n", argv[0], strerror(pid)); return; }
    if(background) { printf("    [pid %d started]\n", pid); return; }
    if(task_wait(pid, &status) == pid) {
        if(status == -1) printf("    [pid %d killed]\n", pid);
        else printf("    [pid %d exit status %d]\n", pid, status);
    }
}

static void cmd_run(int argc, char **argv) {
    if(argc < 2) { printf("    usage: run <program> [args...]   (a name in /initrd/bin or a path)\n"); return; }
    run_program(argc - 1, argv + 1, false);
}

static void cmd_spawn(int argc, char **argv) {
    if(argc < 2) { printf("    usage: spawn <program> [args...]   (runs in the background)\n"); return; }
    run_program(argc - 1, argv + 1, true);
}

/**** files ****/

/* seconds since 1970 as "2026-10-03 16:11:00" (Howard Hinnant's civil-from-days) */
static void fmt_time(u32 t, char *buf, size_t size) {
    u32 days = t / 86400, rem = t % 86400;
    i32 z = (i32)days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    u32 doe = (u32)(z - era * 146097);
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i32 y = (i32)yoe + era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    u32 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if(m <= 2) y++;
    if(!t) { strlcpy(buf, "-", size); return; }
    snprintf(buf, size, "%04d-%02u-%02u %02u:%02u:%02u", y, m, d, rem / 3600, (rem / 60) % 60, rem % 60);
}

static void cmd_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    struct file *f;
    struct dirent de;
    struct stat st;
    char when[24];
    u_int n = 0;
    u_long total = 0;
    int r = vfs_stat(path, &st);
    if(r) { printf("    ls: %s: %s\n", path, strerror(r)); return; }
    if(st.type != VN_DIR) {
        fmt_time(st.mtime, when, sizeof(when));
        printf("    - %8lu  %s  %s\n", (u_long)st.size, when, path);
        return;
    }
    r = vfs_open(path, O_RDONLY | O_DIRECTORY, &f);
    if(r) { printf("    ls: %s: %s\n", path, strerror(r)); return; }
    while((r = vfs_readdir(f, &de)) == 0) {
        fmt_time(de.mtime, when, sizeof(when));
        printf("    %c %8lu  %-19s  %s%s\n", de.type == VN_DIR ? 'd' : '-', (u_long)de.size, when, de.name, de.type == VN_DIR ? "/" : "");
        n++;
        total += de.size;
    }
    if(r < 0) printf("    ls: %s\n", strerror(r));
    vfs_close(f);
    printf("    %u entries, %lu bytes\n", n, total);
}

static void cmd_cat(int argc, char **argv) {
    int i;
    if(argc < 2) { printf("    usage: cat <file>...\n"); return; }
    for(i = 1; i < argc; i++) {
        struct file *f;
        char buf[256];
        int r = vfs_open(argv[i], O_RDONLY, &f), last = '\n';
        if(r) { printf("    cat: %s: %s\n", argv[i], strerror(r)); continue; }
        while((r = vfs_read(f, buf, sizeof(buf))) > 0) {
            int j;
            for(j = 0; j < r; j++) {
                u_char c = (u_char)buf[j];
                if(c == '\n' || c == '\t' || (c >= 32 && c < 127)) printf("%c", c);
                else if(c != '\r') printf(".");
                last = c;
            }
        }
        if(r < 0) printf("\n    cat: %s: %s", argv[i], strerror(r));
        if(last != '\n') printf("\n");
        vfs_close(f);
    }
}

static void cmd_cd(int argc, char **argv) {
    int r = vfs_chdir(argc > 1 ? argv[1] : "/");
    if(r) printf("    cd: %s: %s\n", argc > 1 ? argv[1] : "/", strerror(r));
}

static void cmd_pwd(int argc, char **argv) { printf("    %s\n", vfs_getcwd()); }

static void cmd_stat(int argc, char **argv) {
    struct stat st;
    char when[24];
    int r;
    if(argc < 2) { printf("    usage: stat <path>\n"); return; }
    r = vfs_stat(argv[1], &st);
    if(r) { printf("    stat: %s: %s\n", argv[1], strerror(r)); return; }
    fmt_time(st.mtime, when, sizeof(when));
    printf("    %s: %s, %lu bytes, modified %s, inode %lu on %s (%s)\n", argv[1], st.type == VN_DIR ? "directory" : "file",
           (u_long)st.size, when, (u_long)st.ino, st.dev[0] ? st.dev : "none", st.fs);
}

static void cmd_mount(int argc, char **argv) {
    const struct mount *m;
    int r;
    if(argc == 1) {
        for(m = vfs_mounts(); m; m = m->next)
            printf("    %-6s on %-12s type %s%s\n", m->sb->dev ? m->sb->dev->name : "none", m->path, m->sb->type->name, m->sb->readonly ? " (ro)" : "");
        return;
    }
    if(argc < 3) { printf("    usage: mount [<device> <path> [type]]\n"); return; }
    {
        struct bdev *d = bdev_find(argv[1]);
        if(!d) { printf("    mount: no block device '%s'\n", argv[1]); return; }
        r = vfs_mount(d, argv[2], argc > 3 ? argv[3] : NULL);
        if(r) printf("    mount: %s\n", strerror(r));
    }
}

static void cmd_umount(int argc, char **argv) {
    int r;
    if(argc < 2) { printf("    usage: umount <path>\n"); return; }
    r = vfs_umount(argv[1]);
    if(r) printf("    umount: %s: %s\n", argv[1], strerror(r));
}

static void cmd_sync(int argc, char **argv) {
    u_long h, m, w;
    int r = vfs_sync();
    bcache_stats(&h, &m, &w);
    printf("    %s; block cache: %lu hits, %lu misses, %lu blocks written\n", r ? strerror(r) : "synced", h, m, w);
}

static void cmd_mkdir(int argc, char **argv) {
    int r;
    if(argc < 2) { printf("    usage: mkdir <dir>\n"); return; }
    r = vfs_mkdir(argv[1]);
    if(r) printf("    mkdir: %s: %s\n", argv[1], strerror(r));
}

static void cmd_rm(int argc, char **argv) {
    int i;
    if(argc < 2) { printf("    usage: rm <file>...\n"); return; }
    for(i = 1; i < argc; i++) {
        int r = vfs_unlink(argv[i]);
        if(r) printf("    rm: %s: %s\n", argv[i], strerror(r));
    }
}

static void cmd_rmdir(int argc, char **argv) {
    int r;
    if(argc < 2) { printf("    usage: rmdir <dir>\n"); return; }
    r = vfs_rmdir(argv[1]);
    if(r) printf("    rmdir: %s: %s\n", argv[1], strerror(r));
}

/* write <file> words...: the words, space separated, plus a newline (there is no shell redirection) */
static void cmd_write(int argc, char **argv) {
    struct file *f;
    int r, i;
    if(argc < 3) { printf("    usage: write <file> <text>...\n"); return; }
    r = vfs_open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, &f);
    if(r) { printf("    write: %s: %s\n", argv[1], strerror(r)); return; }
    for(i = 2; i < argc && r >= 0; i++) {
        r = vfs_write(f, argv[i], strlen(argv[i]));
        if(r >= 0) r = vfs_write(f, i + 1 < argc ? " " : "\n", 1);
    }
    if(r < 0) printf("    write: %s\n", strerror(r));
    r = vfs_close(f);
    if(r) printf("    write: close: %s\n", strerror(r));
}

static void cmd_cp(int argc, char **argv) {
    struct file *in, *out;
    char *buf;
    u_long total = 0;
    int r, w;
    if(argc < 3) { printf("    usage: cp <source> <destination>\n"); return; }
    r = vfs_open(argv[1], O_RDONLY, &in);
    if(r) { printf("    cp: %s: %s\n", argv[1], strerror(r)); return; }
    r = vfs_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, &out);
    if(r) { printf("    cp: %s: %s\n", argv[2], strerror(r)); vfs_close(in); return; }
    buf = (char *)kalloc(4096);
    if(!buf) { printf("    cp: out of memory\n"); vfs_close(in); vfs_close(out); return; }
    while((r = vfs_read(in, buf, 4096)) > 0) {
        w = vfs_write(out, buf, r);
        if(w != r) { printf("    cp: write: %s\n", w < 0 ? strerror(w) : "short write"); r = -1; break; }
        total += r;
    }
    if(r < 0 && r != -1) printf("    cp: read: %s\n", strerror(r));
    kfree(buf);
    vfs_close(in);
    r = vfs_close(out);
    if(r) printf("    cp: close: %s\n", strerror(r));
    else if(r == 0) printf("    %lu bytes copied\n", total);
}

static void cmd_hexdump(int argc, char **argv) {
    struct bdev *d;
    u_long lba, nblk, i, j;
    u8 *buf;
    int rc;
    if(argc < 2) { printf("    usage: hexdump <device> [block] [count]   (lsblk lists the devices)\n"); return; }
    d = bdev_find(argv[1]);
    if(!d) {                                            /* not a device: a file, 512 bytes at an offset */
        struct file *f;
        u_long off = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
        int r = vfs_open(argv[1], O_RDONLY, &f);
        if(r) { printf("    no block device or file '%s' (%s)\n", argv[1], strerror(r)); return; }
        buf = (u8 *)kalloc(512);
        if(!buf) { vfs_close(f); return; }
        vfs_lseek(f, (i32)off, SEEK_SET);
        r = vfs_read(f, buf, 512);
        for(i = 0; (int)i < r; i += 16) {
            printf("%08lx  ", off + i);
            for(j = 0; j < 16; j++) { if(i + j < (u_long)r) printf("%02x%s", buf[i + j], j == 7 ? "  " : " "); else printf("   %s", j == 7 ? " " : ""); }
            printf(" |");
            for(j = 0; j < 16 && i + j < (u_long)r; j++) printf("%c", (buf[i + j] >= 32 && buf[i + j] < 127) ? buf[i + j] : '.');
            printf("|\n");
        }
        if(r < 0) printf("    read failed: %s\n", strerror(r));
        kfree(buf);
        vfs_close(f);
        return;
    }
    lba = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    nblk = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
    if(nblk < 1 || nblk > 8) { printf("    1 to 8 blocks at a time\n"); return; }
    buf = (u8 *)kalloc(nblk * d->block_size);
    if(!buf) { printf("    out of memory\n"); return; }
    rc = bdev_read(d, lba, nblk, buf);
    if(rc) printf("    read of %s block %lu failed (%i)\n", d->name, lba, rc);
    else for(i = 0; i < nblk * d->block_size; i += 16) {
        printf("%08lx  ", lba * d->block_size + i);
        for(j = 0; j < 16; j++) printf("%02x%s", buf[i + j], j == 7 ? "  " : " ");
        printf(" |");
        for(j = 0; j < 16; j++) printf("%c", (buf[i + j] >= 32 && buf[i + j] < 127) ? buf[i + j] : '.');
        printf("|\n");
    }
    kfree(buf);
}

static void cmd_test(int argc, char **argv) {
    void *tmp = palloc(1024);
    void *tmp2 = mm_lookup_linear(tmp);
    printf("0x%lx linear is using physical 0x%lx\n", (u_long)tmp, (u_long)tmp2);
    pfree(tmp);
}

static void cmd_beep(int argc, char **argv) { demoBeep(); }
static void cmd_funky(int argc, char **argv) { new_task("funky", conActive, KERNEL, PL_NORMAL, (u_long*)funky); }
static void cmd_die(int argc, char **argv) { new_task("ring3", conActive, USER, PL_NORMAL, (u_long*)funky); } /* a ring-3 task: faults until P1 exists */
static void cmd_demo(int argc, char **argv) { invokeDemo(); }
static void cmd_hanoi(int argc, char **argv) { runHanoi(); }

static void cmd_selftest(int argc, char **argv) { run_selftest(); }

/* exercise the trap framework: real exceptions in this task */
static void cmd_crashdiv(int argc, char **argv) {
    volatile int one = 1, zero = 0; /* two volatiles: gcc folds 1/x into compares with no idiv */
    printf("%i\n", one/zero);
}
static void cmd_crashgp(int argc, char **argv) { asm volatile("mov %0, %%ds" :: "r"(0x1234)); }
static void cmd_crashint(int argc, char **argv) {
    asm volatile("int $0x50");
    printf("    int 0x50 returned\n");
}
static void cmd_crashnull(int argc, char **argv) { *(volatile int *)0 = 1; } /* page 0 is unmapped */

static const struct command commands[] = {
    { "help",      "[command]", "this menu, or one command in detail", cmd_help },
    { "clear",     "",          "clear the active console", cmd_clear },
    { "echo",      "words...",  "print the arguments", cmd_echo },
    { "ps",        "",          "list tasks", cmd_ps },
    { "kill",      "<pid>",     "kill a task", cmd_kill },
    { "sleep",     "[ms]",      "sleep this shell for a while", cmd_sleep },
    { "tick",      "",          "print the tick count", cmd_tick },
    { "uptime",    "",          "time since boot", cmd_uptime },
    { "date",      "",          "CMOS clock", cmd_date },
    { "memstat",   "",          "memory statistics", cmd_memstat },
    { "heaptest",  "",          "random allocations with patterns, then verify the heap", cmd_heaptest },
    { "selftest",  "",          "run the kernel self tests", cmd_selftest },
    { "dmesg",     "",          "kernel log", cmd_dmesg },
    { "irqstat",   "",          "interrupt counts", cmd_irqstat },
    { "bootinfo",  "",          "what the boot loader passed", cmd_bootinfo },
    { "lspci",     "",          "PCI devices with names", cmd_lspci },
    { "drivers",   "",          "driver init status", cmd_drivers },
    { "lpstat",    "",          "parallel port status", cmd_lpstat },
    { "lsblk",     "",          "block devices: disks, partitions, ramdisks", cmd_lsblk },
    { "mount",     "[dev path [type]]", "list mounts, or mount a block device", cmd_mount },
    { "umount",    "<path>",    "unmount", cmd_umount },
    { "ls",        "[path]",    "list a directory", cmd_ls },
    { "cat",       "<file>...", "print files", cmd_cat },
    { "cd",        "[dir]",     "change the current directory", cmd_cd },
    { "pwd",       "",          "print the current directory", cmd_pwd },
    { "stat",      "<path>",    "size, time and location of a file", cmd_stat },
    { "cp",        "<src> <dst>", "copy a file", cmd_cp },
    { "write",     "<file> <text>...", "create a file with one line of text", cmd_write },
    { "mkdir",     "<dir>",     "create a directory", cmd_mkdir },
    { "rm",        "<file>...", "delete files", cmd_rm },
    { "rmdir",     "<dir>",     "delete an empty directory", cmd_rmdir },
    { "sync",      "",          "write cached blocks to the disks", cmd_sync },
    { "ifconfig",  "[if ip mask [gw] [dns]]", "show or set the network configuration", cmd_ifconfig },
    { "arp",       "",          "the ARP table", cmd_arp },
    { "dhcp",      "[if]",      "get an address lease", cmd_dhcp },
    { "nslookup",  "<name>",    "resolve a host name", cmd_nslookup },
    { "tcpecho",   "[port]",    "start a TCP echo server (port 7)", cmd_tcpecho },
    { "httpget",   "<host> <port> [path]", "fetch a page over TCP", cmd_httpget },
    { "ping",      "<ip> [n]",  "ICMP echo", cmd_ping },
    { "netstat",   "",          "interfaces and sockets", cmd_netstat },
    { "run",       "<prog> [args]", "run a user program and wait (also: just type its name)", cmd_run },
    { "spawn",     "<prog> [args]", "run a user program in the background", cmd_spawn },
    { "hexdump",   "<dev> [blk] [n]", "dump blocks of a block device", cmd_hexdump },
    { "test",      "",          "DMA heap allocation and physical lookup", cmd_test },
    { "beep",      "",          "beep the PC speaker", cmd_beep },
    { "funky",     "",          "play a tune in a new task", cmd_funky },
    { "die",       "",          "start a ring-3 task at a kernel address (killed at once: processes use run)", cmd_die },
    { "demo",      "",          "the 2004 demonstration menu", cmd_demo },
    { "hanoi",     "",          "Towers of Hanoi", cmd_hanoi },
    { "crashdiv",  "",          "divide by zero in this shell", cmd_crashdiv },
    { "crashgp",   "",          "general protection fault in this shell", cmd_crashgp },
    { "crashnull", "",          "write through a NULL pointer", cmd_crashnull },
    { "crashint",  "",          "raise an unused vector", cmd_crashint },
    { "reboot",    "",          "reboot the machine", cmd_reboot },
};

/**** line handling ****/

static int split_args(char *line, char **argv) {
    int argc = 0;
    char *save;
    char *tok = strtok_r(line, " \t", &save);
    while(tok && argc < SHELL_MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return argc;
}

void parseCommand(char *line) {
    char *argv[SHELL_MAX_ARGS];
    int argc = split_args(line, argv);
    u_int i;

    if(!argc) return; /* no command entered */
    for(i = 0; i < ncommands; i++) {
        if(!strcmp(commands[i].name, argv[0])) {
            commands[i].fn(argc, argv);
            return;
        }
    }
    {   /* not built in: a user program in /initrd/bin? */
        char path[VFS_PATH_MAX];
        struct stat st;
        program_path(argv[0], path, sizeof(path));
        if(vfs_stat(path, &st) == 0 && st.type == VN_FILE) { run_program(argc, argv, false); return; }
    }
    printf("    Invalid command: %s (try help)\n", argv[0]);
}

/* the shell task body (started by init for each console) */
void startShell(void);

void shell() {
    do_banner();
    startShell();
}

void startShell() {
    char line[SHELL_LINE_LEN];
    ncommands = sizeof(commands) / sizeof(commands[0]);
    for(;;) {
        cputs(LBLUE_TXT, prompt);
        memset(line, 0, sizeof(line));
        cgets(TEAL_TXT, line);
        parseCommand(line);
    }
}
