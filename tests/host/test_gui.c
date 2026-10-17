#include <sys/gui.h>
#include <stdio.h>
#include <string.h>
#include "../../shell/guipriv.h"
static int checks, failed;
#define CHECK(x) do { checks++; if(!(x)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)
static struct gui_state g;
static u32 pixels[8 + 1280*800 + 8];

/**** an in-memory file system: /, /dev, /initrd (read-only), /home, and a USB stick usb1p1 ****/

struct fake { char path[GUI_PATH_MAX]; int type; char data[600]; u32 len, size; bool used; };
static struct fake nodes[96];
static bool mounted;
static int f_writes;

static bool on_stick(const char *p) { return !strncmp(p, "/usb1p1", 7) && (!p[7] || p[7] == '/'); }
static bool readonly_path(const char *p) { return !strncmp(p, "/initrd", 7) || !strncmp(p, "/dev", 4); }
static int find(const char *p) {
    int i;
    if(on_stick(p) && !mounted) return -1;
    for(i = 0; i < (int)(sizeof(nodes) / sizeof(nodes[0])); i++) if(nodes[i].used && !strcmp(nodes[i].path, p)) return i;
    return -1;
}
static void parent_of(const char *p, char *out) { strlcpy(out, p, GUI_PATH_MAX); gui_parent(out); }
static int add(const char *p, int type, const char *data, u32 len) {
    int i = find(p);
    if(i < 0) for(i = 0; nodes[i].used; i++) ;
    memset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[i].used = true;
    strlcpy(nodes[i].path, p, sizeof(nodes[i].path));
    nodes[i].type = type;
    if(data) memcpy(nodes[i].data, data, len);
    nodes[i].len = nodes[i].size = len;
    return i;
}
static int f_list(void *ctx, const char *path, struct gui_entry *out, int max) {
    char parent[GUI_PATH_MAX];
    int i = find(path), n = 0;
    if(i < 0) return -ENOENT;
    if(nodes[i].type != GUI_DIR) return -ENOTDIR;
    for(i = 0; i < (int)(sizeof(nodes) / sizeof(nodes[0])); i++) {
        if(!nodes[i].used || !strcmp(nodes[i].path, "/") || (on_stick(nodes[i].path) && !mounted)) continue;
        parent_of(nodes[i].path, parent);
        if(strcmp(parent, path)) continue;
        if(n < max) {
            strlcpy(out[n].name, gui_basename(nodes[i].path), sizeof(out[n].name));
            out[n].type = nodes[i].type;
            out[n].size = nodes[i].size;
            out[n].mtime = 1792245780;
        }
        n++;
    }
    return n;
}
static int f_stat(void *ctx, const char *path, struct gui_entry *out, bool *device, bool *readonly) {
    int i = find(path);
    if(i < 0) return -ENOENT;
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, gui_basename(path), sizeof(out->name));
    out->type = nodes[i].type;
    out->size = nodes[i].size;
    *device = !strncmp(path, "/dev/", 5);
    *readonly = readonly_path(path);
    return 0;
}
static int f_read(void *ctx, const char *path, char *buf, u32 size, u32 *len) {
    int i = find(path);
    if(i < 0) return -ENOENT;
    if(nodes[i].type == GUI_DIR) return -EISDIR;
    if(nodes[i].size > size) return -EFBIG;
    memcpy(buf, nodes[i].data, nodes[i].len);
    *len = nodes[i].len;
    return 0;
}
static int f_write(void *ctx, const char *path, const char *data, u32 len, bool crlf) {
    char parent[GUI_PATH_MAX], out[600];
    u32 n = 0, k;
    int p, i = find(path);
    parent_of(path, parent);
    p = find(parent);
    if(readonly_path(path)) return -EROFS;
    if(p < 0 || nodes[p].type != GUI_DIR) return -ENOENT;
    if(i >= 0 && nodes[i].type == GUI_DIR) return -EISDIR;
    for(k = 0; k < len && n < sizeof(out) - 1; k++) { if(crlf && data[k] == '\n') out[n++] = '\r'; out[n++] = data[k]; }
    if(n >= sizeof(out) - 1) return -ENOSPC;
    add(path, GUI_FILE, out, n);
    f_writes++;
    return 0;
}
static int f_mkdir(void *ctx, const char *path) {
    char parent[GUI_PATH_MAX];
    parent_of(path, parent);
    if(find(path) >= 0) return -EEXIST;
    if(readonly_path(path)) return -EROFS;
    if(find(parent) < 0) return -ENOENT;
    add(path, GUI_DIR, NULL, 0);
    return 0;
}
static bool has_children(const char *path) {
    char parent[GUI_PATH_MAX];
    int i;
    for(i = 0; i < (int)(sizeof(nodes) / sizeof(nodes[0])); i++) {
        if(!nodes[i].used || !strcmp(nodes[i].path, "/")) continue;
        parent_of(nodes[i].path, parent);
        if(!strcmp(parent, path)) return true;
    }
    return false;
}
static int f_remove(void *ctx, const char *path, bool dir) {
    int i = find(path);
    if(i < 0) return -ENOENT;
    if(readonly_path(path)) return -EROFS;
    if(dir != (nodes[i].type == GUI_DIR)) return dir ? -ENOTDIR : -EISDIR;
    if(dir && has_children(path)) return -ENOTEMPTY;
    nodes[i].used = false;
    return 0;
}
static int f_rename(void *ctx, const char *from, const char *to) {
    char moved[GUI_PATH_MAX];
    u32 len = strlen(from);
    int i;
    if(find(from) < 0) return -ENOENT;
    if(readonly_path(from)) return -EROFS;
    for(i = 0; i < (int)(sizeof(nodes) / sizeof(nodes[0])); i++) {
        if(!nodes[i].used || strncmp(nodes[i].path, from, len) || (nodes[i].path[len] && nodes[i].path[len] != '/')) continue;
        snprintf(moved, sizeof(moved), "%s%s", to, nodes[i].path + len);
        strlcpy(nodes[i].path, moved, sizeof(nodes[i].path));
    }
    return 0;
}
static int f_volumes(void *ctx, struct gui_volume *out, int max) {
    memset(out, 0, sizeof(*out));
    strlcpy(out->dev, "usb1p1", sizeof(out->dev));
    strlcpy(out->fs, "FAT32", sizeof(out->fs));
    strlcpy(out->size, "64M", sizeof(out->size));
    if(mounted) strlcpy(out->path, "/usb1p1", sizeof(out->path));
    return 1;
}
static int f_mount(void *ctx, const char *dev, char *msg, size_t n) {
    if(strcmp(dev, "usb1p1") || mounted) { strlcpy(msg, "mount: no", n); return -1; }
    mounted = true;
    strlcpy(msg, "usb1p1 on /usb1p1: fat32, label STICK, 64 MB, 63.9 MB free, read-write", n);
    return 0;
}
static int f_eject(void *ctx, const char *dev, char *msg, size_t n) {
    if(strcmp(dev, "usb1p1") || !mounted) { strlcpy(msg, "eject: no", n); return -1; }
    mounted = false;
    strlcpy(msg, "usb1: safe to remove", n);
    return 0;
}
static const char *f_error(int err) {
    return err == -ENOENT ? "No such file or directory" : err == -ENOTEMPTY ? "Directory not empty" :
           err == -EROFS ? "Read-only file system" : "Error";
}
static const struct gui_fs fake_fs = { NULL, f_list, f_stat, f_read, f_write, f_mkdir, f_remove, f_rename,
                                       f_volumes, f_mount, f_eject, f_error };
