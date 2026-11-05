/* The suite's host-test fakes: tests/host/gui_fake.h says what each is. */
#include "gui_fake.h"

int checks, failed;

int fake_done(const char *name) {
    printf("%s: %d checks, %d failed\n", name, checks, failed);
    return failed ? 1 : 0;
}

/**** files ****/

struct fake_node { char path[GUI_PATH_MAX]; int type; u32 at, len; bool used; };
static struct fake_node nodes[FAKE_FILES];
static char pool[FAKE_POOL];
static u32 pool_used;
int fake_fail_write;

static int find(const char *p) {
    int i;
    for(i = 0; i < FAKE_FILES; i++) if(nodes[i].used && !strcmp(nodes[i].path, p)) return i;
    return -1;
}

static bool under(const char *p, const char *top) {
    u32 n = strlen(top);
    return !strncmp(p, top, n) && (!p[n] || p[n] == '/');
}

static int add(const char *p, int type, const char *data, u32 len) {
    int i = find(p);
    if(i < 0) for(i = 0; i < FAKE_FILES && nodes[i].used; i++) ;
    if(i >= FAKE_FILES || pool_used + len > FAKE_POOL) return -ENOSPC;
    nodes[i].used = true;
    strlcpy(nodes[i].path, p, sizeof(nodes[i].path));
    nodes[i].type = type;
    nodes[i].at = pool_used;
    nodes[i].len = len;
    if(len) memcpy(pool + pool_used, data, len);
    pool_used += len;
    return 0;
}

void fake_files_reset(void) {
    memset(nodes, 0, sizeof(nodes));
    pool_used = 0;
    fake_fail_write = 0;
    add("/", GUI_DIR, NULL, 0);
    add("/initrd", GUI_DIR, NULL, 0);
    add("/home", GUI_DIR, NULL, 0);
}

void fake_file(const char *path, const char *data, u32 len) { add(path, GUI_FILE, data, len); }
void fake_dir(const char *path) { add(path, GUI_DIR, NULL, 0); }

const char *fake_data(const char *path, u32 *len) {
    int i = find(path);
    if(i < 0 || nodes[i].type != GUI_FILE) return NULL;
    *len = nodes[i].len;
    return pool + nodes[i].at;
}

static void parent_of(const char *p, char *out) { strlcpy(out, p, GUI_PATH_MAX); gui_parent(out); }

static int f_list(void *ctx, const char *path, struct gui_entry *out, int max) {
    char parent[GUI_PATH_MAX];
    int i, n = 0, d = find(path);
    (void)ctx;
    if(d < 0) return -ENOENT;
    if(nodes[d].type != GUI_DIR) return -ENOTDIR;
    for(i = 0; i < FAKE_FILES; i++) {
        if(!nodes[i].used || i == d) continue;
        parent_of(nodes[i].path, parent);
        if(strcmp(parent, path)) continue;
        if(n < max) {
            memset(&out[n], 0, sizeof(out[n]));
            strlcpy(out[n].name, gui_basename(nodes[i].path), sizeof(out[n].name));
            out[n].type = nodes[i].type;
            out[n].size = nodes[i].len;
            out[n].mtime = 1792245780;
        }
        n++;
    }
    return n;
}

static int f_stat(void *ctx, const char *path, struct gui_entry *out, bool *device, bool *readonly) {
    int i = find(path);
    (void)ctx;
    if(i < 0) return -ENOENT;
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, gui_basename(path), sizeof(out->name));
    out->type = nodes[i].type;
    out->size = nodes[i].len;
    out->mtime = 1792245780;
    *device = false;
    *readonly = under(path, "/initrd");
    return 0;
}

static int f_read(void *ctx, const char *path, char *buf, u32 size, u32 *len) {
    int i = find(path);
    (void)ctx;
    if(i < 0) return -ENOENT;
    if(nodes[i].type == GUI_DIR) return -EISDIR;
    if(nodes[i].len > size) return -EFBIG;
    memcpy(buf, pool + nodes[i].at, nodes[i].len);
    *len = nodes[i].len;
    return 0;
}

static int f_read_at(void *ctx, const char *path, u32 off, char *buf, u32 n, u32 *len, u32 *size) {
    int i = find(path);
    (void)ctx;
    *len = *size = 0;
    if(i < 0) return -ENOENT;
    if(nodes[i].type == GUI_DIR) return -EISDIR;
    *size = nodes[i].len;
    if(off >= nodes[i].len) return 0;
    *len = nodes[i].len - off < n ? nodes[i].len - off : n;
    memcpy(buf, pool + nodes[i].at + off, *len);
    return 0;
}

