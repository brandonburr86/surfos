/* Host tests of the suite as one desktop (docs/superpowers/plans/2026-10-17-gui-suite.md): what the tracks'
   applications do together, against the in-memory file system and system of tests/host/gui_fake.c. Built and run by
   `make unittest` (unittest-gui).
   10/17/26: Files' Open takes each kind of file to its application (a picture to the Image Viewer, a file Notepad
   would refuse to Hex, text to Notepad) and asks about Notepad's changes only for what Notepad opens; a character
   copied in the Character Map pastes into Notepad and into the Terminal's session as its UTF-8; and every window open
   at once draws inside guard pixels at three sizes, and the desktop leaves no session behind. */
#include "gui_fake.h"
#include "../../shell/gui_media.h"
#include "../../shell/gui_tools.h"

static struct gui_state g;
static struct fb_surface screen;
static char big[GUI_NOTE_MAX + 1];

static void ticks(int n) { while(n-- > 0) { g.now += 20; gui_tick(&g); } }

/* Files' Open on path, revealed and selected in its window */
static void files_opens(const char *path) {
    gui_open(&g, GUI_FILES);
    files_reveal(&g, path);
    CHECK(g.focus == GUI_FILES && g.files.selected > 0 && !strcmp(g.files.node[g.files.selected].name, gui_basename(path)));
    files_act(&g, ACT_FILES_OPEN);
    ticks(5);
}

static void make_files(void) {
    u8 bmp[54 + 2 * 8];
    CHECK(bmp_header24(bmp, 2, 2) == 8);                /* two rows of 2 pixels, each padded to 8 bytes */
    memset(bmp + 54, 0x80, 2 * 8);
    fake_file("/home/wave.bmp", (const char *)bmp, sizeof(bmp));
    fake_file("/home/notes.txt", "Surf's up\n", 10);
    fake_file("/home/prog.bin", "\x7f" "ELF\x01\x01\x01\0\0\0\0\0\0\0\0\0", 16);
    memset(big, 'w', sizeof(big));                      /* text, a byte more than Notepad takes */
    fake_file("/home/big.log", big, sizeof(big));
    memset(big, 'a', 1000);                             /* text with a NUL byte past the first 512 */
    big[900] = 0;
    fake_file("/home/late.dat", big, 1000);
}

static void test_files_open(void) {
    fake_desktop(&g, 960, 540);
    make_files();
    gui_open(&g, GUI_NOTEPAD);                          /* Notepad has changes of its own */
    fake_type(&g, "draft");
    CHECK(g.note.modified && g.note.len == 5);

    /* a picture: the Image Viewer, with no question about Notepad's changes */
    files_opens("/home/wave.bmp");
    CHECK(g.win[GUI_VIEWER].open && g.focus == GUI_VIEWER && !strcmp(g.viewer.path, "/home/wave.bmp"));
    CHECK(g.viewer.ready && g.viewer.w == 2 && g.viewer.h == 2);
    CHECK(!g.dialog.kind && g.note.modified && g.note.len == 5);

    /* a NUL byte at once, and more than 256 KB: Hex, and Notepad keeps its text */
    files_opens("/home/prog.bin");
    CHECK(g.win[GUI_HEX].open && g.focus == GUI_HEX && !strcmp(g.hex.path, "/home/prog.bin") && g.hex.size == 16);
    CHECK(!g.dialog.kind && g.note.modified && g.note.len == 5);
    files_opens("/home/big.log");
    CHECK(g.focus == GUI_HEX && !strcmp(g.hex.path, "/home/big.log") && g.hex.size == GUI_NOTE_MAX + 1);
    CHECK(!g.dialog.kind && g.note.modified && g.note.len == 5);

    /* text: Notepad, which asks about its changes first (Don't save) */
    files_opens("/home/notes.txt");
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && g.dialog.act == ACT_DLG_GUARD);
    gui_key(&g, GUI_RIGHT);
    gui_key(&g, '\r');
    CHECK(!g.dialog.kind && g.focus == GUI_NOTEPAD && !strcmp(g.note.path, "/home/notes.txt") && g.note.len == 10);

    /* a NUL byte further on is Notepad's to find: it says so, and Hex is left as it was */
    files_opens("/home/late.dat");
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && strstr(g.dialog.text[0], "NUL bytes") != NULL);
    CHECK(!strcmp(g.hex.path, "/home/big.log"));
    gui_key(&g, '\r');
    CHECK(!g.dialog.kind);

    /* an empty file is text; a file gone since Files listed it goes to Notepad, which says why */
    fake_file("/home/notes.txt", "", 0);
    files_opens("/home/notes.txt");
    CHECK(!g.dialog.kind && g.focus == GUI_NOTEPAD && !strcmp(g.note.path, "/home/notes.txt") && g.note.len == 0);
    gui_open(&g, GUI_FILES);
    files_reveal(&g, "/home/prog.bin");
    CHECK(g.fs->remove(g.fs->ctx, "/home/prog.bin", false) == 0);
    files_act(&g, ACT_FILES_OPEN);
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "Cannot open the file"));
    CHECK(!strcmp(g.hex.path, "/home/big.log"));
    gui_key(&g, '\r');
}

