/*
SurfOS ring0 Shell (v0.002)
(C)2004 Brandon Burr, command table 10/2026 (roadmap U1)

A line is split into words; the first one selects a command from the table below,
the rest arrive as argv. `help` is generated from the same table.

10/6/26 (surfsh): every command returns a status (shell.h): 0 success, 1 failure,
2 a usage error. A word $? is the status of the previous command (`echo $?`); a line
that is no command is 127, and a program run by its name gives its exit status.
The prompt is the SurfOS prompt (shell/prompt.c): this shell has system privileges,
so it is elevated, "SURF ≋#>", or "SURF ≋?>" as init's recovery console when the user
shell cannot run; each command's output is paged (TTY_PAGE). The user
shell, surfsh, runs these commands in its own task through SYS_kcmd and lists them
through SYS_kcmdinfo (kernel/syscall.c, surfos/kcmd.h): shell_run_command() and
shell_command_info() below.

10/6/26: a command is a call (surfos/task.h): a kill of its task waits for its end, and
Ctrl+C cuts it short (status 130). What waits for the user (a question, a key, a sleep,
a ping) gives up when that happens (task_interrupted()), along its usual way out.
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
#include <sys/pciinfo.h>
#include <sys/usb.h>
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
#include <surfos/tty.h>
#include <surfos/system.h>
#include <surfos/klog.h>
#include <surfos/interrupt.h>
#include <surfos/multiboot.h>
#include <surfos/cpu.h>
#include <surfos/acpi.h>
#include <surfos/version.h>
#include <surfos/prompt.h>
#include <surfos/kcmd.h>

#include <mm/kalloc.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/pmm.h>

#include "shell.h"

static const struct command commands[];
static const u_int ncommands;           /* both defined after the commands */

/* 10/6/26: a question, seen and answered on this task's console even while a kernel command
   run by the user shell has its output kept for a file (SYS_kcmd: console_capture_begin()).
   A kill or Ctrl+C while it waits leaves the answer empty: no question is answered so. */
void shell_ask(const char *question, char *answer, size_t size) {
    struct kcapture *kept = curTask->kcap;
    struct tty *tty = curTask->tty ? curTask->tty : tty_active;
    console_capture_end();
    if(console_paging(curTask->con)) console_page_new(curTask->con);   /* seen after q too, from a new screenful */
    printf("%s", question);
    memset(answer, 0, size);
    if(tty_readline(tty, answer, size) < 0 && !(curTask->intr & TI_INT)) printf("\n");   /* killed: the line ends */
    if(kept) console_capture_begin(kept);
}

/* 10/9/26 (Wi-Fi): shell_ask() for a secret, a network's password. The terminal's echo is off
   for that one line (tty_readline_secret(), kernel/tty.c): no key typed is shown, on the
   screen, the serial line or a telnet session (telnetd leaves the echo to SurfOS: WILL ECHO),
   the line goes into no history, and each key is wiped from the input queue as it is read.
   Nothing of it is printed or logged here; the caller wipes answer as soon as it is done. */
void shell_ask_secret(const char *question, char *answer, size_t size) {
    struct kcapture *kept = curTask->kcap;
    struct tty *tty = curTask->tty ? curTask->tty : tty_active;
    console_capture_end();
    if(console_paging(curTask->con)) console_page_new(curTask->con);
    printf("%s", question);
    memset(answer, 0, size);
    if(tty_readline_secret(tty, answer, size) < 0 && !(curTask->intr & TI_INT)) printf("\n");
    if(kept) console_capture_begin(kept);
}

/**** small commands ****/

static int cmd_help(int argc, char **argv) {
    u_int i;
    if(argc > 1) {
        for(i = 0; i < ncommands; i++) {
            if(!strcmp(commands[i].name, argv[1])) {
                printf("    %s %s\n    %s\n", commands[i].name, commands[i].args, commands[i].help);
                return CMD_OK;
            }
        }
        printf("    no such command: %s\n", argv[1]);
        return CMD_FAIL;
    }
    printf("\n    %s kernel shell %s\n", SURFOS_NAME, SURFOS_VERSION);
    printf("    -------------------------------\n");
    for(i = 0; i < ncommands; i++) {
        printf("    %-9s %-14s %s\n", commands[i].name, commands[i].args, commands[i].help);
    }
    printf("    -------------------------------\n");
    printf("    (C)%s %s.\n\n", SURFOS_YEARS, SURFOS_AUTHOR);
    return CMD_OK;
}

/* 10/6/26: the caller's console, not the one that shows, and as escape sequences, so the serial
   terminal clears too and `clear > file` puts them in the file: plain colours, the cursor shown,
   the screen erased, the cursor home */
static int cmd_clear(int argc, char **argv) { printf("\033[0m\033[?25h\033[2J\033[H"); return CMD_OK; }