static const struct fake *node(const char *p) { int i = find(p); return i < 0 ? NULL : &nodes[i]; }
static bool holds(const char *p, const char *data) {
    const struct fake *f = node(p);
    return f && f->len == strlen(data) && !memcmp(f->data, data, f->len);
}
static void populate(void) {
    memset(nodes, 0, sizeof(nodes));
    mounted = false;
    add("/", GUI_DIR, NULL, 0);
    add("/dev", GUI_DIR, NULL, 0);
    add("/dev/console", GUI_FILE, NULL, 0);
    add("/initrd", GUI_DIR, NULL, 0);
    add("/initrd/readme.txt", GUI_FILE, "hello from the initrd\n", 22);
    add("/home", GUI_DIR, NULL, 0);
    add("/home/dos.txt", GUI_FILE, "a\r\nb\r\n", 6);
    add("/home/mixed.txt", GUI_FILE, "a\r\nb\n", 5);
    add("/home/bin.dat", GUI_FILE, "ab\0cd", 5);
    add("/home/big.txt", GUI_FILE, NULL, 0);
    nodes[find("/home/big.txt")].size = 300 * 1024;
    add("/home/sub", GUI_DIR, NULL, 0);
    add("/home/sub/deep.txt", GUI_FILE, "deep\n", 5);
    mounted = true;                                     /* the stick's files, seen only while it is mounted */
    add("/usb1p1", GUI_DIR, NULL, 0);
    add("/usb1p1/stick.txt", GUI_FILE, "on the stick\n", 13);
    mounted = false;
}

/**** driving the desktop ****/

