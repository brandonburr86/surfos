/* Host tests for the desktop's side of the desktop applications (docs/superpowers/plans/2026-11-05-desktop-apps.md,
   track desk), against the in-memory files and system of tests/host/gui_fake.c: the application registry
   (shell/guireg.c: the format of rootfs/apps/README, every refusal and its line in the log, every bound, the
   files of rootfs/apps/ themselves), Apps > Programs and its tiles at three sizes, Files opening a file in the
   program its type belongs to and Open with, screenshots (the chooser in the first writable volume, the save, its
   care, a dialog in the way), and the preferences on the stick (read at start, Settings' change applied at once,
   Pointer & desktop's saved). Built and run by `make unittest` (unittest-desk). */
#include "gui_fake.h"

static struct gui_state g;

static void tick(void) { gui_tick(&g); gui_input_done(&g); }
static void key(int k) { gui_key(&g, k); }

static void draw(int width, int height) {
    struct fb_surface s;
    fake_screen(&s, width, height);
    gui_render(&g, &s);
}

static const char ICON[] =
    "icon = ................\n" "icon = .KKKKKKKKKKKKKK.\n" "icon = .KPPPPPPPPPPPPK.\n" "icon = .KPRRPPGGPPBBPK.\n"
    "icon = .KPRRPPGGPPBBPK.\n" "icon = .KPPPPPPPPPPPPK.\n" "icon = .KPCCPPSSPPYYPK.\n" "icon = .KPCCPPSSPPYYPK.\n"
    "icon = .KPPPPPPPPPPPPK.\n" "icon = .KPDDPPWWPPLLPK.\n" "icon = .KPDDPPWWPPLLPK.\n" "icon = .KPPPPPPPPPPPPK.\n"
    "icon = .KPAAPPMMPPFFPK.\n" "icon = .KPPPPPPPPPPPPK.\n" "icon = .KKKKKKKKKKKKKK.\n" "icon = ................\n";

/* text parses as an entry: 0; or the refusal's why contains want */
static int parse(const char *text, struct gui_reg *r, char *why) {
    return gui_reg_parse(text, (u32)strlen(text), r, why, 96);
}

static bool refused(const char *text, const char *want) {
    struct gui_reg r;
    char why[96];
    bool ok = parse(text, &r, why) < 0 && strstr(why, want) != NULL;
    if(!ok) printf("  (parse of [%s] said [%s], not [%s])\n", text, why, want);
    return ok;
}