static int f_write(void *ctx, const char *path, const char *data, u32 len, bool crlf, char *kept, size_t n) {
    char parent[GUI_PATH_MAX];
    int p;
    (void)ctx; (void)crlf;
    if(kept && n) kept[0] = 0;
    if(fake_fail_write) return fake_fail_write;
    parent_of(path, parent);
    p = find(parent);
    if(p < 0 || nodes[p].type != GUI_DIR) return -ENOENT;
    if(under(path, "/initrd")) return -EROFS;
    return add(path, GUI_FILE, data, len);
}

static int f_mkdir(void *ctx, const char *path) {
    (void)ctx;
    if(find(path) >= 0) return -EEXIST;
    return add(path, GUI_DIR, NULL, 0);
}

static int f_remove(void *ctx, const char *path, bool dir) {
    int i = find(path);
    (void)ctx; (void)dir;
    if(i < 0) return -ENOENT;
    nodes[i].used = false;
    return 0;
}

static int f_rename(void *ctx, const char *from, const char *to) {
    int i = find(from);
    (void)ctx;
    if(i < 0) return -ENOENT;
    if(find(to) >= 0) nodes[find(to)].used = false;
    strlcpy(nodes[i].path, to, sizeof(nodes[i].path));
    return 0;
}

static int f_volumes(void *ctx, struct gui_volume *out, int max) { (void)ctx; (void)out; (void)max; return 0; }
static int f_mount(void *ctx, const char *dev, char *msg, size_t n) { (void)ctx; snprintf(msg, n, "mounted %s", dev); return 0; }
static int f_eject(void *ctx, const char *dev, char *msg, size_t n) { (void)ctx; snprintf(msg, n, "%s: safe to remove", dev); return 0; }

static const char *f_error(int err) {
    switch(err) {
    case -ENOENT: return "No such file or directory";
    case -EROFS: return "Read-only file system";
    case -EFBIG: return "File too large";
    case -ENOSPC: return "No space left on device";
    case -EIO: return "I/O error";
    }
    return "failed";
}

const struct gui_fs fake_fs = { NULL, f_list, f_stat, f_read, f_write, f_mkdir, f_remove, f_rename, f_volumes,
                                f_mount, f_eject, f_error, f_read_at };

/**** the system ****/

struct gui_task fake_tasks[GUI_TASK_MAX];
int fake_ntasks;
struct gui_memory fake_mem;
struct gui_machine fake_machine;
struct gui_disk fake_disks[GUI_DISK_MAX];
int fake_ndisks;
char fake_log[GUI_LOG_MAX];
u32 fake_log_written;
int fake_killed;
char fake_term_out[8192], fake_term_in[8192];
u32 fake_term_out_len, fake_term_in_len;
int fake_term_cols, fake_term_rows, fake_term_open_count;
bool fake_term_running, fake_term_ended;

static void wins_reset(void);           /* 10/25/26: below, with the programs' windows */

static void task(int pid, int ppid, int state, int tty, u32 flags, const char *name) {
    struct gui_task *t = &fake_tasks[fake_ntasks++];
    t->pid = pid; t->ppid = ppid; t->state = state; t->tty = tty; t->flags = flags;
    strlcpy(t->name, name, sizeof(t->name));
}

