/*
SurfOS Desktop: System Monitor
--------------------
File: guimon.c Date: 10/17/26
--------------------
Three pages, the tabs of its toolbar. Processes: the tasks as ps lists them (pid, name,
state, console, and whether each is a program or a kernel thread), sorted by a column (a
click on its heading, again to reverse it), with a selection that follows its pid from one
reading to the next. End task asks first, then asks struct gui_sys's kill, which ends a
program only, never init or this desktop's own task, and says why in the status bar.
Memory: physical memory, the kernel heap and the DMA heap, as bars and numbers. System: the
processor, firmware, profile, display, build, uptime, tasks and timers. The tick reads all
of it about once a second while the window is open and in view (tucked away, it waits);
drawing uses only what was read. Its Edit menu copies a task's line (or a page), finds a
task by name and goes to a pid. Track SYS of docs/superpowers/plans/2026-10-17-gui-suite.md;
the window manager reaches it through gui_app_monitor (struct gui_app, shell/guipriv.h).
10/18/26: the pid followed stays followed until the user moves the selection, also through a
reading without it (a task that ended, or one a reading missed); a reading that fails keeps
what the last one read; the Memory and System pages scroll in a short window; End task's kill
waits for the end of the system call the task is in, and never ends the shells above the desktop;
once a task End task waited for is gone from a reading, the status bar says it has ended.
*/
#include "gui_sys.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define READ_MS 1000                    /* how often the tick reads the system */
#define END_TASK 3                      /* the toolbar's button after the pages */
#define DLG_END 1                       /* its dialogs (gui_app_act) */
#define DLG_FIND 2
#define DLG_GOTO 3

static const struct gui_tool tools[] = {
    { "Processes", GUI_MON_PROCESSES }, { "Memory", GUI_MON_MEMORY }, { "System", GUI_MON_SYSTEM }, { "End task", END_TASK },
};
#define TOOLS (int)(sizeof(tools) / sizeof(tools[0]))

static const char *const help[] = {
    "#System Monitor",
    "The tasks, memory and the machine, read every second.",
    "Left and Right (or a click on a tab) change the page.",
    "Processes: click a heading to sort by it, again to",
    "reverse it; S sorts by the next column, R reverses.",
    "The arrows, Page Up and Down, Home and End select a",
    "task. Del or End task ends a program, after asking: in",
    "a system call, it ends when the call does. Kernel",
    "threads and the shells this desktop runs in are never",
    "ended. Ctrl+C copies the task's line (or the page),",
    "Ctrl+F finds a task by name, Ctrl+L goes to a pid.",
    "Memory and System scroll with the arrows when short.",
};

/* the Processes page's columns: where each starts (window-relative), and its heading; the pids are right-aligned */
static const int column_x[GUI_MON_COLUMNS] = { 0, 62, 196, 272, 344 };
static const char *const headings[GUI_MON_COLUMNS] = { "PID", "Name", "State", "Console", "Kind" };
#define PID_RIGHT 52                    /* where the pids end */
#define NAME_CELLS 15                   /* a task's name: at most 15 characters (GUI_TASK_NAME) */
#define LINE_H 24                       /* 10/18/26: the Memory and System pages scroll by lines this high */
#define MEMORY_LINES 9                  /* three meters of three lines */
#define SYSTEM_ROWS 8

struct mon_layout { int head_y, list_y, rows, bar_x, status_y; };

static void layout(const struct gui_state *g, struct mon_layout *l) {
    const struct gui_window *w = &g->win[GUI_MONITOR];
    l->head_y = TITLE_H + BAR_H + 3;
    l->list_y = l->head_y + 20;
    l->status_y = w->h - STATUS_H - 1;
    l->rows = (l->status_y - l->list_y) / ROW_H;
    if(l->rows < 1) l->rows = 1;
    l->bar_x = w->w - 2 - SCROLL_W;
}

static int min_width(void) {
    int w = tool_span(tools, TOOLS);
    return w > 360 ? w : 360;
}

/**** words ****/

static const char *state_name(int state) {
    switch(state) {                                     /* as ps says them */
    case GUI_TASK_READY: return "ready";
    case GUI_TASK_RUNNING: return "running";
    case GUI_TASK_BLOCKED: return "blocked";
    case GUI_TASK_SLEEPING: return "sleeping";
    case GUI_TASK_DEAD: return "zombie";
    }
    return "?";
}

/* tty 0-3: the consoles F1-F4; 16 + n: pty n; -1: none */
static void console_name(int tty, char *out, size_t n) {
    if(tty >= 0 && tty < 16) snprintf(out, n, "F%d", tty + 1);
    else if(tty >= 16) snprintf(out, n, "pty %d", tty - 16);
    else strlcpy(out, "-", n);
}

static const char *kind_name(const struct gui_task *t) {
    return (t->flags & GUI_TASK_USER) ? "program" : "kernel thread";
}

/**** the list ****/