static void test_parse(void) {
    struct gui_reg r;
    char why[96], text[GUI_REG_FILE + 64], big[200];
    int i;
    CHECK(parse("program = wpaint\ntitle = Paint\n", &r, why) == 0 && !strcmp(r.program, "wpaint") &&
          !strcmp(r.title, "Paint") && r.apps && !r.desktop && !r.ntypes && !r.icon_set && r.app == -1);
    snprintf(text, sizeof(text), "# Paint: pictures\r\n  program=wpaint  \r\n\ttitle =  Paint  Pro \r\n"
             "types = BMP png\r\nopen-with = gif jpg JPEG\r\napps = no\r\ndesktop = yes\r\n\r\n%s", ICON);
    CHECK(parse(text, &r, why) == 0 && !strcmp(r.title, "Paint  Pro") && r.ntypes == 2 && !strcmp(r.types[0], "bmp") &&
          r.nopen == 3 && !strcmp(r.open_with[2], "jpeg") && !r.apps && r.desktop && r.icon_set &&
          r.icon[3][3] == 'R' && r.icon[0][0] == '.');
    CHECK(reg_takes(&r, "sea.PNG", false) && !reg_takes(&r, "a.gif", false) && reg_takes(&r, "a.Gif", true) &&
          !reg_takes(&r, "a.tiff", true) && !reg_takes(&r, "a.averyverylongtypename", true) && !reg_takes(&r, "png", true));
    /* 11/05/26 (the review): types as a program's chooser takes them (surfos/win.h's win_types()): whole names too */
    CHECK(parse("program = wcode\ntitle = Code\ntypes = c h =Makefile =README.txt abcdefghijklmno\n", &r, why) == 0 &&
          r.ntypes == 5 && !strcmp(r.types[2], "=makefile") && !strcmp(r.types[4], "abcdefghijklmno"));
    CHECK(reg_takes(&r, "Makefile", false) && reg_takes(&r, "MAKEFILE", false) && reg_takes(&r, "readme.TXT", false) &&
          !reg_takes(&r, "x.txt", false) && !reg_takes(&r, "Makefile.in", false) && reg_takes(&r, "main.c", false));
    CHECK(gui_reg_color('K') == INK && gui_reg_color('D') == 0 && gui_reg_color('W') == 0xFFFFFF &&
          gui_reg_color('.') == 0xFF000000 && gui_reg_color('Q') == 0xFF000000 && gui_reg_color(0) == 0xFF000000);
    /* every refusal names its line and why */
    CHECK(refused("title = Paint\n", "no program"));
    CHECK(refused("program = wpaint\n", "no title"));
    CHECK(refused("program = wpaint\ntitle = Paint\ncolour = red\n", "line 3: a key: not one the desktop knows"));
    CHECK(refused("program = wpaint\ntitle = Paint\n\x1b[2J = red\n", "line 3: a key: not one the desktop knows"));
    CHECK(refused("program = wpaint\nprogram = other\ntitle = x\n", "line 2: program: given twice"));
    CHECK(refused("program wpaint\n", "line 1: not key = value"));
    CHECK(refused(" = wpaint\n", "line 1: not key = value"));
    CHECK(refused("program = ../evil\ntitle = x\n", "a program is"));
    CHECK(refused("program = .hidden\ntitle = x\n", "a program is"));
    CHECK(refused("program = /initrd/bin/wpaint\ntitle = x\n", "a program is"));
    CHECK(refused("program = abcdefghijabcdefghijabcdefghijab\ntitle = x\n", "a program is"));     /* 32 */
    CHECK(parse("program = abcdefghijabcdefghijabcdefghija\ntitle = x\n", &r, why) == 0);       /* 31 */
    CHECK(refused("program = p\ntitle = abcdefghijabcdefghijabcdefghijab\n", "a title is 1 to 31 bytes"));
    CHECK(refused("program = p\ntitle = a\x01" "b\n", "no control characters"));
    /* (the review of stage 2) a title as a program's menu titles: UTF-8 by the console's rules, no C1, none that passes
       for the desktop's (its sign, its menus' names) */
    CHECK(refused("program = p\ntitle = caf\xE9\n", "UTF-8"));
    CHECK(refused("program = p\ntitle = a\xC2\x85" "b\n", "UTF-8"));
    CHECK(refused("program = p\ntitle = \xE2\x89\x8B Paint\n", "no sign of the desktop's"));
    CHECK(refused("program = p\ntitle = My \xE2\x89\x88 app\n", "no sign of the desktop's"));
    CHECK(refused("program = p\ntitle = Apps\n", "none of the desktop's menus"));
    CHECK(refused("program = p\ntitle = \xC2\xA0" "surfOS\n", "none of the desktop's menus"));
    CHECK(refused("program = p\ntitle = Window:\n", "none of the desktop's menus"));     /* 11/09/26: punctuation after */
    CHECK(parse("program = p\ntitle = Caf\xC3\xA9 \xE2\x80\x94 Applications\n", &r, why) == 0);
    CHECK(refused("program = p\ntitle = x\ntypes = .png\n", "line 3: types: not a list of types"));
    CHECK(refused("program = p\ntitle = x\ntypes = png,bmp\n", "not a list of types"));
    CHECK(refused("program = p\ntitle = x\ntypes = abcdefghijklmnop\n", "not a list of types"));     /* 16 */
    CHECK(refused("program = p\ntitle = x\ntypes = a b c d e f g h i j k l m n o p q\n", "not a list of types"));
    CHECK(refused("program = p\ntitle = x\ntypes = png =\n", "not a list of types"));
    CHECK(refused("program = p\ntitle = x\nopen-with = =a/b\n", "line 3: open-with: not a list of types"));
    CHECK(refused("program = p\ntitle = x\ntypes = png\tbmp\n", "types are separated by spaces"));
    CHECK(parse("program = p\ntitle = x\ntypes = a b c d e f g h i j k l m n o p\n", &r, why) == 0 && r.ntypes == 16);
    CHECK(refused("program = p\ntitle = x\ntypes =\n", "no type is given"));
    CHECK(refused("program = p\ntitle = x\napps = maybe\n", "apps: yes or no"));
    CHECK(refused("program = p\ntitle = x\ndesktop = YES\n", "desktop: yes or no"));
    CHECK(refused("program = p\ntitle = x\nicon = ...\n", "an icon line is 16 characters"));
    CHECK(refused("program = p\ntitle = x\nicon = ...............Q\n", "not in the palette"));
    CHECK(refused("program = p\ntitle = x\nicon = ................\n", "1 icon lines, not 16"));
    snprintf(text, sizeof(text), "program = p\ntitle = x\n%sicon = ................\n", ICON);
    CHECK(refused(text, "more than 16 icon lines"));
    memset(big, 'a', sizeof(big));
    memcpy(big, "title = ", 8);
    big[sizeof(big) - 1] = 0;
    snprintf(text, sizeof(text), "program = p\n%s\n", big);
    CHECK(refused(text, "line 2: longer than 128 bytes"));
    memcpy(text, "program = p\ntitle = x\0y\n", 24);
    CHECK(gui_reg_parse(text, 24, &r, why, sizeof(why)) < 0 && strstr(why, "line 2: a NUL byte"));
    memset(text, '#', sizeof(text));                    /* a file longer than GUI_REG_FILE, read whole or not at all */
    for(i = 64; i < (int)sizeof(text); i += 64) text[i] = '\n';
    CHECK(gui_reg_parse(text, GUI_REG_FILE + 1, &r, why, sizeof(why)) < 0 && strstr(why, "longer than 4096"));
    snprintf(text, sizeof(text), "program = p\ntitle = x");             /* no line end at the end */
    CHECK(parse(text, &r, why) == 0 && !strcmp(r.title, "x"));
}