void fake_sys_reset(void) {
    fake_ntasks = 0;
    task(0, -1, GUI_TASK_READY, -1, 0, "idle");
    task(1, -1, GUI_TASK_SLEEPING, -1, GUI_TASK_USER, "init");
    task(2, 1, GUI_TASK_BLOCKED, -1, 0, "net");
    task(5, 1, GUI_TASK_RUNNING, 0, GUI_TASK_USER | GUI_TASK_SHELL, "surfsh");
    task(9, 5, GUI_TASK_SLEEPING, 1, GUI_TASK_USER, "edit");
    memset(&fake_mem, 0, sizeof(fake_mem));
    fake_mem.ram_kb = 8 * 1024 * 1024; fake_mem.free_pages = 1500000; fake_mem.total_pages = 2000000;
    fake_mem.heap_used = 3 << 20; fake_mem.heap_free = 1 << 20; fake_mem.heap_size = 64 << 20;
    fake_mem.dma_used = 1 << 20; fake_mem.dma_free = 3 << 20; fake_mem.dma_size = 4 << 20;
    memset(&fake_machine, 0, sizeof(fake_machine));
    strlcpy(fake_machine.cpu, "Intel(R) Core(TM) i3-1005G1 CPU @ 1.20GHz", sizeof(fake_machine.cpu));
    strlcpy(fake_machine.firmware, "UEFI", sizeof(fake_machine.firmware));
    strlcpy(fake_machine.profile, "laptop", sizeof(fake_machine.profile));
    strlcpy(fake_machine.video, "1920x1080x32", sizeof(fake_machine.video));
    strlcpy(fake_machine.build, "0efbade 2026-10-17 21:00", sizeof(fake_machine.build));
    fake_machine.uptime_s = 3725; fake_machine.tasks = 5; fake_machine.timers = 3;
    memset(fake_disks, 0, sizeof(fake_disks));
    fake_ndisks = 0;
    strlcpy(fake_log, "SurfOS starting\n+USB: usb0: General USB Flash Disk\n", sizeof(fake_log));
    fake_log_written = (u32)strlen(fake_log);
    fake_killed = 0;
    fake_term_out_len = fake_term_in_len = 0;
    fake_term_cols = fake_term_rows = fake_term_open_count = 0;
    fake_term_running = fake_term_ended = false;
    wins_reset();
}

void fake_term_feed(const char *s, u32 n) {
    if(fake_term_out_len + n > sizeof(fake_term_out)) n = sizeof(fake_term_out) - fake_term_out_len;
    memcpy(fake_term_out + fake_term_out_len, s, n);
    fake_term_out_len += n;
}

static int s_tasks(void *ctx, struct gui_task *out, int max) {
    int n = fake_ntasks < max ? fake_ntasks : max;
    (void)ctx;
    memcpy(out, fake_tasks, n * sizeof(*out));
    return n;
}
static int s_memory(void *ctx, struct gui_memory *out) { (void)ctx; *out = fake_mem; return 0; }
static int s_machine(void *ctx, struct gui_machine *out) { (void)ctx; *out = fake_machine; return 0; }
static int s_kill(void *ctx, int pid, char *msg, size_t n) {
    int i;
    (void)ctx;
    fake_killed = pid;
    for(i = 0; i < fake_ntasks; i++) {
        if(fake_tasks[i].pid != pid) continue;
        if(!(fake_tasks[i].flags & GUI_TASK_USER) || pid <= 1) { snprintf(msg, n, "not one to end"); return -EACCES; }
        snprintf(msg, n, "ended %s (pid %d)", fake_tasks[i].name, pid);
        memmove(&fake_tasks[i], &fake_tasks[i + 1], (fake_ntasks - i - 1) * sizeof(fake_tasks[0]));
        fake_ntasks--;
        return 0;
    }
    snprintf(msg, n, "no task with pid %d any more", pid);
    return -ENOENT;
}
static int s_disks(void *ctx, struct gui_disk *out, int max) {
    int n = fake_ndisks < max ? fake_ndisks : max;
    (void)ctx;
    memcpy(out, fake_disks, n * sizeof(*out));
    return n;
}
static int s_log(void *ctx, char *buf, u32 size, u32 *len, u32 *written) {
    u32 n = (u32)strlen(fake_log);
    (void)ctx;
    if(n > size) { memcpy(buf, fake_log + n - size, size); n = size; }
    else memcpy(buf, fake_log, n);
    *len = n;
    *written = fake_log_written;
    return 0;
}
static int s_term_open(void *ctx, int cols, int rows) {
    (void)ctx;
    if(fake_term_running) return -EAGAIN;
    fake_term_running = true;
    fake_term_ended = false;
    fake_term_cols = cols; fake_term_rows = rows;
    fake_term_open_count++;
    return 3;
}
static int s_term_read(void *ctx, int h, char *buf, u32 n) {
    (void)ctx;
    if(h != 3) return -EINVAL;
    if(!fake_term_out_len) return fake_term_ended ? -EPIPE : 0;
    if(n > fake_term_out_len) n = fake_term_out_len;
    memcpy(buf, fake_term_out, n);
    memmove(fake_term_out, fake_term_out + n, fake_term_out_len - n);
    fake_term_out_len -= n;
    return (int)n;
}
static int s_term_write(void *ctx, int h, const char *buf, u32 n) {
    (void)ctx;
    if(h != 3 || !fake_term_running) return -EPIPE;
    if(fake_term_in_len + n > sizeof(fake_term_in)) n = sizeof(fake_term_in) - fake_term_in_len;
    memcpy(fake_term_in + fake_term_in_len, buf, n);
    fake_term_in_len += n;
    return (int)n;
}
static int s_term_resize(void *ctx, int h, int cols, int rows) {
    (void)ctx;
    if(h != 3) return -EINVAL;
    fake_term_cols = cols; fake_term_rows = rows;
    return 0;
}
static void s_term_close(void *ctx, int h) {
    (void)ctx;
    if(h == 3) fake_term_running = false;
}