static void key(int k) { gui_key(&g, k); }
static void type(const char *s) { for(; *s; s++) gui_key(&g, *s == '\n' ? '\r' : (u8)*s); }
static void click(int x,int y) {
    struct pointer_event e = {x,y,0,1};
    gui_pointer(&g,&e); e.buttons=0; gui_pointer(&g,&e);
    g.now += 1000;                                      /* well apart: no double-click */
}
static void double_click(int x, int y) {
    struct pointer_event e = {x,y,0,1};
    gui_pointer(&g,&e); e.buttons=0; gui_pointer(&g,&e);
    g.now += 100;
    e.buttons=1; gui_pointer(&g,&e); e.buttons=0; gui_pointer(&g,&e);
    g.now += 1000;
}
static void drag(int x0, int y0, int x1, int y1) {
    struct pointer_event e = {x0,y0,0,1};
    gui_pointer(&g,&e); e.x=x1; e.y=y1; gui_pointer(&g,&e); e.buttons=0; gui_pointer(&g,&e);
    g.now += 1000;
}
static bool text_is(const char *s) { return g.note.len == strlen(s) && !memcmp(g.note.text, s, g.note.len); }
static int row_named(const char *name) {
    int i;
    for(i = 0; i < g.files.count; i++) if(!strcmp(g.files.node[i].name, name)) return i;
    return -1;
}
static void surface(struct fb_surface *screen, int width, int height) {
    struct fb_mode mode;
    int i;
    memset(&mode,0,sizeof(mode));
    mode.addr=1; mode.width=width; mode.height=height; mode.pitch=width*4;
    mode.bpp=32; mode.type=1; mode.red_pos=16; mode.green_pos=8;
    mode.red_size=mode.green_size=mode.blue_size=8;
    for(i=0;i<(int)(sizeof(pixels)/sizeof(pixels[0]));i++) pixels[i]=0xa55a1234;
    fb_setup(screen,(u8*)(pixels+8),&mode);
}
static bool guards(int width, int height) {
    int i;
    for(i=0;i<8;i++) if(pixels[i]!=0xa55a1234) return false;
    for(i=8+width*height;i<(int)(sizeof(pixels)/sizeof(pixels[0]));i++) if(pixels[i]!=0xa55a1234) return false;
    return true;
}
static u32 at(int width, int x, int y) { return pixels[8 + y * width + x]; }

/* every window, then each menu and each kind of dialog, drawn inside the surface */
static void rendering(int width, int height) {
    struct fb_surface screen;
    int m, i;
    surface(&screen, width, height);
    populate();
    gui_init(&g,width,height); g.fs=&fake_fs; g.now=5000; strlcpy(g.clock,"14:05",sizeof(g.clock));
    gui_open(&g,GUI_SKETCH); gui_open(&g,GUI_CALC); gui_open(&g,GUI_SETTINGS); gui_open(&g,GUI_HELP);
    gui_open(&g,GUI_FILES); gui_open(&g,GUI_NOTEPAD);
    for(i = 0; i < 60; i++) type("A long line of text that runs off the right edge of the window \xC3\xA9\t\xE2\x89\x8B\xFF\n");
    gui_render(&g,&screen);
    CHECK(pixels[8]==0xF4F1E6); /* actual menubar color in rendered RGB buffer */
    gui_cursor(&screen,width-1,height-1,2); /* clipped at bottom/right edge */
    CHECK(guards(width,height));
    for(m = 1; m <= 6; m++) { gui_key(&g,7); while(g.menu != m) gui_key(&g,GUI_RIGHT); gui_render(&g,&screen); CHECK(guards(width,height)); gui_key(&g,27); }
    gui_act(&g,ACT_OPEN); gui_render(&g,&screen); CHECK(guards(width,height) && g.dialog.kind==GUI_DLG_OPEN); key(27);
    gui_act(&g,ACT_SAVE_AS); gui_render(&g,&screen); CHECK(guards(width,height) && g.dialog.kind==GUI_DLG_SAVE); key(27);
    gui_act(&g,ACT_GOTO); gui_render(&g,&screen); CHECK(guards(width,height) && g.dialog.kind==GUI_DLG_INPUT); key(27);
    key(17); gui_render(&g,&screen); CHECK(guards(width,height) && g.dialog.kind==GUI_DLG_CONFIRM); key(27);
    gui_act(&g,ACT_MAXIMIZE); gui_act(&g,ACT_FIND); gui_render(&g,&screen); CHECK(guards(width,height));
    for(i = 0; i < GUI_WINDOWS; i++) { g.win[i].x = width - 10; g.win[i].y = height - 10; }   /* hanging off the edge */
    gui_render(&g,&screen); CHECK(guards(width,height));
}