/* a desktop with these .app files in /initrd/apps, and the programs they name in /initrd/bin */
static void with_apps(int width, int height) {
    fake_desktop(&g, width, height);
    fake_dir("/initrd/apps");
    fake_dir("/initrd/bin");
}

static void app(const char *file, const char *text) {
    char path[GUI_PATH_MAX];
    snprintf(path, sizeof(path), "/initrd/apps/%s", file);
    fake_file(path, text, (u32)strlen(text));
}

static void bin(const char *name) {
    char path[GUI_PATH_MAX];
    snprintf(path, sizeof(path), "/initrd/bin/%s", name);
    fake_file(path, "\x7f" "ELF", 4);
}

static void test_load(void) {
    char name[32], text[128];
    int i;
    with_apps(960, 540);
    bin("wpaint");
    bin("wsheet");
    bin("wview");
    bin("wclock");
    app("paint.app", "program = wpaint\ntitle = Paint\ntypes = bmp png\nopen-with = gif\ndesktop = yes\n");
    app("sheet.app", "program = wsheet\ntitle = spreadsheet\ntypes = csv\ndesktop = yes\n");
    app("viewer.app", "program = wview\ntitle = Image Viewer\ntypes = png gif jpg\napps = no\n");
    app("broken.app", "program = wbroken\ntitle = Broken\n");        /* no such program */
    app("typo.app", "program = wview\ntittle = Sheet\n");
    app("clock.app", "program = wclock\ntitle = Clock\ntypes = ics\n");   /* the desktop's own Clock's program */
    fake_file("/initrd/apps/README", "not an entry", 12);
    fake_dir("/initrd/apps/dir.app");
    tick();
    CHECK(g.reg.loaded && g.reg.count == 4);            /* broken and typo left out, README and dir.app not looked at */
    CHECK(!strcmp(g.reg.e[0].file, "clock.app") && !strcmp(g.reg.e[1].file, "paint.app") &&
          !strcmp(g.reg.e[2].file, "sheet.app") && !strcmp(g.reg.e[3].file, "viewer.app"));   /* by file name */
    CHECK(strstr(fake_notes, "gui: apps/broken.app: program: /initrd/bin/wbroken is not there\n") &&
          strstr(fake_notes, "gui: apps/typo.app: line 2: a key: not one the desktop knows\n") &&
          strstr(fake_notes, "gui: apps/viewer.app: png is apps/paint.app's, the first that names it\n"));
    CHECK(g.reg.e[0].app == GUI_CLOCK && g.reg.e[1].app == -1);     /* the Clock's program: the Clock's place */
    CHECK(g.reg.by[0] == 0 && g.reg.by[1] == 3 && g.reg.by[2] == 1 && g.reg.by[3] == 2);   /* by title, any case */
    CHECK(reg_for(&g, "sea.PNG") == 1 && reg_for(&g, "sea.gif") == 3 && reg_for(&g, "a.csv") == 2 &&
          reg_for(&g, "plain.txt") == -1 && reg_for(&g, "noext") == -1 && reg_for(&g, ".png") == -1);
    /* Apps > Programs: those with apps = yes and no desktop application's program, by title */
    CHECK(gui_menu_on(&g, 4, ACT_PROGRAMS) && gui_menu_on(&g, MENU_PROGRAMS, ACT_REG_START + 1) &&
          gui_menu_on(&g, MENU_PROGRAMS, ACT_REG_START + 2) && !gui_menu_on(&g, MENU_PROGRAMS, ACT_REG_START + 3) &&
          !gui_menu_on(&g, MENU_PROGRAMS, ACT_REG_START + 0));
    /* 11/05/26 (the review): one entry a program, the first by file name; a file name the log may not say, left out
       without it */
    with_apps(960, 540);
    bin("wpaint");
    app("a-paint.app", "program = wpaint\ntitle = Paint\n");
    app("b-paint.app", "program = wpaint\ntitle = Paint again\ntypes = bmp\n");
    app("evil\x1b[2J.app", "program = wpaint\ntitle = Evil\n");
    app("sp ace.app", "program = wpaint\ntitle = Space\n");
    tick();
    CHECK(g.reg.count == 1 && !strcmp(g.reg.e[0].file, "a-paint.app"));
    CHECK(strstr(fake_notes, "gui: apps/b-paint.app: program: wpaint has an entry already, apps/a-paint.app\n") &&
          strstr(fake_notes, "gui: apps/: a .app file whose name is not letters, digits, '.', '-' and '_' is left out\n") &&
          !strchr(fake_notes, 0x1b) && !strstr(fake_notes, "sp ace"));
    /* the bound: the first 32 .app files by name */
    with_apps(960, 540);
    for(i = 0; i < GUI_REG_MAX + 8; i++) {
        snprintf(name, sizeof(name), "p%02d.app", GUI_REG_MAX + 7 - i);
        snprintf(text, sizeof(text), "program = wx%02d\ntitle = P%02d\n", GUI_REG_MAX + 7 - i, GUI_REG_MAX + 7 - i);
        app(name, text);
        snprintf(name, sizeof(name), "wx%02d", i);
        bin(name);
    }
    tick();
    CHECK(g.reg.count == GUI_REG_MAX && !strcmp(g.reg.e[0].file, "p00.app") &&
          !strcmp(g.reg.e[GUI_REG_MAX - 1].file, "p31.app"));
    /* none at all: no Programs */
    fake_desktop(&g, 960, 540);
    tick();
    CHECK(g.reg.loaded && g.reg.count == 0 && !gui_menu_on(&g, 4, ACT_PROGRAMS));
}