/* 10/25/26: programs' windows */
struct fake_win fake_wins[FAKE_WINS];
char fake_run_line[256];
int fake_run_count, fake_run_result;
static u32 win_pool[4 * 1024 * 1024];
static u32 win_pool_used;

/* 11/05/26 (track desk) */
char fake_paste[256], fake_run_program[32], fake_run_arg[256], fake_notes[4096];
u32 fake_paste_len, fake_colors[WIN_COLORS];
int fake_paste_count, fake_theme, fake_theme_count, fake_run_file_count;
int fake_pref_theme, fake_pref_speed, fake_pref_result, fake_pref_sets, fake_pref_which, fake_pref_value;

static void wins_reset(void) {
    memset(fake_wins, 0, sizeof(fake_wins));
    win_pool_used = 0;
    fake_run_line[0] = 0;
    fake_run_count = 0;
    fake_run_result = 77;
    fake_clip[0] = 0;
    fake_clip_from = -1;
    fake_paste[0] = fake_run_program[0] = fake_run_arg[0] = fake_notes[0] = 0;     /* 11/05/26 */
    fake_paste_len = 0;
    fake_paste_count = fake_theme_count = fake_run_file_count = fake_pref_sets = 0;
    fake_theme = -1;
    memset(fake_colors, 0, sizeof(fake_colors));
    fake_pref_theme = 0;
    fake_pref_speed = 1;
    fake_pref_result = 0;
    fake_pref_which = fake_pref_value = -1;
}

static u32 *pixels_for(int w, int h, u32 color) {
    u32 n = (u32)w * (u32)h, i, *p;
    if(win_pool_used + n > sizeof(win_pool) / sizeof(win_pool[0])) return NULL;
    p = win_pool + win_pool_used;
    win_pool_used += n;
    for(i = 0; i < n; i++) p[i] = color;
    return p;
}

int fake_win_open(int w, int h, u32 flags, u32 background, const char *title, int pid) {
    int i;
    for(i = 0; i < FAKE_WINS && fake_wins[i].used; i++) ;
    if(i == FAKE_WINS) return -1;
    memset(&fake_wins[i], 0, sizeof(fake_wins[i]));
    fake_wins[i].px = pixels_for(w, h, background);
    if(!fake_wins[i].px) return -1;
    fake_wins[i].used = true;
    fake_wins[i].handle = 200 + i;
    fake_wins[i].st.w = w;
    fake_wins[i].st.h = h;
    fake_wins[i].st.flags = flags;
    fake_wins[i].st.background = background;
    fake_wins[i].st.pid = pid;
    strlcpy(fake_wins[i].st.title, title, sizeof(fake_wins[i].st.title));
    return i;
}

void fake_win_present(int i, u32 color) {
    u32 k, n = (u32)fake_wins[i].st.w * (u32)fake_wins[i].st.h;
    for(k = 0; k < n; k++) fake_wins[i].px[k] = color;
    fake_wins[i].ready = true;
    fake_wins[i].st.serial++;
}

void fake_win_size(int i, int w, int h, u32 color) {
    u32 *p = pixels_for(w, h, color);
    if(!p) return;
    fake_wins[i].px = p;
    fake_wins[i].st.w = w;
    fake_wins[i].st.h = h;
    fake_wins[i].st.serial++;
}

void fake_win_end(int i) { fake_wins[i].ended = true; }

int fake_win_count(int i, int type) {
    int k, n = 0;
    for(k = 0; k < fake_wins[i].nev; k++) if(fake_wins[i].ev[k].type == type) n++;
    return n;
}

const struct win_event *fake_win_last(int i, int type) {
    int k;
    for(k = fake_wins[i].nev - 1; k >= 0; k--) if(fake_wins[i].ev[k].type == type) return &fake_wins[i].ev[k];
    return NULL;
}

void fake_win_forget(int i) { fake_wins[i].nev = 0; }

/* the window of handle h that the desktop took, not let go of: its number, -1 */
static int win_of(int h) {
    int i = h - 200;
    return i >= 0 && i < FAKE_WINS && fake_wins[i].used && fake_wins[i].accepted && !fake_wins[i].closed ? i : -1;
}