static void notepad(void) {
    struct fb_surface screen;
    struct note_layout l;
    struct gui_window *w;
    const u8 *glyph;
    int row, col, x0, y0;
    populate();
    gui_init(&g,960,540); g.fs=&fake_fs; g.now=10000;
    key(5); CHECK(g.focus==GUI_NOTEPAD && g.win[GUI_NOTEPAD].open);
    type("Hello\nWorld"); CHECK(text_is("Hello\nWorld") && g.note.modified);
    key(GUI_HOME); key(GUI_UP); CHECK(g.note.cursor==0);
    key(GUI_END); CHECK(g.note.cursor==5);
    key(GUI_DOWN); CHECK(g.note.cursor==11);
    key(8); CHECK(text_is("Hello\nWorl"));
    key(26); CHECK(text_is("Hello\nWorld"));
    key(26); CHECK(text_is("Hello\n"));
    key(25); CHECK(text_is("Hello\nWorld"));
    key(1); CHECK(g.note.anchor==0 && g.note.cursor==11);
    key(3); CHECK(g.clip_len==11 && !g.quit && !g.dialog.kind);   /* Ctrl+C copies in Notepad */
    key(GUI_END); key(22); CHECK(text_is("Hello\nWorldHello\nWorld"));
    key(26); CHECK(text_is("Hello\nWorld"));
    type("\xC3\xA9"); CHECK(text_is("Hello\nWorld\xC3\xA9"));
    key(GUI_LEFT); CHECK(g.note.cursor==11);              /* one character, two bytes */
    key(GUI_DELETE); CHECK(text_is("Hello\nWorld"));
    type("\xC3\xA9"); key(8); CHECK(text_is("Hello\nWorld"));
    key(6); CHECK(g.note.finding);
    type("HELLO"); key('\r'); CHECK(g.note.anchor==0 && g.note.cursor==5 && g.note.finding);
    key(27); CHECK(!g.note.finding && g.note.anchor==0 && g.note.cursor==5);
    key(12); CHECK(g.dialog.kind==GUI_DLG_INPUT);
    type("2\n"); CHECK(!g.dialog.kind && g.note.cursor==6);
    key(9); CHECK(g.note.text[6]=='\t' && g.focus==GUI_NOTEPAD); key(8);
    /* the mouse: a drag selects, a double-click takes a word, the line numbers a line */
    w=&g.win[GUI_NOTEPAD]; note_layout(&g,&l);
    drag(w->x+l.tx+8+1,w->y+l.ty+2,w->x+l.tx+4*8+1,w->y+l.ty+2);
    CHECK(g.note.anchor==1 && g.note.cursor==4);
    double_click(w->x+l.tx+2*8+2,w->y+l.ty+18);
    CHECK(g.note.anchor==6 && g.note.cursor==11);
    click(w->x+10,w->y+l.ty+2); CHECK(g.note.anchor==0 && g.note.cursor==6);
    key(GUI_END); key(GUI_DOWN); key(GUI_END);
    /* what is drawn: the first character in the first cell */
    surface(&screen,960,540); gui_render(&g,&screen);
    x0=w->x+l.tx; y0=w->y+l.ty; glyph=font8x16['H'];
    for(row=0;row<16;row++) for(col=0;col<8;col++)
        if(at(960,x0+col,y0+row)!=((glyph[row]&(0x80>>col))?0x153D49:0xFFFDF5)) { CHECK(!"the H of Hello"); row=16; break; }
    /* Save as: a name typed in a folder chosen */
    key(19); CHECK(g.dialog.kind==GUI_DLG_SAVE && !strcmp(g.dialog.dir,"/"));
    key(GUI_DOWN); key('\r'); CHECK(!strcmp(g.dialog.dir,"/home"));
    type("notes.txt"); key('\r');
    CHECK(!g.dialog.kind && holds("/home/notes.txt","Hello\nWorld") && !g.note.modified && !strcmp(g.note.path,"/home/notes.txt"));
    key(12); type("1\n"); type("!"); key(19); CHECK(holds("/home/notes.txt","!Hello\nWorld") && !g.note.modified);
    key(26); CHECK(g.note.modified); key(25); CHECK(!g.note.modified);   /* back where it was saved */
    /* Open: CRLF files are edited with LF and saved with CRLF; mixed ones keep their bytes */
    key(15); CHECK(g.dialog.kind==GUI_DLG_OPEN && !strcmp(g.dialog.dir,"/home"));
    key(GUI_DOWN); key(GUI_DOWN); key(GUI_DOWN); key(GUI_DOWN); CHECK(!strcmp(g.dialog.list[g.dialog.selected-1].name,"dos.txt"));
    key('\r'); CHECK(!g.dialog.kind && g.note.crlf && text_is("a\nb\n") && !strcmp(g.note.path,"/home/dos.txt"));
    type("c"); key(19); CHECK(holds("/home/dos.txt","ca\r\nb\r\n"));
    CHECK(note_load(&g,"/home/mixed.txt")==0 && !g.note.crlf && text_is("a\r\nb\n"));
    CHECK(note_load(&g,"/home/big.txt")==-EFBIG && g.dialog.kind==GUI_DLG_MESSAGE && text_is("a\r\nb\n")); key(27);
    CHECK(note_load(&g,"/home/sub")==-EISDIR && text_is("a\r\nb\n")); key(27);
    CHECK(note_load(&g,"/dev/console")==-EINVAL && text_is("a\r\nb\n")); key(27);
    CHECK(note_load(&g,"/home/bin.dat")==-EINVAL && g.dialog.kind==GUI_DLG_MESSAGE && g.note.len==0); key(27);
    CHECK(!g.dialog.kind);
    /* a read-only volume: Save offers Save as */
    CHECK(note_load(&g,"/initrd/readme.txt")==0 && g.note.readonly);
    type("x"); key(19); CHECK(g.dialog.kind==GUI_DLG_CONFIRM && g.dialog.act==ACT_DLG_READONLY);
    key('\r'); CHECK(g.dialog.kind==GUI_DLG_SAVE && !strcmp(g.dialog.dir,"/initrd"));
    key(GUI_DOWN); key(GUI_UP); key('\r'); CHECK(!strcmp(g.dialog.dir,"/"));
    key(GUI_DOWN); key('\r'); CHECK(!strcmp(g.dialog.dir,"/home"));
    key(1); type("copy.txt"); key('\r');
    CHECK(holds("/home/copy.txt","xhello from the initrd\n") && !g.note.readonly && !strcmp(g.note.path,"/home/copy.txt"));
    /* Save as over another file asks first */
    gui_act(&g,ACT_SAVE_AS); field_set(&g.dialog.field,"dos.txt",false); key('\r');
    CHECK(g.dialog.kind==GUI_DLG_CONFIRM && g.dialog.act==ACT_DLG_REPLACE);
    key(27); CHECK(!g.dialog.kind && holds("/home/dos.txt","ca\r\nb\r\n"));
    /* unsaved changes: Cancel stays, Don't save leaves */
    type("y"); key(17); CHECK(g.dialog.kind==GUI_DLG_CONFIRM && g.dialog.act==ACT_DLG_GUARD && !g.quit);
    key(27); CHECK(!g.quit && !g.dialog.kind && g.pending==PEND_NONE);
    key(17); key(GUI_RIGHT); key('\r'); CHECK(g.quit);
    g.quit=false;
    /* Save, then what waited: another file opens */
    key(15); key(GUI_END); CHECK(!strcmp(g.dialog.list[g.dialog.selected-1].name,"notes.txt"));
    key('\r'); CHECK(g.dialog.kind==GUI_DLG_CONFIRM && g.dialog.act==ACT_DLG_GUARD);
    key('\r'); CHECK(!g.dialog.kind && holds("/home/copy.txt","xyhello from the initrd\n") && text_is("!Hello\nWorld"));
    /* an untitled note: its guard's Save is a Save as */
    key(14); CHECK(g.note.len==0 && !g.note.path[0]);
    type("draft"); key(14); CHECK(g.dialog.kind==GUI_DLG_CONFIRM);
    key('\r'); CHECK(g.dialog.kind==GUI_DLG_SAVE && !strcmp(g.dialog.dir,"/home"));
    type("draft.txt"); key('\r'); CHECK(holds("/home/draft.txt","draft") && g.note.len==0 && !g.note.modified);
    /* closing Notepad asks too, and takes the document with it */
    type("z"); gui_act(&g,ACT_CLOSE); CHECK(g.dialog.kind==GUI_DLG_CONFIRM && g.win[GUI_NOTEPAD].open);
    key(GUI_RIGHT); key('\r'); CHECK(!g.win[GUI_NOTEPAD].open && g.note.len==0);
    /* the limits: the size of a note, and a change too big for undo */
    key(5); memset(g.clip,'q',GUI_CLIP_MAX); g.clip_len=GUI_CLIP_MAX;
    key(22); CHECK(g.note.len==GUI_CLIP_MAX && g.note.nundo==0);
    key(26); CHECK(g.note.len==GUI_CLIP_MAX && !strcmp(g.note.status,"Nothing to undo"));
    key(22); CHECK(g.note.len==2*GUI_CLIP_MAX && g.note.len==GUI_NOTE_MAX);
    key(22); CHECK(g.note.len==GUI_NOTE_MAX && !strcmp(g.note.status,"Notepad holds 256 KB at most"));
    key(1); key(8); CHECK(g.note.len==0);
    for(row=0;row<GUI_UNDO_STEPS+20;row++) { type("w\n"); }
    CHECK(g.note.nundo<=GUI_UNDO_STEPS);
    for(row=0;row<GUI_UNDO_STEPS+20;row++) key(26);
    CHECK(g.note.len>0 && g.note.modified);
}