static void test_programs_menu(int width, int height) {
    char name[32], text[600];
    int i, x, y, w, h, before;
    with_apps(width, height);
    for(i = 0; i < 20; i++) {                           /* more than a menu shows at 640x400 */
        snprintf(name, sizeof(name), "p%02d.app", i);
        snprintf(text, sizeof(text), "program = wx%02d\ntitle = Program %02d\ndesktop = yes\n%s", i, i, i == 3 ? ICON : "");
        app(name, text);
        snprintf(name, sizeof(name), "wx%02d", i);
        bin(name);
    }
    tick();
    CHECK(g.reg.count == 20);
    /* Apps: Run program... keeps its place (13 down from Notepad), Programs after it */
    key(7);
    for(i = 0; i < 3; i++) key(GUI_RIGHT);
    CHECK(g.menu == 4);
    for(i = 0; i < 13; i++) key(GUI_DOWN);
    gui_menu_box(&g, 4, &x, &y, &w, &h);
    CHECK(y + h <= height && x + w <= width);           /* inside the screen, scrolled where it must be */
    key(GUI_DOWN);                                      /* Programs: Right goes in */
    key(GUI_RIGHT);
    CHECK(g.menu == MENU_PROGRAMS && g.selection == 0);
    draw(width, height);
    CHECK(fake_guards(width, height));
    gui_menu_box(&g, MENU_PROGRAMS, &x, &y, &w, &h);
    CHECK(y + h <= height && x + w <= width);
    for(i = 0; i < 19; i++) key(GUI_DOWN);              /* to the last: it shows */
    draw(width, height);
    CHECK(g.selection == 19 && fake_guards(width, height));
    key(GUI_LEFT);                                      /* back to Apps, on Programs */
    CHECK(g.menu == 4 && g.selection >= 13);
    key(GUI_RIGHT);
    key('\r');                                          /* Program 00 */
    CHECK(g.menu == 0 && fake_run_count == 1 && !strcmp(fake_run_line, "wx00") && g.reg.pid[0] == 77);
    before = fake_run_count;
    gui_act(&g, ACT_REG_START + 0);                     /* again before its window came: nothing (a double-click) */
    CHECK(fake_run_count == before);
    fake_win_present(fake_win_open(100, 80, 0, 0x203040, "Program 00", 77), 0xABCDEF);
    tick();
    CHECK(client_of_pid(&g, 77) >= 0 && g.focus == client_of_pid(&g, 77));     /* the launcher's: the focus */
    gui_act(&g, ACT_NOTEPAD);
    gui_act(&g, ACT_REG_START + 0);                     /* its window back, no second program */
    CHECK(fake_run_count == before && g.focus == client_of_pid(&g, 77));
    draw(width, height);                                /* its icon: the registry's (none: the programs') */
    CHECK(fake_guards(width, height));
    /* the tiles: after the desktop's own and the suite's, as many as fit */
    draw(width, height);
    CHECK(fake_guards(width, height));
}