static int cmd_echo(int argc, char **argv) {
    int i;
    for(i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    printf("\n");
    return CMD_OK;
}

static int cmd_reboot(int argc, char **argv) {
    printf("\n    Please wait... rebooting...");
    reboot();
    return CMD_FAIL;                    /* not reached: reset() halts when nothing resets the machine */
}

static int cmd_tick(int argc, char **argv) {
    printf("\n    Ticks: %lu\n\n", getticks());
    return CMD_OK;
}

static int cmd_uptime(int argc, char **argv) {
    u_long ms = uptime_ms();
    printf("    up %lu.%02lu s (%lu ticks at %i Hz), %u tasks, %u timers pending\n",
           ms / 1000, (ms % 1000) / 10, getticks(), HZ, task_count(), ktimers_count());
    return CMD_OK;
}

static int cmd_date(int argc, char **argv) {
    time_t t;
    get_time(&t);
    printf("    %04u-%02u-%02u %02u:%02u:%02u (CMOS clock)\n", t.year, t.month, t.date, t.hour, t.minute, t.second);
    return CMD_OK;
}

/* 10/6/26: a whole number, nothing else in the word */
static bool number_arg(const char *s, u_long *out) {
    char *end;
    if(!*s || *s == '-' || *s == '+') return false;
    *out = strtoul(s, &end, 10);
    return !*end;
}

/* 10/6/26: a kill or Ctrl+C cuts the sleep short. -u sleeps as a driver's delay does, where nothing cuts
   it short: a kill then comes at once after its 2 seconds (surfos/task.h shows that in the kernel log) */
static int cmd_sleep(int argc, char **argv) {
    u_long ms = 1000, t0;
    int plain = argc > 1 && !strcmp(argv[1], "-u"), r = 0;
    if(argc > 2 + plain || (argc == 2 + plain && !number_arg(argv[1 + plain], &ms))) {
        printf("    usage: sleep [-u] [ms]   (milliseconds, 1000 when not given; -u: not cut short by a kill or Ctrl+C)\n");
        return CMD_USAGE;
    }
    t0 = getticks();
    if(plain) sleep_ms(ms);
    else r = sleep_intr(ms);
    if(r) printf("    sleep cut short after %lu ms\n", (getticks() - t0) * (1000 / HZ));
    else printf("    slept %lu ms (%lu ticks)\n", ms, getticks() - t0);
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_ps(int argc, char **argv) { print_tasks(); return CMD_OK; }

static int cmd_kill(int argc, char **argv) {
    surf_task *t;
    u_long pid;
    char *end;
    if(argc < 2) { printf("    usage: kill <pid>\n"); return CMD_USAGE; }
    pid = strtoul(argv[1], &end, 10);
    if(!argv[1][0] || *end) { printf("    usage: kill <pid>   (a number: ps lists them)\n"); return CMD_USAGE; }
    t = task_find(pid);
    if(!t) { printf("    no task with pid %lu\n", pid); return CMD_FAIL; }
    if(t == idle_task || t == init_task_ptr) { printf("    not that one\n"); return CMD_FAIL; }
    printf("    killing '%s' (pid %lu)\n", t->name, pid);
    kill_task(t);                       /* 10/6/26: inside a call it ends with the call; this shell itself, after this command */
    return CMD_OK;
}

static int cmd_memstat(int argc, char **argv) { printMemInfo(); return CMD_OK; }

static int cmd_heaptest(int argc, char **argv) { return heaptest() ? CMD_FAIL : CMD_OK; }

static int cmd_dmesg(int argc, char **argv) { klog_dump(); return CMD_OK; }

static int cmd_irqstat(int argc, char **argv) {
    int i;
    printf("    IRQ  count\n");
    for(i = 0; i < NR_IRQS; i++) if(irq_count[i]) printf("    %3i  %lu\n", i, irq_count[i]);
    printf("    spurious: %lu\n", spurious_irq_count);
    return CMD_OK;
}

static int cmd_bootinfo(int argc, char **argv) {
    char buf[160];
    mb_print();
    printf("  screen: %s\n", console_describe(buf, sizeof(buf)));
    return CMD_OK;
}
/* 10/7/26: bus:device.function in hexadecimal, as lspci prints it (00:14.3) */
static bool pci_addr_arg(const char *s, u8 *bus, u8 *slot, u8 *func) {
    char *end;
    u_long b, d, f;
    if(!isxdigit((unsigned char)*s)) return false;
    b = strtoul(s, &end, 16);
    if(*end != ':' || b > 0xff || !isxdigit((unsigned char)end[1])) return false;
    d = strtoul(end + 1, &end, 16);
    if(*end != '.' || d > 0x1f || !isxdigit((unsigned char)end[1])) return false;
    f = strtoul(end + 1, &end, 16);
    if(*end || f > 7) return false;
    *bus = (u8)b;
    *slot = (u8)d;
    *func = (u8)f;
    return true;
}

/* 10/7/26: lspci [-v] [bus:device.function]: -v adds what each configuration space says; an
   address shows that device in full, with its 256 bytes */
static int cmd_lspci(int argc, char **argv) {
    struct pci_dev *d;
    u8 bus = 0, slot = 0, func = 0;
    bool verbose = false, one = false;
    int i;
    for(i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-v")) verbose = true;
        else if(!one && pci_addr_arg(argv[i], &bus, &slot, &func)) one = true;
        else {
            printf("    usage: lspci [-v] [bus:device.function]\n"
                   "    -v: the ids with the subsystem, power state, interrupts (MSI, MSI-X) and PCI Express link;\n"
                   "    an address (lspci 00:14.3): that device in full, with its configuration space\n");
            return CMD_USAGE;
        }
    }
    if(!one) {
        pci_print(verbose);
        return CMD_OK;
    }
    d = pci_find_at(bus, slot, func);
    if(!d) {
        printf("    lspci: no PCI device at %02x:%02x.%x (lspci lists them)\n", bus, slot, func);
        return CMD_FAIL;
    }
    pci_print_device(d, true, true);
    return CMD_OK;
}

/* 10/7/26: what a Wi-Fi driver for this machine needs to know about its card (docs/WIFI-PLANNING.md):
   the chipset, every wireless PCI device in full, the other network controllers, and the USB devices,
   among them the Bluetooth half of a combo card, whose id names the card. Reads only. */
#define WIFIINFO_USB 24
static int cmd_wifiinfo(int argc, char **argv) {
    struct usb_info *usb;                       /* 3.6 KB: from the heap, not the kernel stack */
    struct pci_dev *d;
    u8 cfg[PCI_CONFIG_SIZE];
    char buf[96];
    const char *hint;
    u_int n, i, wireless = 0, other = 0;

    printf("    Wi-Fi report (docs/WIFI-PLANNING.md): send all of it\n\n");
    for(d = pci_devices; d; d = d->next)
        if(d->class_code == PCI_CLASS_BRIDGE && (d->subclass == 0x00 || d->subclass == 0x01))
            printf("    chipset: %02x:%02x.%x %04x:%04x %s\n", d->bus, d->slot, d->func, d->vendor, d->device,
                   pci_class_name(d->class_code, d->subclass));

    printf("\n    Wireless PCI devices:\n");
    for(d = pci_devices; d; d = d->next) {
        pci_read_config(d, cfg);
        if(!pciinfo_is_wireless(cfg)) continue;
        wireless++;
        pci_print_device(d, true, true);
    }
    if(!wireless) printf("    none: no PCI device is a wireless network controller (class 0280 or 0d) or a known Wi-Fi chip\n");

    for(d = pci_devices; d; d = d->next) {
        if(d->class_code != PCI_CLASS_NETWORK) continue;
        pci_read_config(d, cfg);
        if(pciinfo_is_wireless(cfg)) continue;
        if(!other++) printf("\n    Other network controllers:\n");
        printf("    %02x:%02x.%x %04x:%04x, subsystem %04x:%04x, %s\n", d->bus, d->slot, d->func, d->vendor, d->device,
               d->subsys_vendor, d->subsys_id, pci_class_name(d->class_code, d->subclass));
    }

    printf("\n    USB: %s\n", usb_describe(buf, sizeof(buf)));
    usb = (struct usb_info *)kalloc(WIFIINFO_USB * sizeof(struct usb_info));
    n = usb ? usb_snapshot(usb, WIFIINFO_USB) : 0;
    if(!usb) printf("    (no memory for the list of USB devices)\n");
    for(i = 0; i < n; i++) {
        printf("    port %2u  %04x:%04x  %-8s  %s -> %s\n", usb[i].port, usb[i].vendor, usb[i].product,
               usb_class_name(usb[i].class_code), usb[i].name, usb[i].status);
        hint = pciinfo_bluetooth(usb[i].vendor, usb[i].product, usb[i].class_code);
        if(hint) printf("             probably: %s\n", hint);
    }

    printf("\n    The network's own settings (security, band, channel) need the driver to be read here:\n"
           "    on the Mac, hold Option and click the Wi-Fi icon. Never send the network's password.\n");
    kfree(usb);
    return CMD_OK;
}
static int cmd_lsusb(int argc, char **argv) { usb_print(); return CMD_OK; }
static int cmd_drivers(int argc, char **argv) { print_drivers(); return CMD_OK; }
static int cmd_cpuinfo(int argc, char **argv) { cpu_print(); return CMD_OK; }
static int cmd_acpi(int argc, char **argv) { acpi_print(); return CMD_OK; }

static int cmd_himem(int argc, char **argv) {
    char wrote[40], got[40];
    u64 phys;
    int r = himem_test(wrote, got, sizeof(wrote), &phys);
    if(r < 0) { printf("    no RAM above 4 GB is mapped (none in the memory map, or no PSE-36)\n"); return CMD_FAIL; }
    printf("    himem: wrote \"%s\" at physical 0x%llx, read back \"%s\": %s\n", wrote, phys, got, r ? "MISMATCH" : "ok");
    return r ? CMD_FAIL : CMD_OK;
}

/* 10/5/26: raw mode (tty.h) from end to end: the bytes the keys send, until q */
static int cmd_rawkeys(int argc, char **argv) {
    struct tty *t = curTask->tty;
    char buf[16];
    int n, i, reads, size = tty_ctl(t, TTY_GETSIZE, 0);
    tty_ctl(t, TTY_RAW, 1);
    printf("    raw mode (%dx%d): press keys, q ends\n    ", size >> 16, size & 0xFFFF);
    for(reads = 0; reads < 256; reads++) {
        n = tty_read(t, buf, sizeof(buf));
        if(n < 0) break;                /* 10/6/26: cut short (a kill) */
        for(i = 0; i < n; i++) printf("%02x ", (u_char)buf[i]);
        if(memchr(buf, 'q', n)) break;
    }
    tty_ctl(t, TTY_RAW, 0);
    printf("\n");
    return CMD_OK;
}

/* 10/5/26: the escape sequences the console draws (tty.h), laid out for the screen tests:
   tools/qemu-run.py reads them back from VGA memory, tools/qemu-usb.py from the framebuffer */
static int cmd_ansi(int argc, char **argv) {
    int size = tty_ctl(curTask->tty, TTY_GETSIZE, 0), cols = size >> 16, rows = size & 0xFFFF;
    printf("\033[2J\033[HANSI-TEST top left");
    printf("\033[3;10H\033[7mINVERSE\033[0m normal");
    printf("\033[4;10H\033[1;33;44mYELLOW-ON-BLUE\033[m");
    printf("\033[5;10HKEEP-ERASED\033[5;14H\033[K");
    printf("\033[8;10HA\033[BB\033[2AC\033[3DD\033[5CE");                    /* moves: D C E above A, B below */
    printf("\033[11;10H\033[5n\033[?7h\033(B\033[ q\033[38;5;196mUNKNOWN-IGNORED\033[0m");
    printf("\033[%d;%dHZ", rows, cols);                                          /* the corner, without scrolling */
    printf("\033[13;1H\033[?25l\033[?25hANSI-TEST done, %dx%d\n", cols, rows);
    return CMD_OK;
}

static int cmd_lpstat(int argc, char **argv) { printParStatus(); return CMD_OK; }

static int cmd_lsblk(int argc, char **argv) {
    if(argc > 2 || (argc == 2 && strcmp(argv[1], "-s"))) { printf("    usage: lsblk [-s]   (-s: blocks moved)\n"); return CMD_USAGE; }
    if(argc > 1) bdev_print_io();
    else bdev_print();
    return CMD_OK;
}

/**** network ****/

static int cmd_ifconfig(int argc, char **argv) {
    struct netdev *dev;
    ipaddr_t ip, mask, gw = 0, dns = 0;
    if(argc == 1) { netdev_print(); return CMD_OK; }
    dev = netdev_find(argv[1]);
    if(!dev) { printf("    no interface '%s'\n", argv[1]); return CMD_FAIL; }
    if(argc < 4 || !ip_parse(argv[2], &ip) || !ip_parse(argv[3], &mask) || (argc > 4 && !ip_parse(argv[4], &gw)) ||
       (argc > 5 && !ip_parse(argv[5], &dns))) {
        printf("    usage: ifconfig <if> <address> <netmask> [gateway] [dns]\n");
        return CMD_USAGE;
    }
    mutex_lock(&net_lock);
    dev->ip = ip;
    dev->netmask = mask;
    dev->gateway = gw;
    if(dns) dev->dns = dns;
    arp_announce(dev);
    mutex_unlock(&net_lock);
    netdev_print();
    return CMD_OK;
}

static int cmd_arp(int argc, char **argv) { arp_print(); return CMD_OK; }

static int cmd_dhcp(int argc, char **argv) {
    struct netdev *dev = argc > 1 ? netdev_find(argv[1]) : netdev_default();
    if(!dev) { printf("    no interface\n"); return CMD_FAIL; }
    if(dhcp_configure(dev, 8000) != 0) { printf("    dhcp: no lease\n"); return CMD_FAIL; }
    netdev_print();
    return CMD_OK;
}

static int cmd_nslookup(int argc, char **argv) {
    ipaddr_t ip;
    char a[16];
    if(argc < 2) { printf("    usage: nslookup <name>\n"); return CMD_USAGE; }
    if(!netdev_default() || !netdev_default()->dns) { printf("    no DNS server configured (ifconfig ... dns, or dhcp)\n"); return CMD_FAIL; }
    if(dns_resolve(argv[1], &ip, 3000) != 0) { printf("    %s: not found\n", argv[1]); return CMD_FAIL; }
    printf("    %s has address %s\n", argv[1], ipfmt(ip, a));
    return CMD_OK;
}

/* any reply is a success, as ping has it elsewhere. 10/6/26: Ctrl+C or a kill ends it at once, with the
   count of what was sent */
static int cmd_ping(int argc, char **argv) {
    ipaddr_t ip;
    char a[16];
    u_long n = 4;
    int count, seq, got = 0, sent = 0, rtt;
    u8 ttl = 0;
    if(argc < 2 || argc > 3 || (argc == 3 && (!number_arg(argv[2], &n) || n < 1 || n > 100000))) {
        printf("    usage: ping <address|name> [count]   (count: 1 or more, 4 when not given)\n");
        return CMD_USAGE;
    }
    count = (int)n;
    if(!netdev_default() || !netdev_default()->ip) { printf("    ping: no address configured (ifconfig or dhcp first)\n"); return CMD_FAIL; }
    if(!ip_parse(argv[1], &ip) && dns_resolve(argv[1], &ip, 3000) != 0) { printf("    ping: cannot resolve %s\n", argv[1]); return CMD_FAIL; }
    printf("    PING %s: 56 data bytes\n", ipfmt(ip, a));
    for(seq = 1; seq <= count && !task_interrupted(); seq++) {
        rtt = icmp_ping(ip, (u16)curTask->pid, (u16)seq, 56, 2000, &ttl);
        sent++;
        if(rtt >= 0) { got++; printf("    64 bytes from %s: icmp_seq=%d ttl=%u time=%d ms\n", a, seq, ttl, rtt); }
        else if(!task_interrupted()) printf("    request timeout for icmp_seq %d\n", seq);
        if(seq < count) sleep_intr(rtt > 0 && rtt < 1000 ? 1000 - rtt : 1000);
    }
    printf("    %d packets transmitted, %d received, %d%% packet loss\n", sent, got, sent ? (sent - got) * 100 / sent : 0);
    return got ? CMD_OK : CMD_FAIL;
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

/* the server task says itself whether it could listen: the status says it was started */
static int cmd_tcpecho(int argc, char **argv) {
    char *end = NULL;
    u_long port = argc > 1 ? strtoul(argv[1], &end, 10) : 7;
    if(argc > 2 || (end && (!argv[1][0] || *end || !port || port > 65535))) {
        printf("    usage: tcpecho [port]   (1 to 65535, default 7)\n");
        return CMD_USAGE;
    }
    if(!kthread_create("tcpecho", tcpecho_thread, (void *)port, PL_NORMAL, TF_DETACHED)) {
        printf("    tcpecho: no memory for the server task\n");
        return CMD_FAIL;
    }
    printf("    tcpecho server started on port %lu (background task)\n", port);
    return CMD_OK;
}

static int cmd_httpget(int argc, char **argv) {
    ipaddr_t ip;
    struct tcp_socket *s;
    const char *path = argc > 3 ? argv[3] : "/";
    char req[300], buf[513], a[16];
    u_long total = 0, t0, shown = 0, p = 0;
    u16 port;
    int n;
    if(argc < 3 || argc > 4 || !number_arg(argv[2], &p) || !p || p > 65535) {
        printf("    usage: httpget <host> <port> [path]   (port 1 to 65535)\n");
        return CMD_USAGE;
    }
    port = (u16)p;
    /* 10/6/26: a request that does not fit is refused, not sent cut short (the server would wait for its end) */
    n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " SURFOS_NAME "/" SURFOS_VERSION "\r\nConnection: close\r\n\r\n",
                 path, argv[1]);
    if(n < 0 || n >= (int)sizeof(req)) {
        printf("    httpget: the request would be longer than %u bytes: a shorter host name or path\n", (u_int)sizeof(req) - 1);
        return CMD_USAGE;
    }
    if(!ip_parse(argv[1], &ip) && dns_resolve(argv[1], &ip, 3000) != 0) { printf("    httpget: cannot resolve %s\n", argv[1]); return CMD_FAIL; }
    t0 = getticks();
    s = tcp_connect(ip, port, 5000);
    if(!s) { printf("    httpget: connection to %s:%u failed\n", ipfmt(ip, a), port); return CMD_FAIL; }
    if(tcp_send(s, req, strlen(req)) != (int)strlen(req)) { printf("    httpget: send failed\n"); tcp_close(s); return CMD_FAIL; }
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
    printf("\n    %lu bytes received in %lu ms%s\n", total, (getticks() - t0) * (1000 / HZ),
           n >= 0 ? "" : task_interrupted() ? " (cut short)" : " (timed out)");
    return n < 0 ? CMD_FAIL : CMD_OK;
}

static int cmd_netstat(int argc, char **argv) {
    netdev_print();
    udp_print();
    tcp_print();
    return CMD_OK;
}

/**** processes ****/

/* "hello" means /initrd/bin/hello unless the name has a slash in it */
static void program_path(const char *name, char *path, size_t size) {
    if(strchr(name, '/')) strlcpy(path, name, size);
    else snprintf(path, size, "/initrd/bin/%s", name);
}

/* 10/6/26: the status of a program in the foreground is its exit status, CMD_FAIL when it
   could not start or was killed (a status below 0 would read as an error number). A process
   running this as a kernel command (the user shell, SYS_kcmd) is the program's parent and
   passes its descriptors 0, 1 and 2 on: `run hello > out.txt` puts hello's output in the file.
   A program in the background stays detached (init collects it), wherever spawn ran. */
static int run_program(int argc, char **argv, bool background) {
    char path[VFS_PATH_MAX];
    int pid, status = 0;
    program_path(argv[0], path, sizeof(path));
    pid = process_spawn_files(path, argc, argv, curTask->con, background ? TF_DETACHED : 0, curTask->uvm ? curTask->files : NULL);
    if(pid < 0) { printf("    %s: %s\n", argv[0], strerror(pid)); return CMD_FAIL; }
    if(background) { printf("    [pid %d started]\n", pid); return CMD_OK; }
    if(task_wait_opts(pid, &status, WINTR) != pid) return CMD_FAIL;   /* 10/6/26: this task is killed; the program stays */
    if(status == -1) { printf("    [pid %d killed]\n", pid); return CMD_FAIL; }
    printf("    [pid %d exit status %d]\n", pid, status);
    return status < 0 ? CMD_FAIL : status;
}

static int cmd_run(int argc, char **argv) {
    if(argc < 2) { printf("    usage: run <program> [args...]   (a name in /initrd/bin or a path)\n"); return CMD_USAGE; }
    return run_program(argc - 1, argv + 1, false);
}

static int cmd_spawn(int argc, char **argv) {
    if(argc < 2) { printf("    usage: spawn <program> [args...]   (runs in the background)\n"); return CMD_USAGE; }
    return run_program(argc - 1, argv + 1, true);
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

static int cmd_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    struct file *f;
    struct dirent de;
    struct stat st;
    char when[24];
    u_int n = 0;
    u_long total = 0;
    int r = vfs_stat(path, &st);
    if(r) { printf("    ls: %s: %s\n", path, strerror(r)); return CMD_FAIL; }
    if(st.type != VN_DIR) {
        fmt_time(st.mtime, when, sizeof(when));
        printf("    - %8lu  %s  %s\n", (u_long)st.size, when, path);
        return CMD_OK;
    }
    r = vfs_open(path, O_RDONLY | O_DIRECTORY, &f);
    if(r) { printf("    ls: %s: %s\n", path, strerror(r)); return CMD_FAIL; }
    while((r = vfs_readdir(f, &de)) == 0) {
        fmt_time(de.mtime, when, sizeof(when));
        printf("    %c %8lu  %-19s  %s%s\n", de.type == VN_DIR ? 'd' : '-', (u_long)de.size, when, de.name, de.type == VN_DIR ? "/" : "");
        n++;
        total += de.size;
    }
    if(r < 0) printf("    ls: %s\n", strerror(r));
    vfs_close(f);
    printf("    %u entries, %lu bytes\n", n, total);
    return r < 0 ? CMD_FAIL : CMD_OK;
}

static int cmd_cat(int argc, char **argv) {
    int i, status = CMD_OK;
    if(argc < 2) { printf("    usage: cat <file>...\n"); return CMD_USAGE; }
    for(i = 1; i < argc; i++) {
        struct file *f;
        char buf[256];
        int r = vfs_open(argv[i], O_RDONLY, &f), last = '\n';
        if(r) { printf("    cat: %s: %s\n", argv[i], strerror(r)); status = CMD_FAIL; continue; }
        while((r = vfs_read(f, buf, sizeof(buf))) > 0 && !task_interrupted()) {   /* 10/6/26: /dev/zero ends too */
            int j;
            for(j = 0; j < r; j++) {
                u_char c = (u_char)buf[j];
                if(c == '\n' || c == '\t' || (c >= 32 && c < 127) || c >= 0x80) printf("%c", c);  /* 10/6/26: UTF-8 too */
                else if(c != '\r') printf(".");
                last = c;
            }
        }
        if(r < 0) { printf("\n    cat: %s: %s", argv[i], strerror(r)); status = CMD_FAIL; }
        if(last != '\n') printf("\n");
        vfs_close(f);
    }
    return status;
}

static int cmd_cd(int argc, char **argv) {
    int r = vfs_chdir(argc > 1 ? argv[1] : "/");
    if(r) printf("    cd: %s: %s\n", argc > 1 ? argv[1] : "/", strerror(r));
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_pwd(int argc, char **argv) { printf("    %s\n", vfs_getcwd()); return CMD_OK; }

static int cmd_stat(int argc, char **argv) {
    struct stat st;
    char when[24];
    int r;
    if(argc < 2) { printf("    usage: stat <path>\n"); return CMD_USAGE; }
    r = vfs_stat(argv[1], &st);
    if(r) { printf("    stat: %s: %s\n", argv[1], strerror(r)); return CMD_FAIL; }
    fmt_time(st.mtime, when, sizeof(when));
    printf("    %s: %s, %lu bytes, modified %s, inode %lu on %s (%s)\n", argv[1], st.type == VN_DIR ? "directory" : "file",
           (u_long)st.size, when, (u_long)st.ino, st.dev[0] ? st.dev : "none", st.fs);
    return CMD_OK;
}

static int cmd_sync(int argc, char **argv) {
    u_long h, m, w;
    int r = vfs_sync();
    bcache_stats(&h, &m, &w);
    printf("    %s; block cache: %lu hits, %lu misses, %lu blocks written\n", r ? strerror(r) : "synced", h, m, w);
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_mkdir(int argc, char **argv) {
    int r;
    if(argc < 2) { printf("    usage: mkdir <dir>\n"); return CMD_USAGE; }
    r = vfs_mkdir(argv[1]);
    if(r) printf("    mkdir: %s: %s\n", argv[1], strerror(r));
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_rm(int argc, char **argv) {
    int i, status = CMD_OK;
    if(argc < 2) { printf("    usage: rm <file>...\n"); return CMD_USAGE; }
    for(i = 1; i < argc; i++) {
        int r = vfs_unlink(argv[i]);
        if(r) { printf("    rm: %s: %s\n", argv[i], strerror(r)); status = CMD_FAIL; }
    }
    return status;
}

static int cmd_rmdir(int argc, char **argv) {
    int r;
    if(argc < 2) { printf("    usage: rmdir <dir>\n"); return CMD_USAGE; }
    r = vfs_rmdir(argv[1]);
    if(r) printf("    rmdir: %s: %s\n", argv[1], strerror(r));
    return r ? CMD_FAIL : CMD_OK;
}

/* write <file> words...: the words, space separated, plus a newline (there is no shell redirection) */
static int cmd_write(int argc, char **argv) {
    struct file *f;
    int r, c, i;
    if(argc < 3) { printf("    usage: write <file> <text>...\n"); return CMD_USAGE; }
    r = vfs_open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, &f);
    if(r) { printf("    write: %s: %s\n", argv[1], strerror(r)); return CMD_FAIL; }
    for(i = 2; i < argc && r >= 0; i++) {
        r = vfs_write(f, argv[i], strlen(argv[i]));
        if(r >= 0) r = vfs_write(f, i + 1 < argc ? " " : "\n", 1);
    }
    if(r < 0) printf("    write: %s\n", strerror(r));
    c = vfs_close(f);
    if(c) printf("    write: close: %s\n", strerror(c));
    return r < 0 || c ? CMD_FAIL : CMD_OK;
}

static int cmd_cp(int argc, char **argv) {
    struct file *in, *out;
    char *buf;
    u_long total = 0;
    int r, w;
    if(argc < 3) { printf("    usage: cp <source> <destination>\n"); return CMD_USAGE; }
    r = vfs_open(argv[1], O_RDONLY, &in);
    if(r) { printf("    cp: %s: %s\n", argv[1], strerror(r)); return CMD_FAIL; }
    r = vfs_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, &out);
    if(r) { printf("    cp: %s: %s\n", argv[2], strerror(r)); vfs_close(in); return CMD_FAIL; }
    buf = (char *)kalloc(4096);
    if(!buf) { printf("    cp: out of memory\n"); vfs_close(in); vfs_close(out); return CMD_FAIL; }
    while((r = vfs_read(in, buf, 4096)) > 0) {
        w = vfs_write(out, buf, r);
        if(w != r) { printf("    cp: write: %s\n", w < 0 ? strerror(w) : "short write"); r = -1; break; }
        total += r;
        if(task_interrupted()) { printf("    cp: cut short after %lu bytes\n", total); r = -1; break; }   /* 10/6/26: a kill, Ctrl+C */
    }
    if(r < 0 && r != -1) printf("    cp: read: %s\n", strerror(r));
    kfree(buf);
    vfs_close(in);
    w = vfs_close(out);
    if(w) printf("    cp: close: %s\n", strerror(w));
    else if(r == 0) printf("    %lu bytes copied\n", total);
    return r || w ? CMD_FAIL : CMD_OK;
}

/* 10/5/26: how fast a disk reads (nothing is written): 64 KB requests through the block layer */
static int cmd_readbench(int argc, char **argv) {
    struct bdev *d;
    u_long mb, blocks, done = 0, t0, ms;
    u8 *buf;
    int r = 0;
    if(argc < 2) { printf("    usage: readbench <device> [MB]   (reads only; lsblk lists the devices)\n"); return CMD_USAGE; }
    d = bdev_find(argv[1]);
    if(!d) { printf("    no block device '%s'\n", argv[1]); return CMD_FAIL; }
    mb = argc > 2 ? strtoul(argv[2], NULL, 0) : 8;
    blocks = mb * 2048;
    if(blocks > d->nblocks) blocks = d->nblocks;
    buf = (u8 *)kalloc(65536);
    if(!buf) { printf("    readbench: out of memory\n"); return CMD_FAIL; }
    t0 = getticks();
    while(done < blocks && !task_interrupted()) {        /* 10/6/26: a kill or Ctrl+C stops it */
        u_long n = blocks - done > 128 ? 128 : blocks - done;
        r = bdev_read(d, (u32)done, (u32)n, buf);
        if(r) { printf("    read of block %lu failed (%i)\n", done, r); break; }
        done += n;
    }
    ms = (getticks() - t0) * (1000 / HZ);
    printf("    %lu KB from %s in %lu ms", done / 2, d->name, ms);
    if(ms) printf(": %lu KB/s", (done / 2) * 1000 / ms);
    printf("\n");
    kfree(buf);
    return r ? CMD_FAIL : CMD_OK;
}

static int cmd_hexdump(int argc, char **argv) {
    struct bdev *d;
    u_long lba, nblk, i, j;
    u8 *buf;
    int rc;
    if(argc < 2) { printf("    usage: hexdump <device> [block] [count]   (lsblk lists the devices)\n"); return CMD_USAGE; }
    d = bdev_find(argv[1]);
    if(!d) {                                            /* not a device: a file, 512 bytes at an offset */
        struct file *f;
        u_long off = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
        int r = vfs_open(argv[1], O_RDONLY, &f);
        if(r) { printf("    no block device or file '%s' (%s)\n", argv[1], strerror(r)); return CMD_FAIL; }
        buf = (u8 *)kalloc(512);
        if(!buf) { printf("    hexdump: out of memory\n"); vfs_close(f); return CMD_FAIL; }
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
        return r < 0 ? CMD_FAIL : CMD_OK;
    }
    lba = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    nblk = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
    if(nblk < 1 || nblk > 8) { printf("    1 to 8 blocks at a time\n"); return CMD_USAGE; }
    buf = (u8 *)kalloc(nblk * d->block_size);
    if(!buf) { printf("    out of memory\n"); return CMD_FAIL; }
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
    return rc ? CMD_FAIL : CMD_OK;
}

static int cmd_test(int argc, char **argv) {
    void *tmp = palloc(1024);
    void *tmp2;
    if(!tmp) { printf("    test: no DMA memory\n"); return CMD_FAIL; }
    tmp2 = mm_lookup_linear(tmp);
    printf("0x%lx linear is using physical 0x%lx\n", (u_long)tmp, (u_long)tmp2);
    pfree(tmp);
    return CMD_OK;
}

static int cmd_beep(int argc, char **argv) { demoBeep(); return CMD_OK; }
/* 10/6/26: on the caller's console, not the one that shows */
static int cmd_funky(int argc, char **argv) { return new_task("funky", curTask->con, KERNEL, PL_NORMAL, (u_long*)funky) ? CMD_OK : CMD_FAIL; }
static int cmd_die(int argc, char **argv) { return new_task("ring3", curTask->con, USER, PL_NORMAL, (u_long*)funky) ? CMD_OK : CMD_FAIL; } /* a ring-3 task: faults until P1 exists */
static int cmd_demo(int argc, char **argv) { invokeDemo(); return CMD_OK; }
static int cmd_hanoi(int argc, char **argv) { runHanoi(); return CMD_OK; }

static int cmd_selftest(int argc, char **argv) { return run_selftest() ? CMD_FAIL : CMD_OK; }

/* exercise the trap framework: real exceptions in this task (which they kill: the status of the
   three that fault is never seen; the report reaches the screen also when the user shell sent
   the output to a file, kernel/panic.c) */
static int cmd_crashdiv(int argc, char **argv) {
    volatile int one = 1, zero = 0; /* two volatiles: gcc folds 1/x into compares with no idiv */
    printf("%i\n", one/zero);
    return CMD_FAIL;
}
static int cmd_crashgp(int argc, char **argv) { asm volatile("mov %0, %%ds" :: "r"(0x1234)); return CMD_FAIL; }
static int cmd_crashint(int argc, char **argv) {
    console_capture_end();          /* 10/6/26: its report is not fatal, and still belongs on the screen */
    asm volatile("int $0x50");
    printf("    int 0x50 returned\n");
    return CMD_OK;
}
static int cmd_crashnull(int argc, char **argv) { *(volatile int *)0 = 1; return CMD_FAIL; } /* page 0 is unmapped */

static const struct command commands[] = {
    { "help",      "[command]", "this menu, or one command in detail", cmd_help },
    { "clear",     "",          "clear the console", cmd_clear },
    { "echo",      "words...",  "print the arguments", cmd_echo },
    { "ps",        "",          "list tasks", cmd_ps },
    { "kill",      "<pid>",     "kill a task", cmd_kill },
    { "sleep",     "[-u] [ms]", "sleep this shell for a while", cmd_sleep },
    { "tick",      "",          "print the tick count", cmd_tick },
    { "uptime",    "",          "time since boot", cmd_uptime },
    { "date",      "",          "CMOS clock", cmd_date },
    { "memstat",   "",          "memory statistics", cmd_memstat },
    { "heaptest",  "",          "random allocations with patterns, then verify the heap", cmd_heaptest },
    { "selftest",  "",          "run the kernel self tests", cmd_selftest },
    { "dmesg",     "",          "kernel log", cmd_dmesg },
    { "irqstat",   "",          "interrupt counts", cmd_irqstat },
    { "bootinfo",  "",          "what the boot loader passed", cmd_bootinfo },
    { "cpuinfo",   "",          "processor model and features", cmd_cpuinfo },
    { "acpi",      "",          "firmware ACPI tables (profile=laptop)", cmd_acpi },
    { "himem",     "",          "write and read RAM above 4 GB through a PSE-36 page", cmd_himem },
    { "lspci",     "[-v] [bus:dev.fn]", "PCI devices (-v: ids, power, interrupts, link)", cmd_lspci },
    { "wifiinfo",  "",          "the Wi-Fi card's ids, for a Wi-Fi driver", cmd_wifiinfo },
    { "lsusb",     "",          "USB controllers and what is plugged in", cmd_lsusb },
    { "drivers",   "",          "driver status, console, timer and keyboard", cmd_drivers },
    { "lpstat",    "",          "parallel port status", cmd_lpstat },
    { "rawkeys",   "",          "raw mode: show the bytes keys send (q ends)", cmd_rawkeys, CMD_ASKS },
    { "ansi",      "",          "draw a test pattern with escape sequences", cmd_ansi },
    { "lsblk",     "[-s]",      "disks, partitions, ramdisks (-s: blocks moved)", cmd_lsblk },
    { "fdisk",     "<disk> [cmd]", "partition tables: new mbr|gpt, add, delete, type", cmd_fdisk },
    { "mount",     "[-r] [dev [path]]", "list mounts, or mount a block device (-r: read-only)", cmd_mount },
    { "umount",    "<path|dev>", "unmount", cmd_umount },
    { "eject",     "<disk>",    "unmount all of a disk and flush it: safe to pull out", cmd_eject },
    { "df",        "",          "size, use and free space of mounted volumes", cmd_df },
    { "mkfs",      "[-t fat16|fat32] <dev> [label]", "make a FAT file system (asks first)", cmd_mkfs },
    { "ls",        "[path]",    "list a directory", cmd_ls },
    { "cat",       "<file>...", "print files", cmd_cat },
    { "cd",        "[dir]",     "change the current directory", cmd_cd },
    { "pwd",       "",          "print the current directory", cmd_pwd },
    { "stat",      "<path>",    "size, time and location of a file", cmd_stat },
    { "cp",        "<src> <dst>", "copy a file", cmd_cp },
    { "mv",        "<from> <to>", "rename or move a file or directory", cmd_mv },
    { "write",     "<file> <text>...", "create a file with one line of text", cmd_write },
    { "mkdir",     "<dir>",     "create a directory", cmd_mkdir },
    { "rm",        "<file>...", "delete files", cmd_rm },
    { "rmdir",     "<dir>",     "delete an empty directory", cmd_rmdir },
    { "sync",      "",          "write cached blocks to the disks", cmd_sync },
    { "fsbench",   "<dir> [MB]", "time writing and reading a file", cmd_fsbench },
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
    { "readbench", "<dev> [MB]", "read speed of a block device (reads only)", cmd_readbench },
    { "test",      "",          "DMA heap allocation and physical lookup", cmd_test },
    { "beep",      "",          "beep the PC speaker", cmd_beep },
    { "funky",     "",          "play a tune in a new task", cmd_funky },
    { "die",       "",          "start a ring-3 task at a kernel address (killed at once: processes use run)", cmd_die },
    { "demo",      "",          "the 2004 demonstration menu", cmd_demo, CMD_ASKS },
    { "hanoi",     "",          "Towers of Hanoi", cmd_hanoi, CMD_ASKS },
    { "crashdiv",  "",          "divide by zero in this shell", cmd_crashdiv },
    { "crashgp",   "",          "general protection fault in this shell", cmd_crashgp },
    { "crashnull", "",          "write through a NULL pointer", cmd_crashnull },
    { "crashint",  "",          "raise an unused vector", cmd_crashint },
    { "reboot",    "",          "reboot the machine", cmd_reboot },
};
static const u_int ncommands = sizeof(commands) / sizeof(commands[0]);

/**** line handling ****/

/* the words of a line; 10/6/26: a word $? becomes last, the previous command's status, in status[] */
static int split_args(char *line, char **argv, int last, char *status, size_t size) {
    int argc = 0;
    char *save;
    char *tok = strtok_r(line, " \t", &save);
    while(tok && argc < SHELL_MAX_ARGS) {
        if(!strcmp(tok, "$?")) {
            snprintf(status, size, "%d", last);
            tok = status;
        }
        argv[argc++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return argc;
}

/* 10/6/26: run the command argv[0] in the calling task; its status, -ENOENT when there is no
   such command. This shell's lines and the user shell's SYS_kcmd come here. */
int shell_run_command(int argc, char **argv) {
    u_int i;
    if(argc < 1 || !argv[0]) return -ENOENT;
    for(i = 0; i < ncommands; i++) {
        if(!strcmp(commands[i].name, argv[0])) {
            int r;
            if((commands[i].flags & CMD_ASKS) && curTask->kcap) {   /* 10/6/26: its questions would go to the file */
                struct kcapture *kept = curTask->kcap;
                console_capture_end();
                printf("    %s reads the keyboard as it goes: not with its output in a file\n", argv[0]);
                console_capture_begin(kept);
                return CMD_USAGE;
            }
            r = commands[i].fn(argc, argv);
            return r < 0 ? CMD_FAIL : r;       /* below 0 a status would read as an error number */
        }
    }
    return -ENOENT;
}

/* 10/6/26: entry index of the table, for help and Tab completion in the user shell
   (SYS_kcmdinfo); 0, or -ENOENT after the last one */
int shell_command_info(int index, struct kcmd_info *out) {
    if(index < 0 || (u_int)index >= ncommands) return -ENOENT;
    memset(out, 0, sizeof(*out));                    /* nothing of the kernel's stack goes along */
    strlcpy(out->name, commands[index].name, sizeof(out->name));
    strlcpy(out->args, commands[index].args, sizeof(out->args));
    strlcpy(out->help, commands[index].help, sizeof(out->help));
    return 0;
}

/* run one line; its status (shell.h), or last again for an empty line */
int parseCommand(char *line, int last) {
    char *argv[SHELL_MAX_ARGS], status[12];
    int argc = split_args(line, argv, last, status, sizeof(status)), r;

    if(!argc) return last; /* no command entered */
    r = shell_run_command(argc, argv);
    if(r != -ENOENT) return r;
    {   /* not built in: a user program in /initrd/bin? */
        char path[VFS_PATH_MAX];
        struct stat st;
        program_path(argv[0], path, sizeof(path));
        if(vfs_stat(path, &st) == 0 && st.type == VN_FILE) return run_program(argc, argv, false);
    }
    printf("    Invalid command: %s (try help)\n", argv[0]);
    return 127;                         /* what other shells answer for a command that is not there */
}

/* 10/6/26: the SurfOS prompt (surfos/prompt.h) for the current directory: elevated, since this
   shell has system privileges, diagnostic as a recovery console; in Unicode when the terminal
   draws it, in the SurfOS colours */
static void show_prompt(int status, int diagnostic) {
    struct prompt_ctx c;
    char buf[PROMPT_MAX];
    int caps = tty_ctl(curTask->tty, TTY_CAPS, 0);
    memset(&c, 0, sizeof(c));
    c.cwd = vfs_getcwd();
    c.last_status = status;
    c.elevated = 1;
    c.diagnostic = diagnostic;
    c.width = (int)COLUMNS;
    c.unicode = caps > 0 && (caps & TTY_CAP_UTF8);
    c.color = caps > 0 && (caps & TTY_CAP_COLOR);    /* 10/6/26: only where the console draws colours */
    prompt_render(&c, buf, sizeof(buf));        /* cut short (still terminated) only by a path longer than any is */
    puts(buf);                                  /* not printf: its buffer is shorter than PROMPT_MAX */
}

/* the shell task body, started by init for each console (kernel/task.c). 10/6/26: why is NULL
   when this shell was asked for (shell=kernel), otherwise init's reason for a recovery console,
   said once before the first prompt */
void startShell(int diagnostic);

void shell(void *why) {
    do_banner();
    if(why) {
        cputs(KERN_TXT_COLOR, (char *)why);
        printf("\n");
    }
    startShell(why != NULL);
}

/* 10/6/26: each command's output is paged (TTY_PAGE, tty.h): it stops at --More-- after a screenful.
   A line is a call (surfos/task.h): a kill of this shell waits for its end, Ctrl+C cuts it short (130). */
void startShell(int diagnostic) {
    char line[SHELL_LINE_LEN];
    int status = 0;
    for(;;) {
        show_prompt(status, diagnostic);
        memset(line, 0, sizeof(line));
        cgets(TEAL_TXT, line);
        task_call_enter(TC_COMMAND);
        tty_ctl(curTask->tty, TTY_PAGE, 1);
        status = parseCommand(line, status);
        tty_ctl(curTask->tty, TTY_PAGE, 0);
        if(task_call_leave(TC_COMMAND)) status = KCMD_INTERRUPTED;    /* a kill that waited ends this shell here */
    }
}