static int s_win_accept(void *ctx) {
    int i;
    (void)ctx;
    for(i = 0; i < FAKE_WINS; i++) {
        struct fake_win *w = &fake_wins[i];
        if(!w->used || !w->ready || w->accepted || w->ended) continue;
        w->accepted = true;
        return w->handle;
    }
    return -EAGAIN;
}

static int s_win_update(void *ctx, int h, struct win_state *st, struct win_view *view) {
    int i = win_of(h);
    (void)ctx;
    if(i < 0) return -EINVAL;
    if(fake_wins[i].ended) return -EPIPE;
    *st = fake_wins[i].st;
    view->pixels = fake_wins[i].px;
    view->w = fake_wins[i].st.w;
    view->h = fake_wins[i].st.h;
    view->menu = fake_wins[i].menus ? fake_wins[i].menu : NULL;     /* 11/05/26: lent, as the pixels */
    view->menus = fake_wins[i].menus;
    return 0;
}

static int s_win_event(void *ctx, int h, const struct win_event *e) {
    int i = win_of(h);
    (void)ctx;
    if(i < 0) return -EINVAL;
    if(fake_wins[i].nev < FAKE_EVENTS) fake_wins[i].ev[fake_wins[i].nev++] = *e;
    return 0;
}

static void s_win_close(void *ctx, int h) {
    int i = win_of(h);
    (void)ctx;
    if(i >= 0) fake_wins[i].closed = true;
}

static int s_run(void *ctx, const char *line, char *msg, size_t n) {
    (void)ctx;
    strlcpy(fake_run_line, line, sizeof(fake_run_line));
    fake_run_count++;
    if(fake_run_result < 0) snprintf(msg, n, "could not start %s", line);
    return fake_run_result;
}

/* 10/26/26: the text a program put on the clipboard, taken once: the desktop's when it came from the window it names
   (the one with its focus), dropped otherwise */
char fake_clip[64];
int fake_clip_from;
static int s_win_clip(void *ctx, int h, char *out, size_t n) {
    size_t len = strlen(fake_clip);
    (void)ctx;
    if(h < 0 || h != fake_clip_from) len = 0;
    if(len > n) len = n;
    memcpy(out, fake_clip, len);
    fake_clip[0] = 0;
    return (int)len;
}

/* 11/05/26 (track desk): menus, the clipboard given, choosers, the theme, a program with a file, the log, the
   preferences */
void fake_win_menus(int i, const struct win_menu_item *items, int n) {
    if(n > WIN_MENU_ENTRIES) n = WIN_MENU_ENTRIES;
    if(n > 0) memcpy(fake_wins[i].menu, items, (size_t)n * sizeof(*items));
    fake_wins[i].menus = n > 0 ? n : 0;
    fake_wins[i].st.menu_serial++;
}

void fake_win_ask(int i, const struct win_dialog *d) {
    fake_wins[i].ask = *d;
    fake_wins[i].st.dialog = 1;
    fake_wins[i].asked++;                               /* the request's number: the answer must name it */
}

static int s_win_paste(void *ctx, const char *text, u32 len) {
    (void)ctx;
    if(len >= sizeof(fake_paste)) len = sizeof(fake_paste) - 1;
    memcpy(fake_paste, text, len);
    fake_paste[len] = 0;
    fake_paste_len = len;
    fake_paste_count++;
    return 0;
}

static int s_win_dialog(void *ctx, int h, struct win_dialog *out, u32 *no) {
    int i = win_of(h);
    (void)ctx;
    if(i < 0) return -EINVAL;
    if(!fake_wins[i].st.dialog) return -ENOENT;
    *out = fake_wins[i].ask;
    *no = (u32)fake_wins[i].asked;
    fake_wins[i].st.dialog = 0;
    fake_wins[i].waiting = true;
    return 0;
}

static int s_win_answer(void *ctx, int h, u32 no, const char *path) {
    int i = win_of(h);
    struct win_event e;
    (void)ctx;
    if(i < 0) return -EINVAL;
    if(!fake_wins[i].waiting || no != (u32)fake_wins[i].asked) {   /* answered already, or not the one waiting */
        fake_wins[i].wrong++;
        return -EINVAL;
    }
    fake_wins[i].waiting = false;
    strlcpy(fake_wins[i].answer, path ? path : "", sizeof(fake_wins[i].answer));
    if(path) fake_wins[i].answers++;
    else fake_wins[i].cancels++;
    memset(&e, 0, sizeof(e));
    e.type = WIN_EV_DIALOG;
    e.value = path ? 1 : 0;
    e.x = (int)fake_wins[i].ask.id;
    if(fake_wins[i].nev < FAKE_EVENTS) fake_wins[i].ev[fake_wins[i].nev++] = e;
    return 0;
}