static void test_tiles(void) {
    int i;
    with_apps(960, 540);
    bin("wx");
    bin("wy");
    app("a.app", "program = wx\ntitle = Alpha\ndesktop = yes\n");
    app("b.app", "program = wy\ntitle = Beta\n");      /* no tile */
    tick();
    fake_run_count = 0;
    /* the suite's 8 tiles fill two columns after the desktop's: Alpha is the first of the fourth, at (480, 120) */
    fake_click(&g, 480 + 70, 120 + 20);
    CHECK(fake_run_count == 1 && !strcmp(fake_run_line, "wx"));
    for(i = 0; i < 4; i++) fake_click(&g, 480 + 70, 120 + 52 + 20);     /* Beta has none: nothing below */
    CHECK(fake_run_count == 1);
    /* P3 (the review of stage 2): a tile the screen has no room for is neither drawn nor clicked; at 640x400 the
       suite's fill the third and fourth columns, the registry's would start at the fifth, past the right edge */
    with_apps(640, 400);
    for(i = 0; i < 6; i++) {
        char name[32], text[80];
        snprintf(name, sizeof(name), "p%d.app", i);
        snprintf(text, sizeof(text), "program = wx%d\ntitle = Tile %d\ndesktop = yes\n", i, i);
        app(name, text);
        snprintf(name, sizeof(name), "wx%d", i);
        bin(name);
    }
    tick();
    fake_run_count = 0;
    fake_click(&g, 636, 140);
    fake_click(&g, 630, 300);
    CHECK(fake_run_count == 0);
}