static void files(void) {
    int i, home;
    populate();
    gui_init(&g,960,540); g.fs=&fake_fs; g.now=10000;
    key(2); CHECK(g.focus==GUI_FILES && g.files.count==5);
    CHECK(g.files.node[1].type==GUI_NODE_DRIVE && !strcmp(g.files.node[1].name,"usb1p1"));
    CHECK(!strcmp(g.files.node[2].name,"dev") && !strcmp(g.files.node[3].name,"home") && !strcmp(g.files.node[4].name,"initrd"));
    key(GUI_DOWN); key(GUI_DOWN); key(GUI_DOWN); CHECK(g.files.selected==3);
    key(GUI_RIGHT); CHECK(g.files.node[3].expanded && g.files.count==10);
    CHECK(!strcmp(g.files.node[4].name,"sub") && !strcmp(g.files.node[5].name,"big.txt"));
    key(GUI_RIGHT); CHECK(g.files.selected==4);
    i=row_named("dos.txt"); g.files.selected=i; key('\r');
    CHECK(g.focus==GUI_NOTEPAD && !strcmp(g.note.path,"/home/dos.txt") && text_is("a\nb\n"));
    key(2); CHECK(g.focus==GUI_FILES);
    /* the mouse: a double-click on a folder opens it, the triangle closes it */
    i=row_named("sub");
    double_click(g.win[GUI_FILES].x+200,g.win[GUI_FILES].y+TITLE_H+BAR_H+3+20+(i-g.files.top)*ROW_H+4);
    CHECK(g.files.node[i].expanded && row_named("deep.txt")==i+1);
    click(g.win[GUI_FILES].x+8+2*16+2,g.win[GUI_FILES].y+TITLE_H+BAR_H+3+20+(i-g.files.top)*ROW_H+4);
    CHECK(!g.files.node[i].expanded && row_named("deep.txt")<0);
    /* a stick: Mount shows its files, Eject puts the drive back */
    key(GUI_HOME); key(GUI_DOWN); CHECK(g.files.node[g.files.selected].type==GUI_NODE_DRIVE);
    CHECK(files_can(&g,ACT_FILES_MOUNT) && !files_can(&g,ACT_FILES_EJECT));
    key('\r'); CHECK(mounted && !strcmp(g.files.node[g.files.selected].name,"usb1p1") && g.files.node[g.files.selected].type==GUI_DIR);
    CHECK(row_named("stick.txt")>0 && g.files.node[row_named("home")].expanded);
    CHECK(!strncmp(g.files.status,"usb1p1 on /usb1p1",17));
    g.files.selected=row_named("stick.txt"); CHECK(files_can(&g,ACT_FILES_EJECT) && files_can(&g,ACT_FILES_RENAME));
    gui_act(&g,ACT_FILES_EJECT); CHECK(!mounted && !strcmp(g.files.status,"usb1: safe to remove"));
    CHECK(g.files.node[1].type==GUI_NODE_DRIVE && row_named("stick.txt")<0);
    /* Notepad's unsaved file keeps a stick in */
    g.files.selected=1; gui_act(&g,ACT_FILES_MOUNT); CHECK(mounted);
    note_load(&g,"/usb1p1/stick.txt"); key(5); type("more "); key(2);
    g.files.selected=row_named("usb1p1"); gui_act(&g,ACT_FILES_EJECT);
    CHECK(mounted && g.dialog.kind==GUI_DLG_MESSAGE); key(27);
    key(5); key(19); CHECK(holds("/usb1p1/stick.txt","more on the stick\n"));
    key(2); gui_act(&g,ACT_FILES_EJECT); CHECK(!mounted);
    /* New folder, New file, Rename, Delete */
    home=row_named("home"); g.files.selected=home;
    gui_act(&g,ACT_FILES_NEW_FOLDER); CHECK(g.dialog.kind==GUI_DLG_INPUT && !strcmp(g.dialog.target,"/home"));
    type("docs\n"); CHECK(node("/home/docs") && node("/home/docs")->type==GUI_DIR);
    files_path(&g,g.files.selected,g.dialog.target,sizeof(g.dialog.target)); CHECK(!strcmp(g.dialog.target,"/home/docs"));
    gui_act(&g,ACT_FILES_NEW_FILE); key('\r'); CHECK(holds("/home/docs/untitled.txt",""));
    CHECK(!strcmp(g.files.node[g.files.selected].name,"untitled.txt"));
    gui_act(&g,ACT_FILES_RENAME); type("todo.txt\n"); CHECK(holds("/home/docs/todo.txt","") && !node("/home/docs/untitled.txt"));
    gui_act(&g,ACT_FILES_NEW_FILE); type("todo.txt\n"); CHECK(g.dialog.kind==GUI_DLG_MESSAGE); key(27);
    gui_act(&g,ACT_FILES_NEW_FOLDER); type("a/b\n"); CHECK(g.dialog.kind==GUI_DLG_MESSAGE && !node("/home/docs/a")); key(27);
    g.files.selected=row_named("docs"); gui_act(&g,ACT_FILES_DELETE);
    CHECK(g.dialog.kind==GUI_DLG_CONFIRM && g.dialog.choice==2);
    key('\r'); CHECK(!g.dialog.kind && node("/home/docs"));             /* Enter alone: Cancel */
    gui_act(&g,ACT_FILES_DELETE); key(GUI_LEFT); key('\r');
    CHECK(g.dialog.kind==GUI_DLG_MESSAGE && node("/home/docs")); key(27);   /* not empty */
    g.files.selected=row_named("todo.txt"); gui_act(&g,ACT_FILES_DELETE); key(GUI_LEFT); key('\r');
    CHECK(!node("/home/docs/todo.txt") && !strcmp(g.files.status,"Deleted todo.txt"));
    /* renaming Notepad's file, or a folder it is in, moves Notepad's path along */
    CHECK(note_load(&g,"/home/sub/deep.txt")==0);
    key(2); g.files.selected=row_named("sub"); gui_act(&g,ACT_FILES_RENAME); type("deeper\n");
    CHECK(node("/home/deeper/deep.txt") && !strcmp(g.note.path,"/home/deeper/deep.txt"));
    /* nothing to write in the initrd, /dev or the root */
    g.files.selected=row_named("initrd"); key(GUI_RIGHT); g.files.selected=row_named("readme.txt");
    CHECK(!files_can(&g,ACT_FILES_NEW_FILE) && !files_can(&g,ACT_FILES_RENAME) && !files_can(&g,ACT_FILES_DELETE) && files_can(&g,ACT_FILES_OPEN));
    g.files.selected=0; CHECK(!files_can(&g,ACT_FILES_NEW_FOLDER) && !files_can(&g,ACT_FILES_OPEN));
    /* a refresh keeps the open folders and the selection */
    g.files.selected=row_named("readme.txt"); gui_act(&g,ACT_FILES_REFRESH);
    CHECK(!strcmp(g.files.node[g.files.selected].name,"readme.txt") && g.files.node[row_named("home")].expanded);
    /* Show in Files: the folders down to Notepad's file open */
    gui_close(&g,GUI_FILES); CHECK(!g.win[GUI_FILES].open);
    note_load(&g,"/home/deeper/deep.txt"); key(5); gui_act(&g,ACT_REVEAL);
    CHECK(g.focus==GUI_FILES && !strcmp(g.files.node[g.files.selected].name,"deep.txt"));
}