static int s_win_theme(void *ctx, int theme, const u32 *colors) {
    (void)ctx;
    fake_theme = theme;
    memcpy(fake_colors, colors, sizeof(fake_colors));
    fake_theme_count++;
    return 0;
}

static int s_run_file(void *ctx, const char *program, const char *arg, char *msg, size_t n) {
    (void)ctx;
    strlcpy(fake_run_program, program, sizeof(fake_run_program));
    strlcpy(fake_run_arg, arg, sizeof(fake_run_arg));
    fake_run_file_count++;
    if(fake_run_result < 0) snprintf(msg, n, "could not start %s", program);
    return fake_run_result;
}

static void s_note(void *ctx, const char *line) {
    size_t used = strlen(fake_notes);
    (void)ctx;
    snprintf(fake_notes + used, sizeof(fake_notes) - used, "%s\n", line);
}

static int s_prefs(void *ctx, int *theme, int *speed) {
    (void)ctx;
    *theme = fake_pref_theme;
    *speed = fake_pref_speed;
    return 0;
}

static int s_pref_set(void *ctx, int which, int value, char *msg, size_t n) {
    (void)ctx;
    fake_pref_sets++;
    fake_pref_which = which;
    fake_pref_value = value;
    if(fake_pref_result >= 0) {                         /* in force: the next read says so */
        if(which == 0) fake_pref_theme = value;
        else fake_pref_speed = value;
    }
    if(fake_pref_result) snprintf(msg, n, "%s", fake_pref_result > 0 ? "no settings stick" : "refused");
    return fake_pref_result;
}

const struct gui_sys fake_sys = { NULL, s_tasks, s_memory, s_machine, s_kill, s_disks, s_log,
                                  s_term_open, s_term_read, s_term_write, s_term_resize, s_term_close,
                                  s_win_accept, s_win_update, s_win_event, s_win_close, s_run, s_win_clip,
                                  s_win_paste, s_win_dialog, s_win_answer, s_win_theme,   /* 11/05/26 */
                                  s_run_file, s_note, s_prefs, s_pref_set };

/**** a screen ****/

static u32 pixels[8 + 1280 * 800 + 8];

void fake_screen(struct fb_surface *s, int width, int height) {
    struct fb_mode mode;
    int i;
    memset(&mode, 0, sizeof(mode));
    mode.addr = 1; mode.width = width; mode.height = height; mode.pitch = width * 4;
    mode.bpp = 32; mode.type = 1; mode.red_pos = 16; mode.green_pos = 8;
    mode.red_size = mode.green_size = mode.blue_size = 8;
    for(i = 0; i < (int)(sizeof(pixels) / sizeof(pixels[0])); i++) pixels[i] = 0xa55a1234;
    fb_setup(s, (u8 *)(pixels + 8), &mode);
}

bool fake_guards(int width, int height) {
    int i;
    for(i = 0; i < 8; i++) if(pixels[i] != 0xa55a1234) return false;
    for(i = 8 + width * height; i < (int)(sizeof(pixels) / sizeof(pixels[0])); i++) if(pixels[i] != 0xa55a1234) return false;
    return true;
}

u32 fake_pixel(int width, int x, int y) { return pixels[8 + y * width + x]; }

/**** the desktop ****/

void fake_desktop(struct gui_state *g, int width, int height) {
    fake_files_reset();
    fake_sys_reset();
    gui_init(g, width, height);
    g->fs = &fake_fs;
    g->sys = &fake_sys;
    g->now = 5000;
    strlcpy(g->clock, "14:05", sizeof(g->clock));
    g->time.year = 2026; g->time.month = 10; g->time.date = 17; g->time.weekday = 7;
    g->time.hour = 14; g->time.minute = 5; g->time.second = 30;
}

void fake_click(struct gui_state *g, int x, int y) {
    struct pointer_event e;
    memset(&e, 0, sizeof(e));
    e.x = x; e.y = y; e.buttons = POINTER_LEFT;
    gui_pointer(g, &e);
    e.buttons = 0;
    gui_pointer(g, &e);
}

void fake_type(struct gui_state *g, const char *s) {
    while(*s) gui_key(g, (unsigned char)*s++);
}