static void test_files_open(void) {
    int k, row, i;
    with_apps(960, 540);
    bin("wpaint");
    bin("wtext");
    app("paint.app", "program = wpaint\ntitle = Paint\ntypes = png\nopen-with = bmp txt\n");
    app("text.app", "program = wtext\ntitle = Writer\nopen-with = txt\n");
    fake_dir("/home/My pics");
    fake_file("/home/My pics/sea view.png", "png", 3);
    fake_file("/home/notes.txt", "hello", 5);
    fake_file("/home/odd.xyz", "odd", 3);
    tick();
    gui_act(&g, ACT_FILES);
    for(row = 0; row < g.files.count && strcmp(g.files.node[row].name, "home"); row++) ;
    g.files.selected = row;
    files_act(&g, ACT_FILES_OPEN);                      /* /home opens */
    for(k = 0; k < g.files.count && strcmp(g.files.node[k].name, "My pics"); k++) ;
    g.files.selected = k;
    files_act(&g, ACT_FILES_OPEN);
    for(k = 0; k < g.files.count && strcmp(g.files.node[k].name, "sea view.png"); k++) ;
    CHECK(k < g.files.count);
    g.files.selected = k;
    files_act(&g, ACT_FILES_OPEN);                      /* its type is Paint's: started with the path, spaces and all */
    CHECK(fake_run_file_count == 1 && !strcmp(fake_run_program, "wpaint") &&
          !strcmp(fake_run_arg, "/home/My pics/sea view.png") && reg_of_pid(&g, 77) == 0);
    /* Open with: every program that takes it, then Notepad and Hex */
    CHECK(gui_menu_on(&g, 2, ACT_OPEN_WITH));
    CHECK(gui_menu_on(&g, MENU_OPEN_WITH, ACT_REG_OPEN + 0) && !gui_menu_on(&g, MENU_OPEN_WITH, ACT_REG_OPEN + 1) &&
          gui_menu_on(&g, MENU_OPEN_WITH, ACT_WITH_NOTEPAD) && gui_menu_on(&g, MENU_OPEN_WITH, ACT_WITH_HEX));
    for(k = 0; k < g.files.count && strcmp(g.files.node[k].name, "notes.txt"); k++) ;
    g.files.selected = k;
    CHECK(gui_menu_on(&g, MENU_OPEN_WITH, ACT_REG_OPEN + 0) && gui_menu_on(&g, MENU_OPEN_WITH, ACT_REG_OPEN + 1));
    files_act(&g, ACT_FILES_OPEN);                      /* named by no types: Notepad's, as before */
    CHECK(g.focus == GUI_NOTEPAD && !strcmp(g.note.path, "/home/notes.txt") && fake_run_file_count == 1);
    gui_act(&g, ACT_FILES);
    key(7);                                             /* File > Open with > Writer, by the keyboard */
    key(GUI_RIGHT);
    CHECK(g.menu == 2);
    key(GUI_DOWN);                                      /* Open with */
    key(GUI_RIGHT);
    CHECK(g.menu == MENU_OPEN_WITH);
    for(i = 0; i < 4 && g.selection >= 0; i++) {
        if(gui_menu_on(&g, MENU_OPEN_WITH, ACT_REG_OPEN + 1) && g.selection == 1) break;
        key(GUI_DOWN);
    }
    key('\r');
    CHECK(fake_run_file_count == 2 && !strcmp(fake_run_program, "wtext") && !strcmp(fake_run_arg, "/home/notes.txt"));
    gui_act(&g, ACT_WITH_HEX);                          /* Open with > Hex */
    CHECK(g.focus == GUI_HEX);
    gui_act(&g, ACT_FILES);
    fake_run_result = -2;                               /* a program that cannot start: said */
    gui_act(&g, ACT_REG_OPEN + 1);
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "Not opened"));
    key('\r');
    fake_run_result = 77;
    for(k = 0; k < g.files.count && strcmp(g.files.node[k].name, "odd.xyz"); k++) ;
    g.files.selected = k;
    CHECK(!gui_menu_on(&g, MENU_OPEN_WITH, ACT_REG_OPEN + 0) && gui_menu_on(&g, MENU_OPEN_WITH, ACT_WITH_NOTEPAD));
    g.files.selected = row;                             /* a folder: no Open with */
    CHECK(!gui_menu_on(&g, 2, ACT_OPEN_WITH));
}

