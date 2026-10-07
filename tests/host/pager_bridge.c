/* Host-side test of the console and the pager (tests/host/test_pager.c): the kernel side.
   The real kernel/console.c, kernel/tty.c and driver/cp437.c are built for the host and
   linked with these stand-ins for the rest of the kernel: tasks as plain structures (the
   test's threads run them, one at a time), wait queues, a serial port and a framebuffer
   that only record, interrupts as one flag (pager_shim/surfos/irq.h). yield() hands the
   CPU back to the test, which decides who runs next. The functions sim_*() are the test's
   view; it is built against the host's C library and sees no SurfOS header. */
#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/tty.h>
#include <surfos/keyboard.h>
#include <surfos/wait.h>
#include <surfos/irq.h>
#include <surfos/interrupt.h>
#include <surfos/multiboot.h>
#include <sys/serial.h>
#include <sys/framebuffer.h>
#include <sys/cp437.h>
#include <blibc_common.h>

#define NT 8                    /* tasks: the test's enum in test_pager.c */

int sim_if = 1;                 /* the interrupt flag */
volatile int in_irq;
static int term;                /* 10/6/26: the terminal the test uses: 0 (F1), or a pty's (NUM_CONSOLES + n) */
static u_char term_esc;         /* tty_term_input()'s state for it */
struct boot_info bootinfo;
surf_task *curTask, *idle_task, *init_task_ptr;
static surf_task T[NT];
static int ascii_word;

void emu_block(int id);         /* test_pager.c: this task waits until the test resumes it */
void emu_fail(const char *why);

/**** what the console needs from the rest of the kernel ****/

static char serial_rec[1 << 16];
static int serial_len;
void serial_putc(char c) { if(serial_len < (int)sizeof(serial_rec)) serial_rec[serial_len++] = c; }
void serial_puts(const char *s) { while(*s) serial_putc(*s++); }
void serial_console_putc(int c) {           /* driver/serial.c's rules */
    switch(c) {
    case '\n': serial_putc('\r'); serial_putc('\n'); break;
    case 0x08: serial_putc('\b'); serial_putc(' '); serial_putc('\b'); break;
    case '\r': case 0x1B: serial_putc((char)c); break;
    default: if((c >= 32 && c <= 126) || c < 0 || c >= 0x80) serial_putc((char)c); break;
    }
}
void serial_console_clear(void) { serial_puts("\033[2J\033[H"); }

/* 10/6/26: a stream console's bytes for its master (kernel/pty.c's pty_out()): recorded, for any
   pseudo-terminal, as if the master read them at once */
static char stream_rec[1 << 16];
static int stream_len, stream_drop;
int pty_out(surf_console *con, const char *s, u_int n, int how) {
    (void)con;
    (void)how;
    if(stream_drop || stream_len + (int)n > (int)sizeof(stream_rec)) return 0;   /* no room at all: dropped */
    memcpy(stream_rec + stream_len, s, n);
    stream_len += (int)n;
    return 1;
}
void pty_task_gone(struct surf_task *task) { (void)task; }
static int poll_wakes;                  /* 10/6/26: poll_wake() calls, for the test */
void poll_wake(void) { poll_wakes++; }

int framebuffer_init(void) { return 0; }
void framebuffer_bind(const u8 *cells) { (void)cells; }
void framebuffer_grid(u_int *cols, u_int *rows) { *cols = 80; *rows = 25; }
int framebuffer_grow(const u8 *cells) { (void)cells; return -1; }
void framebuffer_draw_cell(u32 col, u32 row, u8 ch, u8 color) { (void)col; (void)row; (void)ch; (void)color; }
void framebuffer_present(void) {}
void framebuffer_set_cursor(u32 col, u32 row) { (void)col; (void)row; }
bool framebuffer_active(void) { return true; }
const char *framebuffer_describe(char *buf, size_t size) { if(size) buf[0] = 0; return buf; }
void framebuffer_panic(void) {}
void framebuffer_task_died(struct surf_task *t) { (void)t; }

void klog_write(const char *s) { (void)s; }
void klog_dump(void) {}
void *kalloc(size_t n) { (void)n; return NULL; }
void kfree(void *p) { (void)p; }
bool bootinfo_has_arg(const struct boot_info *bi, const char *arg) { (void)bi; return ascii_word && !strcmp(arg, "ascii"); }