static void windows(void) {
    struct gui_window *w;
    int x, y, ww, wh;
    populate();
    gui_init(&g,960,540); g.fs=&fake_fs; g.now=10000;
    key(5); w=&g.win[GUI_NOTEPAD]; ww=w->w; wh=w->h;
    double_click(w->x+120,w->y+10); CHECK(w->max && w->x==0 && w->y==MENU_H && w->w==960 && w->h==540-MENU_H-TASK_H);
    double_click(w->x+120,w->y+10); CHECK(!w->max && w->w==ww && w->h==wh);
    x=w->x; y=w->y;
    drag(x+w->w-4,y+w->h-4,x+w->w-104,y+w->h-54); CHECK(w->w==ww-100 && w->h==wh-50 && w->x==x);
    drag(x+w->w-4,y+w->h-4,x+20,y+20); CHECK(w->w==360 && w->h==200);
    click(x+w->w-74+10,y+10); CHECK(w->min && g.focus!=GUI_NOTEPAD);
    key(5); CHECK(!w->min && g.focus==GUI_NOTEPAD);
    click(x+w->w-50+10,y+10); CHECK(w->max);
    gui_act(&g,ACT_MAXIMIZE); CHECK(!w->max);
    /* the taskbar: one click brings a window up, another tucks it away */
    gui_open(&g,GUI_CALC); CHECK(g.focus==GUI_CALC);
    click(6+10,540-10); CHECK(g.win[GUI_CALC].min && g.focus!=GUI_CALC);   /* the first button is the Calculator's */
    click(6+10,540-10); CHECK(!g.win[GUI_CALC].min && g.focus==GUI_CALC);
    /* the menus: by the mouse and by the keys */
    click(160,14); CHECK(g.menu==3);
    click(800,300); CHECK(!g.menu);
    key(7); CHECK(g.menu==1); key(GUI_RIGHT); key(GUI_RIGHT); key(GUI_RIGHT); CHECK(g.menu==4);
    key(GUI_DOWN); key('\r'); CHECK(!g.menu && g.focus==GUI_FILES);
    key(7); key(GUI_LEFT); CHECK(g.menu==6); key('\r'); CHECK(g.focus==GUI_HELP);
    key(7); key(GUI_RIGHT); key(GUI_RIGHT); CHECK(g.menu==3);
    key('\r'); CHECK(!g.menu && g.note.undo_at==0);      /* Undo is off: Enter on a disabled item does nothing */
    /* Tab: the next window, but a tab in Notepad */
    gui_open(&g,GUI_CALC); key(9); CHECK(g.focus!=GUI_CALC);
    CHECK(gui_wants_arrows(&g)==(g.focus==GUI_NOTEPAD || g.focus==GUI_FILES || g.focus==GUI_HELP));
    gui_open(&g,GUI_CALC); CHECK(!gui_wants_arrows(&g));
    key(7); CHECK(gui_wants_arrows(&g)); key(27);
    /* Ctrl+C elsewhere leaves, as before */
    key(3); CHECK(g.quit);
}