static void test_screenshot(void) {
    static char bmp[54 + 960 * 3 * 2];
    u32 len;
    const char *data;
    with_apps(960, 540);
    tick();
    gui_act(&g, ACT_SCREENSHOT);                        /* SurfOS > Screenshot: the next frame is captured */
    CHECK(g.shot.state == GUI_SHOT_WANTED && !strcmp(g.shot.name, "Screenshot 2026-10-17 140530.bmp") && g.dirty);
    gui_act(&g, ACT_SCREENSHOT);                        /* one at a time */
    CHECK(g.shot.state == GUI_SHOT_WANTED);
    memset(bmp, 0x5A, sizeof(bmp));                     /* the shell's part: the frame as a BMP */
    g.shot.data = bmp;
    g.shot.len = sizeof(bmp);
    g.shot.state = GUI_SHOT_TAKEN;
    tick();                                             /* the chooser, in / (no stick mounted) */
    CHECK(g.dialog.kind == GUI_DLG_SAVE && g.shot.state == GUI_SHOT_ASKING && !strcmp(g.dialog.dir, "/") &&
          !strcmp(g.dialog.field.text, "Screenshot 2026-10-17 140530.bmp") && !strcmp(g.dialog.title, "Save the screenshot"));
    CHECK(!g.dialog.program);                           /* the desktop's own: its sign (the review of stage 2) */
    gui_key(&g, 27);
    CHECK(!g.dialog.kind && g.shot.state == GUI_SHOT_DONE);   /* cancelled: the shell frees it */
    g.shot.state = GUI_SHOT_NONE;
    gui_act(&g, ACT_SCREENSHOT);
    g.shot.data = bmp;
    g.shot.len = sizeof(bmp);
    g.shot.state = GUI_SHOT_TAKEN;
    tick();
    field_set(&g.dialog.field, "/home", false);         /* (a name: the folder's own is refused) */
    field_set(&g.dialog.field, "shot", true);
    gui_key(&g, 27);
    g.shot.state = GUI_SHOT_NONE;
    /* into /home, as a .bmp: saved through the safe save */
    gui_act(&g, ACT_SCREENSHOT);
    g.shot.data = bmp;
    g.shot.len = sizeof(bmp);
    g.shot.state = GUI_SHOT_TAKEN;
    tick();
    CHECK(g.dialog.selected == 0 && !strcmp(g.dialog.list[0].name, "home"));
    key(GUI_DOWN);                                      /* the list: initrd, then home again */
    key(GUI_UP);
    key('\r');                                          /* Enter in the list goes into home */
    CHECK(!strcmp(g.dialog.dir, "/home") && !g.dialog.list_focus);
    field_set(&g.dialog.field, "shot", true);          /* a name without its type: .bmp */
    key('\r');
    data = fake_data("/home/shot.bmp", &len);
    CHECK(data && len == sizeof(bmp) && !memcmp(data, bmp, len) && g.shot.state == GUI_SHOT_DONE && !g.dialog.kind);
    g.shot.state = GUI_SHOT_NONE;
    /* there already: Replace asked; refused for a file the desktop takes care of */
    gui_act(&g, ACT_SCREENSHOT);
    g.shot.data = bmp;
    g.shot.len = 10;
    g.shot.state = GUI_SHOT_TAKEN;
    tick();
    field_set(&g.dialog.field, "shot.bmp", true);
    strlcpy(g.dialog.dir, "/home", sizeof(g.dialog.dir));
    key('\r');
    CHECK(g.dialog.kind == GUI_DLG_CONFIRM && !strcmp(g.dialog.title, "Replace the file?"));
    key('\r');
    data = fake_data("/home/shot.bmp", &len);
    CHECK(data && len == 10 && g.shot.state == GUI_SHOT_DONE);
    g.shot.state = GUI_SHOT_NONE;
    strlcpy(g.unsure[0], "/home/shot.bmp", GUI_PATH_MAX);
    strlcpy(g.kept[0], "/home/shot.bmp.tmp", GUI_PATH_MAX);
    g.nunsure = 1;
    gui_act(&g, ACT_SCREENSHOT);
    g.shot.data = bmp;
    g.shot.len = 10;
    g.shot.state = GUI_SHOT_TAKEN;
    tick();
    field_set(&g.dialog.field, "shot.bmp", true);
    strlcpy(g.dialog.dir, "/home", sizeof(g.dialog.dir));
    key('\r');
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "Screenshot not saved") &&
          g.shot.state == GUI_SHOT_DONE);
    key('\r');
    g.nunsure = 0;
    g.shot.state = GUI_SHOT_NONE;
    /* a write that fails: said */
    gui_act(&g, ACT_SCREENSHOT);
    g.shot.data = bmp;
    g.shot.len = 10;
    g.shot.state = GUI_SHOT_TAKEN;
    tick();
    field_set(&g.dialog.field, "full.bmp", true);
    strlcpy(g.dialog.dir, "/home", sizeof(g.dialog.dir));
    fake_fail_write = -ENOSPC;
    key('\r');
    fake_fail_write = 0;
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "Screenshot not saved") && !fake_data("/home/full.bmp", &len));
    key('\r');
    g.shot.state = GUI_SHOT_NONE;
    /* a dialog up: the chooser waits for it; one that replaces the chooser drops the screenshot */
    gui_act(&g, ACT_SCREENSHOT);
    g.shot.data = bmp;
    g.shot.state = GUI_SHOT_TAKEN;
    dialog_message(&g, "In the way", "", "");
    tick();
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && g.shot.state == GUI_SHOT_TAKEN);
    key('\r');
    tick();
    CHECK(g.dialog.kind == GUI_DLG_SAVE && g.shot.state == GUI_SHOT_ASKING);
    dialog_message(&g, "Something else", "", "");
    CHECK(g.shot.state == GUI_SHOT_DONE);
    key('\r');
    g.shot.state = GUI_SHOT_NONE;
    /* no memory for the picture: said */
    gui_act(&g, ACT_SCREENSHOT);
    g.shot.data = NULL;
    g.shot.state = GUI_SHOT_TAKEN;
    tick();
    CHECK(g.dialog.kind == GUI_DLG_MESSAGE && !strcmp(g.dialog.title, "No screenshot") && g.shot.state == GUI_SHOT_DONE);
    key('\r');
    g.shot.state = GUI_SHOT_NONE;
    CHECK(gui_menu_on(&g, 1, ACT_SCREENSHOT));
}

static void test_volume_dir(void) {
    char dir[GUI_PATH_MAX];
    with_apps(960, 540);
    gui_volume_dir(&g, dir, sizeof(dir));
    CHECK(!strcmp(dir, "/"));                           /* the fake has no volumes mounted unless a test mounts one */
}