void kcritical_enter(void) { if(curTask) curTask->crit++; }
void kcritical_leave(void) { if(curTask && curTask->crit) curTask->crit--; }

void task_list_append(struct task_list *l, surf_task *t) {
    t->prev = l->last;
    t->next = NULL;
    if(l->last) l->last->next = t; else l->first = t;
    l->last = t;
    l->count++;
    t->list = l;
}

void task_list_remove(struct task_list *l, surf_task *t) {
    if(t->prev) t->prev->next = t->next; else l->first = t->next;
    if(t->next) t->next->prev = t->prev; else l->last = t->prev;
    l->count--;
    t->prev = t->next = NULL;
    t->list = NULL;
}

void wq_init(wait_queue_t *q) { q->first = q->last = NULL; q->count = 0; }
u_int wq_count(wait_queue_t *q) { return q->count; }

void wait_prepare(wait_queue_t *q) {
    if(in_interrupt() || curTask->list) emu_fail("wait_prepare in an interrupt, or on a list already (BUG_ON)");
    curTask->state = TS_BLOCKED;
    task_list_append(q, curTask);
}

/* 10/6/26: a wait a kill or Ctrl+C may cut short (task.h); the test's tasks are never cut short */
void wait_prepare_intr(wait_queue_t *q) { wait_prepare(q); curTask->wait_intr = 1; }
int task_interrupt_job(struct surf_task *owner) { (void)owner; return 0; }

static void wake(surf_task *t) {
    task_list_remove(t->list, t);
    t->state = TS_READY;
}

void wake_up(wait_queue_t *q) { if(q->first) wake(q->first); }
void wake_up_all(wait_queue_t *q) { while(q->first) wake(q->first); }
void wake_up_task(struct surf_task *t) { if(t->state == TS_BLOCKED && t->list) wake(t); }

static int id_of(surf_task *t) {
    int i;
    for(i = 0; i < NT; i++) if(t == &T[i]) return i;
    return -1;
}

/* the scheduler: whoever runs next is the test's choice */
void yield(void) {
    if(in_interrupt()) emu_fail("yield in an interrupt");
    emu_block(id_of(curTask));
}

/**** the test's view ****/

/* fresh consoles and terminals, and the family: init's children are the shell and the usb
   thread; the shell's are a command (child, with a grandchild) and a background job */
void sim_init(int ascii) {
    static const int parent[NT] = { -1, -1, 1, 2, 3, 2, 1, -1 };
    int i;
    extern void init_console(void);
    extern surf_console conVideo;
    ascii_word = ascii;
    memset(T, 0, sizeof(T));
    memset(conArray, 0, sizeof(surf_console) * NUM_TTYS);         /* a scenario that ended at a question leaves nothing */
    memset(&conVideo, 0, sizeof(conVideo));
    for(i = 0; i < NT; i++) {
        T[i].pid = i;
        T[i].state = TS_RUNNING;
        T[i].con = &conArray[0];
        T[i].parent = parent[i] >= 0 ? &T[parent[i]] : NULL;
    }
    idle_task = &T[0];
    init_task_ptr = &T[1];
    curTask = &T[2];
    sim_if = 1;
    in_irq = 0;
    init_console();
    init_tty();
    for(i = 0; i < NT; i++) T[i].tty = &ttys[0];
    serial_len = 0;
    stream_len = 0;
    stream_drop = 0;
    term = 0;
    term_esc = 0;
}

/* 10/6/26: from now on the tasks are on pty n's terminal (a stream console), and so are the
   test's input and its look at the pager; n -1 is F1 again */
void sim_term(int n) {
    int i;
    term = n < 0 ? 0 : NUM_CONSOLES + n;
    for(i = 0; i < NT; i++) {
        T[i].con = &conArray[term];
        T[i].tty = &ttys[term];
    }
}

/* what the stream consoles handed to their masters since the last call */
int sim_stream(char *out, int max) {
    int n = stream_len < max - 1 ? stream_len : max - 1;
    memcpy(out, stream_rec, n);
    out[n] = 0;
    stream_len = 0;
    return n;
}