static void test_charmap_paste(void) {
    fake_desktop(&g, 960, 540);
    gui_open(&g, GUI_CHARMAP);
    g.charmap.selected = 0xF7;                          /* the console's wave */
    gui_act(&g, ACT_COPY);
    CHECK(g.clip_len == 3 && !memcmp(g.clip, "\xE2\x89\x8B", 3));

    /* into Notepad (Ctrl+V): its UTF-8 bytes */
    gui_open(&g, GUI_NOTEPAD);
    gui_key(&g, 22);
    CHECK(g.note.len == 3 && !memcmp(g.note.text, "\xE2\x89\x8B", 3));

    /* into the Terminal (Edit > Paste; Ctrl+V is the session's own key there): the session gets the bytes */
    gui_open(&g, GUI_TERMINAL);
    ticks(3);
    CHECK(fake_term_running && g.focus == GUI_TERMINAL);
    fake_term_in_len = 0;
    gui_act(&g, ACT_PASTE);
    ticks(3);
    CHECK(fake_term_in_len == 3 && !memcmp(fake_term_in, "\xE2\x89\x8B", 3));
    fake_term_in_len = 0;
    gui_key(&g, 22);
    ticks(3);
    CHECK(fake_term_in_len == 1 && fake_term_in[0] == 22);
    gui_finish(&g);
    CHECK(!fake_term_running);
}

/* 10/18/26: the Image Viewer and Hex take the arrow keys only with something to move; otherwise the arrows move
   the pointer (shell/gui.c asks gui_wants_arrows() before the keys reach the desktop) */
static void test_arrows(void) {
    fake_desktop(&g, 960, 540);
    make_files();
    gui_open(&g, GUI_VIEWER);
    CHECK(g.focus == GUI_VIEWER && !gui_wants_arrows(&g));
    viewer_show(&g, "/home/wave.bmp");
    ticks(5);
    CHECK(g.viewer.ready && gui_wants_arrows(&g));
    gui_open(&g, GUI_HEX);
    CHECK(g.focus == GUI_HEX && !gui_wants_arrows(&g));
    hex_show(&g, "/home/prog.bin");
    CHECK(gui_wants_arrows(&g));
    gui_open(&g, GUI_CALC);                             /* as before: Calculator's arrows are the pointer's */
    CHECK(!gui_wants_arrows(&g));
    gui_open(&g, GUI_LOG);                              /* an application without the hook: its .arrows */
    CHECK(gui_wants_arrows(&g));
}

/* the left button held at (x, y) (window-relative to id's window), or let go of there */
static void hold(int id, int x, int y, bool held) {
    struct pointer_event e;
    memset(&e, 0, sizeof(e));
    e.x = g.win[id].x + x;
    e.y = g.win[id].y + y;
    e.buttons = held ? POINTER_LEFT : 0;
    gui_pointer(&g, &e);
}

/* 10/18/26: a drag nobody holds any more ends as at a release (gui_release()): the desktop got the screen back with
   no pointer event to say the button went up (shell/gui.c), or the window was minimized mid-drag. Before, the Log,
   held past its top edge, went on scrolling by itself and redrawing the desktop 20 times a second */