static void test_prefs(void) {
    int i;
    with_apps(960, 540);
    fake_pref_theme = 1;                                /* the stick says dusk and fast */
    fake_pref_speed = 3;
    tick();
    CHECK(g.night && g.speed == 3);
    fake_pref_theme = 0;                                /* Settings changes the theme: within a second, here */
    g.now += 500;
    tick();
    CHECK(g.night);
    g.now += 600;
    tick();
    CHECK(!g.night && g.speed == 3);
    gui_act(&g, ACT_SETTINGS);                          /* Pointer & desktop: 2, then T */
    key('2');                                           /* (the review of stage 2) in force at once, */
    CHECK(g.speed == 2 && fake_pref_sets == 0 && !strcmp(g.pref_status, "Kept in the settings in a moment."));
    g.now += GUI_PREF_MS - 1;
    tick();
    CHECK(fake_pref_sets == 0);
    g.now += 1;                                         /* written once it has stood */
    tick();
    CHECK(fake_pref_sets == 1 && fake_pref_which == 1 && fake_pref_value == 2 &&
          !strcmp(g.pref_status, "Saved in the settings on the stick."));
    fake_pref_result = 1;                               /* no settings stick: in force, not saved */
    key('t');
    g.now += GUI_PREF_MS;
    tick();
    CHECK(g.night && fake_pref_which == 0 && fake_pref_value == 1 && strstr(g.pref_status, "not saved: no settings stick"));
    g.now += 1100;
    tick();
    CHECK(g.night && g.speed == 2);                     /* what it changed itself reads back the same */
    fake_pref_result = -1;                              /* refused: not in force there; the next read changes nothing */
    key('1');
    g.now += GUI_PREF_MS;
    tick();
    CHECK(g.speed == 1 && strstr(g.pref_status, "Not changed"));
    g.now += 1100;
    tick();
    CHECK(g.speed == 1);
    draw(960, 540);
    CHECK(fake_guards(960, 540));
    /* P4: T held down a second (the keyboard's repeats) toggles once; a run of clicks writes the stick once, with the
       last value; a change not written yet is written at the desktop's end */
    fake_pref_result = 0;
    fake_pref_sets = 0;
    gui_key(&g, 't');
    for(i = 0; i < 29; i++) gui_key(&g, 't' | GUI_KEY_REPEAT);
    CHECK(!g.night && fake_pref_sets == 0);             /* dusk to ocean, once */
    for(i = 0; i < 5; i++) {
        g.now += 300;
        tick();
        key('3' - (i & 1));                             /* 3, 2, 3, 2, 3: each before the one before stood */
    }
    CHECK(g.speed == 3 && fake_pref_sets == 0);
    g.now += GUI_PREF_MS;
    tick();
    CHECK(fake_pref_sets == 2 && fake_pref_speed == 3 && fake_pref_theme == 0);    /* each preference once */
    key('1');
    gui_finish(&g);                                     /* the desktop's end writes what waits */
    CHECK(fake_pref_sets == 3 && fake_pref_which == 1 && fake_pref_value == 1);
}

/* the host's own calls, to read the files of rootfs/apps/ (the test sees SurfOS's headers, as the kernel does) */
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
int close(int fd);

/* the files of rootfs/apps/ as they are (the tracks' entries; the Makefile names them): each one an entry */
static void test_the_tree(int argc, char **argv) {
    static char text[GUI_REG_FILE + 2];
    struct gui_reg r;
    char why[96];
    int i, fd;
    long len;
    for(i = 1; i < argc; i++) {
        fd = open(argv[i], 0);
        CHECK(fd >= 0);
        if(fd < 0) continue;
        len = read(fd, text, sizeof(text));
        close(fd);
        CHECK(len >= 0);
        if(len < 0) continue;
        if(gui_reg_parse(text, (u32)len, &r, why, sizeof(why)) < 0) printf("  %s: %s\n", argv[i], why);
        CHECK(gui_reg_parse(text, (u32)len, &r, why, sizeof(why)) == 0);
    }
    printf("test_guidesk: %d .app files of rootfs/apps read\n", argc - 1);
}

int main(int argc, char **argv) {
    test_parse();
    test_load();
    test_programs_menu(640, 400);
    test_programs_menu(960, 540);
    test_programs_menu(1280, 800);
    test_tiles();
    test_files_open();
    test_screenshot();
    test_volume_dir();
    test_prefs();
    test_the_tree(argc, argv);
    return fake_done("test_guidesk");
}