static const struct gui_task *selected_task(const struct gui_monitor *m) {
    return m->selected >= 0 && m->selected < m->ntasks ? &m->task[m->selected] : NULL;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int compare_names(const char *a, const char *b) {
    while(*a && lower(*a) == lower(*b)) { a++; b++; }
    return (u8)lower(*a) - (u8)lower(*b);
}

/* running first, the ended last */
static int state_rank(int state) {
    static const int rank[] = { 1, 0, 2, 3, 4 };       /* ready, running, blocked, sleeping, zombie */
    return state >= 0 && state < 5 ? rank[state] : 5;
}

/* F1-F4, then the ptys, then none */
static int console_rank(int tty) { return tty < 0 ? 1 << 20 : tty; }

/* the column's order (reversed when descending), then the pid's */
static int compare(const struct gui_monitor *m, const struct gui_task *a, const struct gui_task *b) {
    int r = 0;
    switch(m->sort) {
    case GUI_MON_PID: r = a->pid < b->pid ? -1 : a->pid > b->pid; break;
    case GUI_MON_NAME: r = compare_names(a->name, b->name); break;
    case GUI_MON_STATE: r = state_rank(a->state) - state_rank(b->state); break;
    case GUI_MON_CONSOLE: r = console_rank(a->tty) - console_rank(b->tty); break;
    case GUI_MON_KIND: r = (int)!(a->flags & GUI_TASK_USER) - (int)!(b->flags & GUI_TASK_USER); break;
    }
    if(m->descending) r = -r;
    return r ? r : a->pid < b->pid ? -1 : a->pid > b->pid;
}

/* in order (few tasks: an insertion sort), then the selection where its pid is now. 10/18/26: a pid that is not
   in this reading (it ended, or the reading missed it) stays followed until the user moves the selection: nothing
   is selected meanwhile, and hint keeps the row it was on (Down goes on from there) */
static void arrange(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    struct gui_task t;
    struct mon_layout l;
    int i, j;
    for(i = 1; i < m->ntasks; i++) {
        t = m->task[i];
        for(j = i; j > 0 && compare(m, &t, &m->task[j - 1]) < 0; j--) m->task[j] = m->task[j - 1];
        m->task[j] = t;
    }
    if(m->pid < 0) m->selected = m->ntasks ? 0 : -1;  /* nothing followed yet (the window opened): the first */
    else {
        for(i = 0; i < m->ntasks && m->task[i].pid != m->pid; i++) ;
        m->selected = i < m->ntasks ? i : -1;
    }
    if(m->selected >= 0) {
        m->pid = m->task[m->selected].pid;
        m->hint = m->selected;
    }
    layout(g, &l);
    m->top = gui_clamp(m->top, 0, m->ntasks > l.rows ? m->ntasks - l.rows : 0);
}

static void show_selected(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    struct mon_layout l;
    layout(g, &l);
    m->top = sys_view(m->selected, m->top, m->ntasks, l.rows);
    g->dirty = true;
}

static void choose(struct gui_state *g, int row) {
    struct gui_monitor *m = &g->monitor;
    if(row < 0 || row >= m->ntasks) return;
    m->selected = m->hint = row;
    m->pid = m->task[row].pid;
    m->status[0] = 0;
    show_selected(g);
}

/* 10/18/26: End task's kill waited for the end of a call (the task was still in the list after it): a reading without
   the task (the whole list: not one cut to the newest) says it has ended, in the status bar while the selection still
   follows it; once the user has moved on, nothing is said */
static void ended(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    int i;
    for(i = 0; i < m->ntasks && m->task[i].pid != m->ending; i++) ;
    if(i < m->ntasks || m->ntasks == GUI_TASK_MAX) return;
    if(m->pid == m->ending)
        snprintf(m->status, sizeof(m->status), "%s (pid %d) has ended", m->ending_name, m->ending);
    m->ending = -1;
}

/* everything again, from struct gui_sys (from the handlers and the tick only). 10/18/26: a reading that fails
   keeps what the last one read (tasks leaves the rows as they were when it fails: sys/gui_sys.h) */
static void read_all(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    struct gui_memory mem;
    struct gui_machine mc;
    int n;
    m->read_ms = g->now;
    m->read = true;
    g->dirty = true;
    if(!g->sys) {
        m->ntasks = 0;
        m->selected = m->pid = m->hint = -1;
        return;
    }
    n = g->sys->tasks ? g->sys->tasks(g->sys->ctx, m->task, GUI_TASK_MAX) : -ENOSYS;
    m->err_tasks = n < 0 ? n : 0;
    if(n >= 0) {
        m->ntasks = n > GUI_TASK_MAX ? GUI_TASK_MAX : n;
        arrange(g);
        if(m->ending >= 0) ended(g);
    }
    m->err_memory = g->sys->memory ? g->sys->memory(g->sys->ctx, &mem) : -ENOSYS;
    if(!m->err_memory) {
        m->mem = mem;
        m->have_memory = true;
    }
    m->err_machine = g->sys->machine ? g->sys->machine(g->sys->ctx, &mc) : -ENOSYS;
    if(!m->err_machine) {
        m->machine = mc;
        m->have_machine = true;
    }
}

static void sort_by(struct gui_state *g, int column) {
    struct gui_monitor *m = &g->monitor;
    if(column == m->sort) m->descending = !m->descending;
    else { m->sort = column; m->descending = false; }
    arrange(g);
    show_selected(g);
    snprintf(m->status, sizeof(m->status), "Sorted by %s%s", headings[m->sort], m->descending ? ", reversed" : "");
}

static void set_page(struct gui_state *g, int page) {
    struct gui_monitor *m = &g->monitor;
    m->page = page;
    m->status[0] = 0;
    m->thumb = false;
    if(page == GUI_MON_PROCESSES) show_selected(g);
    g->dirty = true;
}

/**** ending a task ****/

/* why End task cannot ask about the selected task, or NULL when it can */
static const char *cannot_end(const struct gui_state *g) {
    const struct gui_task *t = selected_task(&g->monitor);
    if(!g->sys || !g->sys->kill) return "this desktop cannot end tasks";
    if(!t) return "no task is selected";
    if(!(t->flags & GUI_TASK_USER)) return "is a kernel thread: End task ends programs";
    if(t->state == GUI_TASK_DEAD) return "has ended already";
    return NULL;
}

static void end_task(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    const struct gui_task *t = selected_task(m);
    const char *no = cannot_end(g);
    char line[80], where[16];
    g->dirty = true;
    if(no) {
        if(t) snprintf(m->status, sizeof(m->status), "%s (pid %d) %s", t->name, t->pid, no);
        else snprintf(m->status, sizeof(m->status), "End task: %s", no);
        return;
    }
    console_name(t->tty, where, sizeof(where));
    snprintf(line, sizeof(line), "End %s (pid %d)%s%s?", t->name, t->pid, t->tty >= 0 ? " on " : "",
             t->tty >= 0 ? where : "");
    /* 10/18/26: in its own code the task ends at once; inside a system call, when the call ends (kill_task_patient()).
       Which of the two the kill meets, the list cannot tell (a reading is a second old): the question names both */
    dialog_confirm(g, gui_app_act(GUI_MONITOR, DLG_END), "End task", line,
                   (t->flags & GUI_TASK_SHELL) ? "Ends now or after a system call; init starts another." :
                   "Ends now or after a system call; unsaved work is lost.", "End task", NULL, "Cancel");
    g->dialog.choice = 2;                               /* Enter alone ends nothing */
    snprintf(g->dialog.target, sizeof(g->dialog.target), "%d", t->pid);
}

/**** finding ****/

static bool has(const char *name, const char *what) {
    u32 n = strlen(what), i;
    for(; *name; name++) {
        for(i = 0; i < n && name[i] && lower(name[i]) == lower(what[i]); i++) ;
        if(i == n) return true;
    }
    return false;
}

/* the next task after the selection whose name has m->find, around the end. 10/18/26: with nothing selected, from
   the row the followed task was on (hint: the task now there first), as Down goes on from there */
static void find_next(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    int k, row, start;
    if(m->page != GUI_MON_PROCESSES) set_page(g, GUI_MON_PROCESSES);
    start = m->selected >= 0 ? m->selected + 1 : m->hint >= 0 ? gui_clamp(m->hint, 0, m->ntasks) : 0;
    for(k = 0; k < m->ntasks; k++) {
        row = (start + k) % m->ntasks;
        if(has(m->task[row].name, m->find)) {
            choose(g, row);
            snprintf(m->status, sizeof(m->status), "Found %s (pid %d)", m->task[row].name, m->task[row].pid);
            return;
        }
    }
    snprintf(m->status, sizeof(m->status), "No task's name has \"%s\"", m->find);
    g->dirty = true;
}

static void go_to(struct gui_state *g, const char *text) {
    struct gui_monitor *m = &g->monitor;
    int pid = atoi(text), row;
    if(m->page != GUI_MON_PROCESSES) set_page(g, GUI_MON_PROCESSES);
    for(row = 0; row < m->ntasks && m->task[row].pid != pid; row++) ;
    if(row < m->ntasks && text[0] >= '0' && text[0] <= '9') choose(g, row);
    else snprintf(m->status, sizeof(m->status), "No task has pid %s", text);
    g->dirty = true;
}

/**** the Edit menu: Copy, Find, Find next, Go to pid ****/

/* a task as one line of text */
static void task_line(const struct gui_task *t, char *out, size_t n) {
    char where[16];
    console_name(t->tty, where, sizeof(where));
    snprintf(out, n, "%5d  %-15s  %-8s  %-7s  %s", t->pid, t->name, state_name(t->state), where, kind_name(t));
}

/* the Memory and System pages' lines, as the page shows them: line k of the page, false after the last */
static bool page_line(const struct gui_state *g, int k, char *out, size_t n);

static void clip(struct gui_state *g, const char *s) {
    u32 len = strlen(s);
    if(len > GUI_CLIP_MAX) len = GUI_CLIP_MAX;
    memcpy(g->clip, s, len);
    g->clip_len = len;
}

static void copy(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    const struct gui_task *t = selected_task(m);
    char text[1024], line[128];
    u32 used = 0;
    int k;
    if(m->page == GUI_MON_PROCESSES) {
        if(!t) return;
        task_line(t, text, sizeof(text));
        clip(g, text);
        snprintf(m->status, sizeof(m->status), "Copied the line of %s (pid %d)", t->name, t->pid);
        return;
    }
    if(m->page == GUI_MON_MEMORY ? !m->have_memory : !m->have_machine) {   /* 10/18/26: never read: no zeros */
        snprintf(m->status, sizeof(m->status), "Nothing copied: %s could not be read",
                 m->page == GUI_MON_MEMORY ? "memory" : "the machine");
        return;
    }
    text[0] = 0;
    for(k = 0; page_line(g, k, line, sizeof(line)); k++) {
        snprintf(text + used, sizeof(text) - used, "%s\n", line);
        used = strlen(text);
    }
    clip(g, text);
    snprintf(m->status, sizeof(m->status), "Copied the %s page", tools[m->page].label);
}

static bool can_edit(const struct gui_state *g, int act) {
    const struct gui_monitor *m = &g->monitor;
    switch(act) {
    case ACT_COPY: return m->page == GUI_MON_PROCESSES ? selected_task(m) != NULL : g->sys != NULL;
    case ACT_FIND: case ACT_GOTO: return m->ntasks > 0;
    case ACT_FIND_NEXT: return m->ntasks > 0 && m->find[0];
    }
    return false;
}

static void edit(struct gui_state *g, int act) {
    struct gui_monitor *m = &g->monitor;
    char pid[16];
    g->dirty = true;
    switch(act) {
    case ACT_COPY: copy(g); break;
    case ACT_FIND:
        dialog_input(g, gui_app_act(GUI_MONITOR, DLG_FIND), "Find a task", "A task's name, or part of it:", m->find);
        break;
    case ACT_FIND_NEXT: find_next(g); break;
    case ACT_GOTO:
        snprintf(pid, sizeof(pid), "%d", m->pid >= 0 ? m->pid : 0);
        dialog_input(g, gui_app_act(GUI_MONITOR, DLG_GOTO), "Go to pid", "A task's pid (ps lists them):", pid);
        break;
    }
}

static void answer(struct gui_state *g, int act, int button, const char *text, const char *target) {
    struct gui_monitor *m = &g->monitor;
    char msg[GUI_STATUS];
    int r, pid, row;
    g->dirty = true;
    if(button != 0) return;
    switch(act) {
    case DLG_END:
        if(!g->sys || !g->sys->kill) return;
        msg[0] = 0;
        pid = atoi(target);
        r = g->sys->kill(g->sys->ctx, pid, msg, sizeof(msg));
        strlcpy(m->status, msg[0] ? msg : r ? "Not ended" : "Ended", sizeof(m->status));
        m->ending = -1;
        read_all(g);                                    /* what is left, at once */
        for(row = 0; !r && row < m->ntasks; row++) {    /* 10/18/26: still there: it ends when its call does (ended()) */
            if(m->task[row].pid != pid) continue;
            m->ending = pid;
            strlcpy(m->ending_name, m->task[row].name, sizeof(m->ending_name));
        }
        break;
    case DLG_FIND:
        strlcpy(m->find, text, sizeof(m->find));
        if(m->find[0]) find_next(g);
        break;
    case DLG_GOTO:
        go_to(g, text);
        break;
    }
}

/**** the Memory and System pages: lines that scroll in a short window (10/18/26) ****/

static int page_lines(int page) { return page == GUI_MON_MEMORY ? MEMORY_LINES : SYSTEM_ROWS; }

/* the lines the page's area shows, at least one */
static int page_shown(const struct mon_layout *l) {
    int n = (l->status_y - l->head_y - 10) / LINE_H;
    return n > 1 ? n : 1;
}

static int page_max(const struct gui_monitor *m, const struct mon_layout *l) {
    int n = page_lines(m->page) - page_shown(l);
    return n > 0 ? n : 0;
}

/* the first line shown: as scrolled, within what the window's height allows now */
static int page_first(const struct gui_monitor *m, const struct mon_layout *l) {
    return gui_clamp(m->scroll[m->page], 0, page_max(m, l));
}

static void page_scroll(struct gui_state *g, int first) {
    struct gui_monitor *m = &g->monitor;
    struct mon_layout l;
    layout(g, &l);
    m->scroll[m->page] = gui_clamp(first, 0, page_max(m, &l));
    g->dirty = true;
}

/**** keys and the pointer ****/

static bool key(struct gui_state *g, int k) {
    struct gui_monitor *m = &g->monitor;
    struct mon_layout l;
    int row, first;
    if(k == GUI_LEFT || k == GUI_RIGHT) {
        set_page(g, (m->page + (k == GUI_RIGHT ? 1 : GUI_MON_PAGES - 1)) % GUI_MON_PAGES);
        return true;
    }
    layout(g, &l);
    if(m->page != GUI_MON_PROCESSES) {                  /* 10/18/26: the page scrolls */
        first = page_first(m, &l);
        switch(k) {
        case GUI_UP: page_scroll(g, first - 1); return true;
        case GUI_DOWN: page_scroll(g, first + 1); return true;
        case GUI_PGUP: page_scroll(g, first - page_shown(&l)); return true;
        case GUI_PGDN: page_scroll(g, first + page_shown(&l)); return true;
        case GUI_HOME: page_scroll(g, 0); return true;
        case GUI_END: page_scroll(g, page_lines(m->page)); return true;
        }
        return false;
    }
    row = sys_move_from(m->selected, m->hint, m->ntasks, l.rows, k);
    if(row != -2) {
        if(row >= 0) choose(g, row);
        return true;
    }
    switch(k) {
    case GUI_DELETE: end_task(g); return true;
    case 's': case 'S': sort_by(g, (m->sort + 1) % GUI_MON_COLUMNS); return true;
    case 'r': case 'R': sort_by(g, m->sort); return true;
    }
    return false;
}

static int column_at(const struct mon_layout *l, int x) {
    int c;
    if(x >= l->bar_x) return -1;
    for(c = GUI_MON_COLUMNS - 1; c > 0 && x < column_x[c]; c--) ;
    return c;
}

static void press(struct gui_state *g, int x, int y) {
    struct gui_monitor *m = &g->monitor;
    struct mon_layout l;
    int i, top, size;
    layout(g, &l);
    i = tool_hit(tools, TOOLS, x, y);
    if(i >= 0) {
        if(tools[i].act == END_TASK) end_task(g);
        else set_page(g, tools[i].act);
        return;
    }
    if(m->page != GUI_MON_PROCESSES) {                  /* 10/18/26: a page's scroll bar: a page, or hold the thumb */
        if(x < l.bar_x || y < l.head_y || y >= l.status_y || !page_max(m, &l)) return;
        i = page_first(m, &l);
        gd_thumb(l.status_y - l.head_y, i, page_shown(&l), page_lines(m->page), &top, &size);
        if(y - l.head_y < top) page_scroll(g, i - page_shown(&l));
        else if(y - l.head_y >= top + size) page_scroll(g, i + page_shown(&l));
        else { m->thumb = true; m->scroll_at = y - l.head_y - top; }
        return;
    }
    if(y >= l.head_y && y < l.list_y) {                 /* a heading: sort by it */
        if((i = column_at(&l, x)) >= 0) sort_by(g, i);
        return;
    }
    if(y < l.list_y || y >= l.list_y + l.rows * ROW_H) return;
    if(x >= l.bar_x) {                                  /* the scroll bar: a page, or hold the thumb */
        gd_thumb(l.rows * ROW_H, m->top, l.rows, m->ntasks, &top, &size);
        if(y - l.list_y < top) m->top -= l.rows;
        else if(y - l.list_y >= top + size) m->top += l.rows;
        else { m->thumb = true; m->scroll_at = y - l.list_y - top; }
        m->top = gui_clamp(m->top, 0, m->ntasks > l.rows ? m->ntasks - l.rows : 0);
        g->dirty = true;
        return;
    }
    choose(g, m->top + (y - l.list_y) / ROW_H);
}

static void drag(struct gui_state *g, int x, int y) {
    struct gui_monitor *m = &g->monitor;
    struct mon_layout l;
    (void)x;
    if(!m->thumb) return;
    layout(g, &l);
    if(m->page != GUI_MON_PROCESSES)
        page_scroll(g, gd_scroll_to(l.status_y - l.head_y, y - l.head_y - m->scroll_at, page_shown(&l), page_lines(m->page)));
    else m->top = gd_scroll_to(l.rows * ROW_H, y - l.list_y - m->scroll_at, l.rows, m->ntasks);
    g->dirty = true;
}

static void release(struct gui_state *g) {
    g->monitor.thumb = false;
}

static void wheel(struct gui_state *g, int rows) {
    struct gui_monitor *m = &g->monitor;
    struct mon_layout l;
    layout(g, &l);
    if(m->page != GUI_MON_PROCESSES) page_scroll(g, page_first(m, &l) + rows);
    else m->top = gui_clamp(m->top + rows, 0, m->ntasks > l.rows ? m->ntasks - l.rows : 0);
    g->dirty = true;
}

/**** opening, and every frame ****/

static void opened(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    m->status[0] = 0;                                   /* the page and the order stay as they were */
    m->selected = m->pid = m->hint = -1;                /* the first task, once read (arrange()) */
    m->ending = -1;
    m->top = 0;
    m->thumb = false;
    read_all(g);
}

static void tick(struct gui_state *g) {
    struct gui_monitor *m = &g->monitor;
    if(g->win[GUI_MONITOR].min) return;                 /* tucked away: read again once it is back */
    if(m->read && g->now - m->read_ms < READ_MS) return;
    read_all(g);
}

/**** drawing ****/

/* the Memory page's three meters: a heading, what it holds, its numbers; the bar's used and total in KB. A heap grows
   when it needs to, up to a limit (the kernel heap's room in the address space, the DMA heap's arena): its bar is of
   that limit, what it has grown to being its used and free together */
struct meter { const char *title; char right[48], line[96]; u32 used, total; };

static void meters(const struct gui_state *g, struct meter *mt) {
    const struct gui_memory *mem = &g->monitor.mem;
    u32 total = mem->total_pages * 4, free = mem->free_pages * 4;
    char a[16], b[16], c[16];
    mt[0].title = "Physical memory";
    sys_kb(mem->ram_kb, a, sizeof(a));
    if(mem->high_kb) {
        sys_kb(mem->high_kb, b, sizeof(b));
        snprintf(mt[0].right, sizeof(mt[0].right), "RAM %s (%s above 4 GB unused)", a, b);
    } else snprintf(mt[0].right, sizeof(mt[0].right), "RAM %s", a);
    mt[0].used = total > free ? total - free : 0;
    sys_kb(mt[0].used, a, sizeof(a));
    sys_kb(free, b, sizeof(b));
    sys_kb(total, c, sizeof(c));
    snprintf(mt[0].line, sizeof(mt[0].line), "%s used, %s free, of %s in pages of 4 KB", a, b, c);
    mt[0].total = total;
    mt[1].title = "Kernel heap";
    mt[2].title = "DMA heap";
    sys_kb(mem->heap_size / 1024, a, sizeof(a));
    snprintf(mt[1].right, sizeof(mt[1].right), "at most %s", a);
    sys_kb(mem->dma_size / 1024, a, sizeof(a));
    snprintf(mt[2].right, sizeof(mt[2].right), "at most %s", a);
    gui_bytes(mem->heap_used, a, sizeof(a));
    gui_bytes(mem->heap_free, b, sizeof(b));
    snprintf(mt[1].line, sizeof(mt[1].line), "%s used, %s free", a, b);
    gui_bytes(mem->dma_used, a, sizeof(a));
    gui_bytes(mem->dma_free, b, sizeof(b));
    snprintf(mt[2].line, sizeof(mt[2].line), "%s used, %s free", a, b);
    mt[1].used = mem->heap_used / 1024;
    mt[1].total = mem->heap_size / 1024;
    mt[2].used = mem->dma_used / 1024;
    mt[2].total = mem->dma_size / 1024;
}

static void uptime(u32 s, char *out, size_t n) {
    snprintf(out, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

static bool system_row(const struct gui_state *g, int k, const char **label, char *value, size_t n) {
    const struct gui_machine *mc = &g->monitor.machine;
    static const char *const labels[SYSTEM_ROWS] = { "Processor", "Firmware", "Profile", "Display", "Build", "Uptime",
                                                     "Tasks", "Timers" };
    if(k < 0 || k >= SYSTEM_ROWS) return false;
    *label = labels[k];
    switch(k) {
    case 0: strlcpy(value, mc->cpu[0] ? mc->cpu : "unknown", n); break;
    case 1: strlcpy(value, mc->firmware[0] ? mc->firmware : "unknown", n); break;
    case 2: strlcpy(value, mc->profile[0] ? mc->profile : "unknown", n); break;
    case 3: snprintf(value, n, "%s (the desktop: %dx%d)", mc->video[0] ? mc->video : "unknown", g->width, g->height); break;
    case 4: strlcpy(value, mc->build[0] ? mc->build : "none recorded (/initrd/etc/build-id)", n); break;
    case 5: uptime(mc->uptime_s, value, n); break;
    case 6: snprintf(value, n, "%u", mc->tasks); break;
    case 7: snprintf(value, n, "%u", mc->timers); break;
    }
    return true;
}

static bool page_line(const struct gui_state *g, int k, char *out, size_t n) {
    struct meter mt[3];
    const char *label;
    char value[96];
    if(g->monitor.page == GUI_MON_MEMORY) {
        if(k < 0 || k >= 3) return false;
        meters(g, mt);
        snprintf(out, n, "%s: %s (%s)", mt[k].title, mt[k].line, mt[k].right);
        return true;
    }
    if(!system_row(g, k, &label, value, sizeof(value))) return false;
    snprintf(out, n, "%s: %s", label, value);
    return true;
}

static void processes_render(const struct gui_state *g, const struct gui_draw *d, const struct mon_layout *l) {
    const struct gui_window *w = &g->win[GUI_MONITOR];
    const struct gui_monitor *m = &g->monitor;
    struct gui_draw list;
    bool focused = g->focus == GUI_MONITOR && !g->dialog.kind && !g->menu;
    char b[32];
    int x0 = w->x, y0 = w->y, row, c, x, py;
    gd_rect(d, x0 + 1, y0 + l->head_y, w->w - 2, 20, SHELL);
    for(c = 0; c < GUI_MON_COLUMNS; c++) {
        x = c == GUI_MON_PID ? PID_RIGHT - (int)strlen(headings[c]) * 8 : column_x[c] + 8;
        if(x + (int)strlen(headings[c]) * 8 > l->bar_x) break;   /* a narrow window: the last column has no room */
        x = gd_text(d, x0 + x, y0 + l->head_y + 2, headings[c], m->sort == c ? INK : MUTED);
        if(m->sort == c) gd_glyph(d, x + 2, y0 + l->head_y + 2, m->descending ? 0x1F : 0x1E, TEAL, 1);   /* ▼ or ▲ */
    }
    gd_rect(d, x0 + 1, y0 + l->list_y, l->bar_x - 1, l->status_y - l->list_y, SYS_SHEET);
    list = gd_clip(d, x0 + 1, y0 + l->list_y, l->bar_x - 1, l->rows * ROW_H);
    if(!g->sys || (m->err_tasks && !m->ntasks)) {     /* 10/18/26: with rows read before, those stay */
        gd_text(&list, x0 + 16, y0 + l->list_y + 8, !g->sys ? "The system cannot be read here." :
                "The tasks could not be read.", MUTED);
    }
    for(row = 0; row < l->rows && m->top + row < m->ntasks; row++) {
        const struct gui_task *t = &m->task[m->top + row];
        py = y0 + l->list_y + row * ROW_H;
        if(m->top + row == m->selected) gd_rect(&list, x0 + 1, py, l->bar_x - 1, ROW_H, focused ? PICK : BAND);
        snprintf(b, sizeof(b), "%d", t->pid);
        gd_text(&list, x0 + PID_RIGHT - (int)strlen(b) * 8, py + 1, b, MUTED);
        gd_name(&list, x0 + column_x[GUI_MON_NAME] + 8, py + 1, t->name, NAME_CELLS, INK);
        gd_text(&list, x0 + column_x[GUI_MON_STATE] + 8, py + 1, state_name(t->state),
                t->state == GUI_TASK_RUNNING ? TEAL : MUTED);
        console_name(t->tty, b, sizeof(b));
        gd_text(&list, x0 + column_x[GUI_MON_CONSOLE] + 8, py + 1, b, MUTED);
        gd_name(&list, x0 + column_x[GUI_MON_KIND] + 8, py + 1, kind_name(t), (l->bar_x - column_x[GUI_MON_KIND] - 12) / 8,
                (t->flags & GUI_TASK_USER) ? INK : MUTED);
    }
    gd_scrollbar(d, x0 + l->bar_x, y0 + l->list_y, l->rows * ROW_H, m->top, l->rows, m->ntasks);
}

/* 10/18/26: a page's area, between the toolbar and the status bar, and its scroll bar when its lines do not fit
   (then the content keeps clear of it); the content's top line is first */
static struct gui_draw page_area(const struct gui_state *g, const struct gui_draw *d, const struct mon_layout *l,
                                 int *right) {
    const struct gui_window *w = &g->win[GUI_MONITOR];
    const struct gui_monitor *m = &g->monitor;
    *right = page_max(m, l) ? l->bar_x - 4 : w->w - 16;
    if(page_max(m, l))
        gd_scrollbar(d, w->x + l->bar_x, w->y + l->head_y, l->status_y - l->head_y, page_first(m, l), page_shown(l),
                     page_lines(m->page));
    return gd_clip(d, w->x + 1, w->y + l->head_y, (page_max(m, l) ? l->bar_x : w->w - 1) - 1, l->status_y - l->head_y);
}

static void memory_render(const struct gui_state *g, const struct gui_draw *d, const struct mon_layout *l) {
    const struct gui_window *w = &g->win[GUI_MONITOR];
    struct gui_draw area;
    struct meter mt[3];
    int x0 = w->x + 16, right, width, cells, k, room, len;
    int y = w->y + l->head_y + 10 - page_first(&g->monitor, l) * LINE_H;
    area = page_area(g, d, l, &right);
    width = right - 16;
    cells = width / 8;
    if(!g->sys || !g->monitor.have_memory) {
        gd_text(&area, x0, w->y + l->head_y + 10, !g->sys ? "The system cannot be read here." : "Memory could not be read.",
                MUTED);
        return;
    }
    meters(g, mt);
    for(k = 0; k < 3; k++, y += 3 * LINE_H) {
        gd_text(&area, x0, y, mt[k].title, TEAL);
        room = cells - (int)strlen(mt[k].title) - 2;    /* what it holds, at the right: cut rather than over the title */
        len = (int)strlen(mt[k].right) < room ? (int)strlen(mt[k].right) : room;
        gd_name(&area, x0 + (cells - len) * 8, y, mt[k].right, room, MUTED);
        sys_bar(&area, x0, y + 22, width, 16, mt[k].used, mt[k].total);
        gd_name(&area, x0, y + 44, mt[k].line, cells, INK);
    }
}

static void system_render(const struct gui_state *g, const struct gui_draw *d, const struct mon_layout *l) {
    const struct gui_window *w = &g->win[GUI_MONITOR];
    struct gui_draw area;
    const char *label;
    char value[96];
    int x0 = w->x + 16, right, k;
    int y = w->y + l->head_y + 10 - page_first(&g->monitor, l) * LINE_H;
    area = page_area(g, d, l, &right);
    if(right == w->w - 16) right -= 8;                  /* clear of the resize grip, as before */
    if(!g->sys || !g->monitor.have_machine) {
        gd_text(&area, x0, w->y + l->head_y + 10, !g->sys ? "The system cannot be read here." :
                "The machine could not be read.", MUTED);
        return;
    }
    for(k = 0; system_row(g, k, &label, value, sizeof(value)); k++, y += LINE_H) {
        gd_text(&area, x0, y, label, MUTED);
        gd_name(&area, x0 + 104, y, value, (right - 16 - 104) / 8, INK);
    }
}

static void render(const struct gui_state *g, const struct gui_draw *d) {
    const struct gui_window *w = &g->win[GUI_MONITOR];
    const struct gui_monitor *m = &g->monitor;
    const struct gui_task *t = selected_task(m);
    struct mon_layout l;
    bool on[TOOLS];
    char b[GUI_STATUS + 64], right[32], where[16];
    int i, x0 = w->x, y0 = w->y, x;
    layout(g, &l);
    for(i = 0; i < TOOLS; i++) on[i] = tools[i].act != END_TASK || (m->page == GUI_MON_PROCESSES && !cannot_end(g));
    gd_rect(d, x0 + 1, y0 + TITLE_H, w->w - 2, BAR_H + 2, BAND);
    tool_render(d, x0, y0, tools, TOOLS, on);
    /* the page shown: its tab in the title's colour (where tool_render() put it: tool_span() of those before it) */
    x = tool_span(tools, m->page) - 4;
    gd_button(d, x0 + x, y0 + TITLE_H + 4, (int)strlen(tools[m->page].label) * 8 + 14, 22, tools[m->page].label,
              TEAL, PAPER);
    gd_rect(d, x0 + 1, y0 + TITLE_H + BAR_H + 2, w->w - 2, 1, LINE);
    if(m->page == GUI_MON_PROCESSES) processes_render(g, d, &l);
    else if(m->page == GUI_MON_MEMORY) memory_render(g, d, &l);
    else system_render(g, d, &l);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, STATUS_H, BAND);
    gd_rect(d, x0 + 1, y0 + l.status_y, w->w - 2, 1, LINE);
    right[0] = 0;
    if(m->status[0]) strlcpy(b, m->status, sizeof(b));
    else if(!g->sys) strlcpy(b, "No system here: nothing to show", sizeof(b));
    /* 10/18/26: a reading that failed keeps the last one's, and says so */
    else if(m->page == GUI_MON_PROCESSES && m->err_tasks && m->ntasks)
        strlcpy(b, "The tasks could not be read now: the list is the last one read", sizeof(b));
    else if(m->page == GUI_MON_MEMORY && m->err_memory && m->have_memory)
        strlcpy(b, "Memory could not be read now: the last numbers read", sizeof(b));
    else if(m->page == GUI_MON_SYSTEM && m->err_machine && m->have_machine)
        strlcpy(b, "The machine could not be read now: as last read", sizeof(b));
    else if(m->page == GUI_MON_PROCESSES && t) {
        console_name(t->tty, where, sizeof(where));
        snprintf(b, sizeof(b), "%s (pid %d): a %s%s%s%s", t->name, t->pid, kind_name(t), t->tty >= 0 ? " on " : "",
                 t->tty >= 0 ? where : "", (t->flags & GUI_TASK_SHELL) ? ", the console's shell" : "");
    } else if(m->page == GUI_MON_PROCESSES && m->pid >= 0)   /* followed, but not in this reading */
        snprintf(b, sizeof(b), "pid %d is not in the list now", m->pid);
    else if(m->page == GUI_MON_SYSTEM) strlcpy(b, "Read every second", sizeof(b));
    else b[0] = 0;
    if(m->page == GUI_MON_PROCESSES && g->sys) {         /* 10/18/26: "of" only when the list is cut (the newest) */
        if(m->ntasks == GUI_TASK_MAX && m->machine.tasks > (u32)m->ntasks)
            snprintf(right, sizeof(right), "the newest %d of %u", m->ntasks, m->machine.tasks);
        else snprintf(right, sizeof(right), "%d task%s", m->ntasks, m->ntasks == 1 ? "" : "s");
    }
    x = w->w - 24 - (int)strlen(right) * 8;             /* clear of the resize grip */
    gd_name(d, x0 + 10, y0 + l.status_y + 3, b, (x - 18) / 8, INK);
    gd_text(d, x0 + x, y0 + l.status_y + 3, right, MUTED);
}

const struct gui_app gui_app_monitor = {
    .title = "System Monitor", .icon = ICON_MONITOR, .shortcut = "", .w = 540, .h = 360,
    .resizable = true, .arrows = true, .keys = false,
    .min_width = min_width,
    .opened = opened, .render = render, .key = key, .press = press, .drag = drag, .release = release, .wheel = wheel,
    .tick = tick, .answer = answer,
    .help = help, .help_lines = (int)(sizeof(help) / sizeof(help[0])),
    .can_edit = can_edit, .edit = edit, .goto_label = "Go to pid\xF9",
};