int main(void) {
    const char *p;
    char b[32];
    struct pointer_event e;
    gui_init(&g,640,400);
    CHECK(g.focus == GUI_ABOUT && g.win[GUI_ABOUT].open);
    gui_key(&g,11); CHECK(g.win[GUI_CALC].open && g.focus == GUI_CALC);
    for(p="12+30="; *p; p++) gui_key(&g,*p);
    CHECK(g.calc.value==42 && !g.calc.error);
    for(p="c7/0="; *p; p++) gui_key(&g,*p);
    CHECK(g.calc.error);
    for(p="c999999999*9="; *p; p++) gui_key(&g,*p);
    CHECK(g.calc.error);
    for(p="c8-12="; *p; p++) gui_key(&g,*p);
    CHECK(g.calc.value == -4);
    gui_key(&g,4); CHECK(g.focus == GUI_SKETCH);
    click(g.win[GUI_SKETCH].x+22,g.win[GUI_SKETCH].y+74);
    CHECK(g.canvas[4*GUI_CANVAS_W+6] == g.brush);
    gui_key(&g,14); CHECK(g.canvas[4*GUI_CANVAS_W+6] == 0);
    /* Drag title well past bounds; still reachable below the menu. */
    e.x=g.win[GUI_SKETCH].x+100; e.y=g.win[GUI_SKETCH].y+10; e.buttons=1; e.wheel=0;
    gui_pointer(&g,&e); e.x=-2000; e.y=-1000; gui_pointer(&g,&e);
    CHECK(g.win[GUI_SKETCH].x==0 && g.win[GUI_SKETCH].y==34);
    e.buttons=0; gui_pointer(&g,&e); CHECK(g.drag==-1);
    gui_key(&g,16); CHECK(g.focus == GUI_SETTINGS);
    click(20,14); CHECK(g.menu==1);
    gui_key(&g,GUI_DOWN); gui_key(&g,27); CHECK(!g.menu && !g.quit);
    gui_key(&g,17); CHECK(g.quit);
    /* Notepad still edits without a file system; Files and Open need one */
    gui_init(&g,640,400); key(5); type("no files"); CHECK(text_is("no files"));
    key(2); CHECK(!g.win[GUI_FILES].open); key(15); CHECK(!g.dialog.kind);
    gui_date(0,b,sizeof(b)); CHECK(!strcmp(b,"1970-01-01 00:00"));
    gui_date(1792245780,b,sizeof(b)); CHECK(!strcmp(b,"2026-10-17 14:03"));
    gui_date(951868740,b,sizeof(b)); CHECK(!strcmp(b,"2000-02-29 23:59"));
    gui_date(4291660800u,b,sizeof(b)); CHECK(!strcmp(b,"2105-12-31 00:00"));
    gui_bytes(1,b,sizeof(b)); CHECK(!strcmp(b,"1 byte"));
    gui_bytes(1023,b,sizeof(b)); CHECK(!strcmp(b,"1023 bytes"));
    gui_bytes(1536,b,sizeof(b)); CHECK(!strcmp(b,"1.5 KB"));
    gui_bytes(262144,b,sizeof(b)); CHECK(!strcmp(b,"256 KB"));
    gui_bytes(5*1024*1024,b,sizeof(b)); CHECK(!strcmp(b,"5.0 MB"));
    strlcpy(b,"/a/b",sizeof(b)); gui_parent(b); CHECK(!strcmp(b,"/a")); gui_parent(b); CHECK(!strcmp(b,"/")); gui_parent(b); CHECK(!strcmp(b,"/"));
    gui_join(b,sizeof(b),"/","x"); CHECK(!strcmp(b,"/x")); gui_join(b,sizeof(b),b,"y"); CHECK(!strcmp(b,"/x/y"));
    notepad();
    files();
    windows();
    rendering(640,400); rendering(960,540); rendering(1280,800);
    printf("gui: %d checks, %d failures\n",checks,failed);
    return !!failed;
}