static void test_release(void) {
    int i, n = 0, first, rows, total, now;
    fake_desktop(&g, 960, 540);
    for(i = 0; i < 300; i++) n += snprintf(fake_log + n, sizeof(fake_log) - (u32)n, "line %04d of the kernel log\n", i);
    fake_log_written = (u32)n;
    gui_open(&g, GUI_LOG);
    ticks(30);
    log_view(&g, &first, &rows, &total);
    CHECK(total >= 300 && rows > 0);
    hold(GUI_LOG, g.win[GUI_LOG].w / 2, g.win[GUI_LOG].h / 2, true);    /* a line chosen, then held past the top */
    hold(GUI_LOG, g.win[GUI_LOG].w / 2, 4, true);
    CHECK(g.app_drag == GUI_LOG && g.logview.drag);
    ticks(10);
    log_view(&g, &now, &rows, &total);
    CHECK(now < first);                                 /* held there, it scrolls up */
    g.buttons = 0;                                      /* the screen came back, as shell/gui.c has it */
    gui_release(&g);
    CHECK(g.app_drag == -1 && !g.logview.drag);
    log_view(&g, &first, &rows, &total);
    ticks(50);
    log_view(&g, &now, &rows, &total);
    CHECK(now == first);                                /* and stays put */
    /* minimized mid-drag (from the keyboard, the button still held): the drag ends too */
    hold(GUI_LOG, g.win[GUI_LOG].w / 2, g.win[GUI_LOG].h / 2, true);
    hold(GUI_LOG, g.win[GUI_LOG].w / 2, 4, true);
    CHECK(g.app_drag == GUI_LOG && g.logview.drag);
    gui_act(&g, ACT_MINIMIZE);
    CHECK(g.win[GUI_LOG].min && g.app_drag == -1 && !g.logview.drag);
    log_view(&g, &first, &rows, &total);
    ticks(50);
    log_view(&g, &now, &rows, &total);
    CHECK(now == first);
    hold(GUI_LOG, 0, 0, false);
}

/* 10/18/26 (with ai-dev's guarded saves): Sketch's Save as BMP never saves over a file that may share its clusters
   with a new file kept beside it (note_refused()), as Notepad's save does not; elsewhere it saves */
static void test_sketch_guarded(void) {
    const char *d;
    u32 len = 0;
    fake_desktop(&g, 960, 540);
    fake_file("/home/s.bmp", "picture", 7);
    fake_file("/home/s.bmp.tmp", "picture", 7);         /* the same size and time: a pair a rename may have left */
    CHECK(sketch_save(&g, "/home/s.bmp") == -EBUSY);
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "Not saved"));
    d = fake_data("/home/s.bmp", &len);
    CHECK(d && len == 7 && !memcmp(d, "picture", 7));
    gui_key(&g, '\r');
    CHECK(!g.dialog.kind && sketch_save(&g, "/home/other.bmp") == 0);
    CHECK(fake_data("/home/other.bmp", &len) && len == GUI_SKETCH_FILE);
}

/* a window of the desktop's own, not an application that is a program now (10/26/26, stage C12: the Clock) */
static bool own(int id) { return !gui_app_of(id) || !gui_app_of(id)->program; }

/* every window open at once, in front in turn, drawn inside the screen; then nothing is left running */
static void test_all_windows(int width, int height) {
    int id, front;
    fake_desktop(&g, width, height);
    fake_screen(&screen, width, height);
    for(id = 0; id < GUI_WINDOWS; id++) if(own(id)) gui_open(&g, id);
    ticks(3);
    for(id = 0; id < GUI_WINDOWS; id++) CHECK(g.win[id].open == own(id));
    for(front = 0; front < GUI_WINDOWS; front++) {
        if(!own(front)) continue;
        gui_open(&g, front);
        CHECK(g.focus == front);
        gui_render(&g, &screen);
        CHECK(fake_guards(width, height));
    }
    CHECK(fake_term_running);
    gui_finish(&g);
    CHECK(!fake_term_running);
}

int main(void) {
    test_files_open();
    test_charmap_paste();
    test_arrows();
    test_release();
    test_sketch_guarded();
    test_all_windows(640, 400);
    test_all_windows(960, 540);
    test_all_windows(1280, 800);
    return fake_done("test_guisuite");
}