void sim_stream_drop(int on) { stream_drop = on; }   /* the master's buffer full: pty_out() drops everything */
void sim_stream_size(int cols, int rows) { console_stream_resize(&conArray[term], (u_int)cols, (u_int)rows); }
void sim_stream_pos(int *x, int *wrap) { *x = (int)conArray[term].loc.x; *wrap = conArray[term].wrap; }
void sim_term_input(int c) { tty_term_input(&ttys[term], &term_esc, (u_char)c); }
int sim_term_queues(int c) { return tty_term_queues(&ttys[term], term_esc, (u_char)c); }
int sim_room(void) { return (int)tty_room(&ttys[term]); }
int sim_poll_wakes(void) { return poll_wakes; }
void sim_clear(void) { clearConsole(curTask->con); }
int sim_key_up(void) { return UP; }
int sim_key_home(void) { return HOME; }

void sim_cur(int task) { curTask = &T[task]; T[task].state = TS_RUNNING; }
int sim_task(void) { return id_of(curTask); }
void sim_print(const char *s, int len) { int i; for(i = 0; i < len; i++) kputch(curTask->con, WHITE_TXT, s[i]); }
void sim_kprintf_line(const char *s) { kprintf("%s", s); }
int sim_ttyctl(int op, unsigned arg) { return tty_ctl(curTask->tty, op, arg); }
void sim_input(int c) { tty_input(&ttys[term], (u_char)c); }
void sim_key(int c) { tty_key(&ttys[term], (u_char)c); }
int sim_getc(void) { return tty_getc(&ttys[term]); }
int sim_readline(char *buf, int size) { return tty_readline(&ttys[term], buf, (size_t)size); }
int sim_lines(void) { return (int)LINES; }
int sim_cols(void) { return (int)COLUMNS; }

/* row r of console 0 as text (the empty cell as a space), and the attribute of its first cell */
void sim_row(int r, char *out, int *attr0) {
    int c;
    for(c = 0; c < (int)COLUMNS; c++) {
        u_char ch = conArray[0].vmem[(r * COLUMNS + c) * 2];
        out[c] = ch ? (char)ch : ' ';
    }
    out[COLUMNS] = 0;
    for(c = COLUMNS; c > 0 && out[c - 1] == ' '; c--) out[c - 1] = 0;
    if(attr0) *attr0 = conArray[0].vmem[r * COLUMNS * 2 + 1];
}

u_char sim_cell(int x, int y) { return conArray[0].vmem[(y * COLUMNS + x) * 2]; }
void sim_cursor(int *x, int *y) { *x = (int)conArray[0].loc.x; *y = (int)conArray[0].loc.y; }

/* what the serial port got since the last call */
int sim_serial(char *out, int max) {
    int n = serial_len < max - 1 ? serial_len : max - 1;
    memcpy(out, serial_rec, n);
    out[n] = 0;
    serial_len = 0;
    return n;
}

void sim_page(int *rows, int *drop, int *asker, int *owner) {
    surf_console *c = &conArray[term];
    *rows = (int)c->page_rows;
    *drop = c->page_drop;
    *asker = c->page_asker ? id_of(c->page_asker) : -1;
    *owner = c->page_owner ? id_of(c->page_owner) : -1;
}

/* a task ends: what task.c's become_zombie() does for the console (interrupts off) */
void sim_task_gone(int task) {
    u_long flags = irq_save();
    if(T[task].list) task_list_remove(T[task].list, &T[task]);
    T[task].state = TS_DEAD;
    tty_task_gone(&T[task]);
    irq_restore(flags);
}

void sim_set_if(int on) { sim_if = on; }
void sim_set_in_irq(int n) { in_irq = n; }
void sim_set_mutexes(int task, int n) { T[task].mutexes = (u_int)n; }
void sim_set_crit(int task, int n) { T[task].crit = (u_int)n; }
int sim_on_list(int task) { return T[task].list != NULL; }
int sim_readers(void) { return (int)ttys[term].readers.count; }
int sim_raw(void) { return ttys[term].raw; }

/* the current task's output kept in memory (console_capture_begin), as SYS_kcmd does it */
static struct kcapture cap;
static char cap_buf[4096];
void sim_capture(int on) {
    if(on) {
        cap.buf = cap_buf;
        cap.len = 0;
        cap.size = sizeof(cap_buf) - 1;
        cap.cut = 0;
        console_capture_begin(&cap);
    } else {
        console_capture_end();
    }
}
const char *sim_captured(void) { cap_buf[cap.len] = 0; return cap_buf; }
