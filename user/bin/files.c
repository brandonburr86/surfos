/*
 * SurfOS Files -- a text file browser (2026).
 * Uses only libsurf: directory snapshots, ANSI output, raw input and spawnfd.
 * The left tree is the current ancestry; the right pane is the current folder.
 * Mouse clicks use SGR terminal reports (serial/Telnet); no native mouse driver.
 */
#include <surf.h>

#define MAX_ENTRIES 8192
#define MAX_COLS 512
#define MAX_ROWS 256
#define EDIT_LIMIT (1024 * 1024)
#define CTRL(c) ((c) & 31)

enum { K_NONE = 256, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END,
       K_PGUP, K_PGDN, K_MOUSE, K_EOF };

static struct dirent *entries;
static int count, selected, top, show_hidden = 1;
static char current[PATH_MAX], message[256];
static struct statfs filesystem;
static int rows, cols, page_rows, tree_width, raw;
static char ancestors[PATH_MAX / 2][PATH_MAX];
static int ancestor_count, tree_indices[MAX_ROWS];
static int mouse_button, mouse_x, mouse_y, mouse_release;
static char output[(MAX_COLS + 64) * MAX_ROWS];
static size_t output_len;

static void append(const char *s) {
    size_t len = strlen(s);
    if(len <= sizeof(output) - output_len) {
        memcpy(output + output_len, s, len);
        output_len += len;
    }
}

static int write_all(const char *buf, size_t len) {
    while(len) {
        ssize_t n = write(1, buf, len);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* All file system text passes through here before reaching the terminal. */
static void cells(char *out, const char *s, int width) {
    int i;
    for(i = 0; i < width && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        out[i] = c >= 32 && c < 127 ? (char)c : '?';
    }
    if(i == width && s[i] && width) out[width - 1] = '~';
    while(i < width) out[i++] = ' ';
    out[width] = 0;
}

static void draw_row(int y, const char *left, const char *right, int inverse) {
    char seq[32], line[MAX_COLS + 1];
    snprintf(seq, sizeof(seq), "\x1b[%d;1H\x1b[0m", y + 1);
    append(seq);
    if(left && tree_width) {
        cells(line, left, tree_width);
        append(line);
        append(" | ");
    }
    if(inverse) append("\x1b[7m");
    cells(line, right, cols - 1 - (left && tree_width ? tree_width + 3 : 0));
    append(line);
    append("\x1b[0m\x1b[K");
}

static void restore_terminal(void) {
    const char *s = "\x1b[?1000l\x1b[?1006l\x1b[0m\x1b[2J\x1b[H\x1b[?25h";
    if(!raw) return;
    write_all(s, strlen(s));
    ttyctl(TTY_RAW, 0);
    raw = 0;
}

static int enter_terminal(void) {
    const char *s = "\x1b[2J\x1b[H\x1b[?25l\x1b[?1000h\x1b[?1006h";
    if(ttyctl(TTY_RAW, 1) < 0) return -1;
    raw = 1;
    return write_all(s, strlen(s));
}

static int dimensions(void) {
    int size = ttyctl(TTY_GETSIZE, 0), old_rows = rows, old_cols = cols;
    cols = size > 0 ? (size >> 16) & 0xffff : 80;
    rows = size > 0 ? size & 0xffff : 25;
    if(cols > MAX_COLS) cols = MAX_COLS;
    if(rows > MAX_ROWS) rows = MAX_ROWS;
    if(cols < 20 || rows < 8) return -1;
    page_rows = rows - 7;
    tree_width = cols >= 72 ? 18 : 0;
    if(raw && (old_rows != rows || old_cols != cols)) append("\x1b[2J");
    return 0;
}

static const char *base(const char *s) {
    const char *p = strrchr(s, '/');
    return p ? p + 1 : s;
}

static void parent_path(char *s) {
    char *p = strrchr(s, '/');
    if(!p || p == s) { s[0] = '/'; s[1] = 0; }
    else *p = 0;
}

/* Normalize without silently truncating input or changing the process cwd. */
static int absolute(const char *path, char out[PATH_MAX]) {
    char tmp[PATH_MAX * 2], cwd[PATH_MAX], *p, *save;
    int n;
    if(path[0] == '/') n = snprintf(tmp, sizeof(tmp), "%s", path);
    else {
        if(!getcwd(cwd, sizeof(cwd))) return -1;
        n = snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    }
    if(n < 0 || n >= (int)sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    strcpy(out, "/");
    for(p = strtok_r(tmp, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        size_t used = strlen(out), len = strlen(p);
        if(!strcmp(p, ".")) continue;
        if(!strcmp(p, "..")) { parent_path(out); continue; }
        if(used + (used > 1) + len >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
        if(used > 1) out[used++] = '/';
        memcpy(out + used, p, len + 1);
    }
    return 0;
}

static int child_path(char path[PATH_MAX], const char *name) {
    int n = snprintf(path, PATH_MAX, "%s%s%s", current, strcmp(current, "/") ? "/" : "", name);
    if(n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int by_name(const void *a, const void *b) {
    const struct dirent *x = a, *y = b;
    int xd = S_ISDIR(x->type), yd = S_ISDIR(y->type), cmp;
    if(xd != yd) return yd - xd;
    cmp = strcasecmp(x->name, y->name);
    return cmp ? cmp : strcmp(x->name, y->name);
}

/* Replace the view only after a complete successful read. */
static int load_directory(const char *path, const char *keep) {
    struct dirent *list = NULL, de;
    int fd, n = 0, cap = 0, result, err, i;
    fd = open(path, O_RDONLY | O_DIRECTORY);
    if(fd < 0) goto failed;
    while((result = getdent(fd, &de)) == 0) {
        if(!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue;
        if(!show_hidden && de.name[0] == '.') continue;
        if(n == MAX_ENTRIES) { errno = EFBIG; goto read_failed; }
        if(n == cap) {
            struct dirent *next;
            cap = cap ? cap * 2 : 32;
            next = realloc(list, (size_t)cap * sizeof(*list));
            if(!next) { errno = ENOMEM; goto read_failed; }
            list = next;
        }
        list[n++] = de;
    }
    if(result < 0) goto read_failed;
    close(fd);
    if(n > 1) qsort(list, (size_t)n, sizeof(*list), by_name);
    selected = 0;
    for(i = 0; keep && i < n; i++) if(!strcmp(list[i].name, keep)) selected = i;
    free(entries);
    entries = list;
    count = n;
    top = 0;
    /* path may be current itself on refresh. */
    if(path != current) strcpy(current, path);
    memset(&filesystem, 0, sizeof(filesystem));
    statfs(current, &filesystem);
    message[0] = 0;
    return 0;
read_failed:
    err = errno;
    close(fd);
    free(list);
    errno = err;
failed:
    snprintf(message, sizeof(message), "Cannot open folder: %s", strerror(errno));
    return -1;
}

static void refresh_directory(void) {
    char keep[NAME_MAX + 1];
    keep[0] = 0;
    if(count) strcpy(keep, entries[selected].name);
    load_directory(current, keep);
}

static void up(void) {
    char path[PATH_MAX], keep[NAME_MAX + 1];
    strcpy(path, current);
    strcpy(keep, base(current));
    parent_path(path);
    load_directory(path, keep);
}

static void make_tree(void) {
    char path[PATH_MAX], reverse[PATH_MAX / 2][PATH_MAX];
    int n = 0, i;
    strcpy(path, current);
    do {
        strcpy(reverse[n++], path);
        if(!strcmp(path, "/")) break;
        parent_path(path);
    } while(n < PATH_MAX / 2);
    ancestor_count = n;
    for(i = 0; i < n; i++) strcpy(ancestors[i], reverse[n - i - 1]);
}

static void date_string(unsigned int stamp, char text[20]) {
    struct tm tm;
    time_t t = (time_t)stamp;
    if(!stamp) { strcpy(text, "-"); return; }
    strftime(text, 20, "%Y-%m-%d %H:%M", localtime_r(&t, &tm));
}

static int render(void) {
    char line[MAX_COLS + 1], label[MAX_COLS + 1], name[MAX_COLS + 1], date[20], tree[MAX_COLS + 1];
    int i, width, name_width, detailed;
    output_len = 0;
    if(dimensions() < 0) return -1;
    if(selected < 0) selected = 0;
    if(count && selected >= count) selected = count - 1;
    if(selected < top) top = selected;
    if(selected >= top + page_rows) top = selected - page_rows + 1;
    make_tree();
    width = cols - 1 - (tree_width ? tree_width + 3 : 0);
    detailed = width >= 48;
    name_width = width - (detailed ? 28 : 11);
    draw_row(0, NULL, " SurfOS Files", 1);
    snprintf(line, sizeof(line), "Path: %s", current);
    if((int)strlen(line) >= cols) snprintf(line, sizeof(line), "Path: ...%s", current + strlen(current) - (cols - 11));
    draw_row(1, NULL, line, 0);
    cells(name, "Name", name_width);
    snprintf(line, sizeof(line), detailed ? "%s %10s %s" : "%s %10s", name, "Bytes", "Modified");
    draw_row(2, tree_width ? "Folders" : NULL, line, 1);
    for(i = 0; i < page_rows; i++) {
        int index = top + i, ti = i;
        tree[0] = 0;
        tree_indices[i] = -1;
        if(ancestor_count > page_rows && i) ti = ancestor_count - page_rows + i;
        if(ti < ancestor_count) {
            int indent = ti < 6 ? ti : 6;
            snprintf(tree, sizeof(tree), "%*s%s%s", indent, "", ti ? "+- " : "", ti ? base(ancestors[ti]) : "/");
            tree_indices[i] = ti;
        }
        line[0] = 0;
        if(index < count) {
            struct dirent *e = &entries[index];
            snprintf(label, sizeof(label), "%s%s", e->name, S_ISDIR(e->type) ? "/" : "");
            cells(name, label, name_width);
            date_string(e->mtime, date);
            if(S_ISDIR(e->type)) snprintf(line, sizeof(line), detailed ? "%s %10s %s" : "%s %10s", name, "<DIR>", date);
            else snprintf(line, sizeof(line), detailed ? "%s %10u %s" : "%s %10u", name, e->size, date);
        } else if(!count && i == 0) strcpy(line, "(empty folder)");
        draw_row(i + 3, tree_width ? tree : NULL, line, count && index == selected);
    }
    if(count) snprintf(line, sizeof(line), "Selected: %s%s", entries[selected].name, S_ISDIR(entries[selected].type) ? "/" : "");
    else strcpy(line, "Selected: none");
    draw_row(rows - 4, NULL, line, 0);
    if(count && !S_ISDIR(entries[selected].type))
        snprintf(line, sizeof(line), "%u bytes | %s%s | %d/%d", entries[selected].size,
                 filesystem.fs, filesystem.flags & ST_RDONLY ? " (read-only)" : "", selected + 1, count);
    else snprintf(line, sizeof(line), "%d entries | %s%s | Hidden: %s", count, filesystem.fs,
                  filesystem.flags & ST_RDONLY ? " (read-only)" : "", show_hidden ? "shown" : "hidden");
    draw_row(rows - 3, NULL, line, 0);
    draw_row(rows - 2, NULL, message[0] ? message : "Arrows select; click selected item to open", 0);
    draw_row(rows - 1, NULL, cols >= 60 ? "Enter Open/Edit  Left Up  r Refresh  . Hidden  q Quit" : "Open/Edit  Up  Refresh  Hidden  Quit", 1);
    return write_all(output, output_len);
}

/* Read one byte: no user-space typeahead can leak into the editor. */
static int byte(int timeout) {
    unsigned char c;
    struct pollfd p = { 0, POLLIN, 0 };
    int r;
    do { r = poll(&p, 1, timeout); } while(r < 0 && errno == EINTR);
    if(!r) return K_NONE;
    if(r < 0) return K_EOF;
    do { r = (int)read(0, &c, 1); } while(r < 0 && errno == EINTR);
    return r == 1 ? c : K_EOF;
}

static int key(void) {
    char seq[64];
    int c = byte(250), n = 0, a = 0, b = 0, d = 0;
    if(c != 27) return c;
    c = byte(100);
    if(c != '[' && c != 'O') return K_NONE;
    do {
        c = byte(100);
        if(c >= K_NONE) return c;
        seq[n++] = (char)c;
        if(c >= 0x40 && c <= 0x7e) break;
    } while(n < (int)sizeof(seq) - 1);
    seq[n] = 0;
    if(n == (int)sizeof(seq) - 1) return K_NONE;
    if(seq[0] == '<') {
        int i = 1, values[3] = {0, 0, 0}, field = 0, digits = 0;
        for(; i < n - 1; i++) {
            if(seq[i] >= '0' && seq[i] <= '9') {
                if(values[field] > 10000) return K_NONE;
                values[field] = values[field] * 10 + seq[i] - '0';
                digits++;
            } else if(seq[i] == ';' && field < 2 && digits) { field++; digits = 0; }
            else return K_NONE;
        }
        if(field != 2 || !digits || (c != 'M' && c != 'm')) return K_NONE;
        mouse_button = values[0]; mouse_x = values[1]; mouse_y = values[2]; mouse_release = c == 'm';
        return K_MOUSE;
    }
    /* Arrows may carry modifiers; only CSI digits/semicolons before the final. */
    for(a = 0; a < n - 1; a++) {
        if(seq[a] >= '0' && seq[a] <= '9') {
            if(b > 10000) return K_NONE;
            b = b * 10 + seq[a] - '0';
        } else if(seq[a] == ';') { if(!d) d = b; b = 0; }
        else return K_NONE;
    }
    switch(c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '~':
        if(!d) d = b;
        switch(d) {
        case 1: case 7: return K_HOME;
        case 4: case 8: return K_END;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
        }
    }
    return K_NONE;
}

static void open_selected(int directories_only) {
    char path[PATH_MAX], keep[NAME_MAX + 1], buf[4096];
    struct stat st;
    int fd, i, pid, status = 0, r;
    ssize_t n;
    char *args[] = { "edit", path, NULL };
    int fds[] = {0, 1, 2};
    if(!count) return;
    if(child_path(path, entries[selected].name) < 0 || stat(path, &st) < 0) goto failed;
    if(S_ISDIR(st.type)) { load_directory(path, NULL); return; }
    if(directories_only) return;
    if(!S_ISREG(st.type) || !strcmp(st.fs, "devfs")) {
        strcpy(message, "Cannot edit a device or special file"); return;
    }
    if(st.size > EDIT_LIMIT) { strcpy(message, "File exceeds edit's 1 MiB limit"); return; }
    fd = open(path, O_RDONLY);
    if(fd < 0) goto failed;
    n = read(fd, buf, sizeof(buf));
    r = errno;
    close(fd);
    if(n < 0) { errno = r; goto failed; }
    for(i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if((c < 32 && c != '\t' && c != '\r' && c != '\n' && c != '\f') || c == 127) {
            strcpy(message, "Binary file: use hexdump to inspect it"); return;
        }
    }
    strcpy(keep, entries[selected].name);
    restore_terminal();
    pid = spawnfd("/initrd/bin/edit", args, fds);
    r = errno;
    if(pid >= 0) {
        do { i = wait(pid, &status); } while(i < 0 && errno == EINTR);
        if(i < 0) { r = errno; status = -1; }
    }
    if(enter_terminal() < 0) { perror("files: terminal"); exit(1); }
    if(load_directory(current, keep) < 0) return;
    if(pid < 0) snprintf(message, sizeof(message), "Cannot launch edit: %s", strerror(r));
    else if(status) snprintf(message, sizeof(message), "Editor exited with status %d", status);
    else strcpy(message, "Editor closed; folder refreshed");
    return;
failed:
    snprintf(message, sizeof(message), "Cannot open item: %s", strerror(errno));
}

static void mouse(void) {
    int row = mouse_y - 4, index;
    if(mouse_release || mouse_x < 1 || mouse_x >= cols || mouse_y < 1 || mouse_y > rows) return;
    if(mouse_button == 64) { selected -= 3; return; }
    if(mouse_button == 65) { selected += 3; return; }
    if(mouse_button != 0) return;
    if(mouse_y == rows) {
        if(cols >= 60) {
            if(mouse_x <= 15) open_selected(0);
            else if(mouse_x >= 18 && mouse_x <= 24) up();
            else if(mouse_x >= 27 && mouse_x <= 35) refresh_directory();
            else if(mouse_x >= 38 && mouse_x <= 45) { show_hidden = !show_hidden; refresh_directory(); }
            else if(mouse_x >= 48 && mouse_x <= 53) { restore_terminal(); exit(0); }
        } else {
            if(mouse_x <= 9) open_selected(0);
            else if(mouse_x >= 12 && mouse_x <= 13) up();
            else if(mouse_x >= 16 && mouse_x <= 22) refresh_directory();
            else if(mouse_x >= 25 && mouse_x <= 30) { show_hidden = !show_hidden; refresh_directory(); }
            else if(mouse_x >= 33 && mouse_x <= 36) { restore_terminal(); exit(0); }
        }
        return;
    }
    if(row < 0 || row >= page_rows) return;
    if(tree_width && mouse_x <= tree_width) {
        index = tree_indices[row];
        if(index >= 0) load_directory(ancestors[index], NULL);
    } else if(!tree_width || mouse_x > tree_width + 3) {
        index = top + row;
        if(index < count) {
            if(selected == index) open_selected(0);
            else selected = index;
        }
    }
}

int main(int argc, char **argv) {
    char path[PATH_MAX];
    int k, changed = 1;
    if(argc > 2 || (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))) {
        puts("usage: files [directory]\nArrows: select; Enter/e: open or edit; Left/Backspace: parent\nHome/End/PgUp/PgDn: navigate; /: root; r: refresh; .: hidden; q: quit\nMouse: select then click to open, or click Open/Edit (SGR terminals).");
        return argc > 2 ? 2 : 0;
    }
    if(!isatty(0) || !isatty(1)) { fputs("files: an interactive terminal is required\n", stderr); return 1; }
    if(absolute(argc == 2 ? argv[1] : ".", path) < 0 || load_directory(path, NULL) < 0) {
        fprintf(stderr, "files: %s\n", message[0] ? message : strerror(errno));
        return 1;
    }
    if(dimensions() < 0) { fputs("files: terminal must be at least 20 columns by 8 rows\n", stderr); free(entries); return 1; }
    atexit(restore_terminal);
    if(enter_terminal() < 0) { perror("files: terminal"); free(entries); return 1; }
    for(;;) {
        int size = ttyctl(TTY_GETSIZE, 0);
        if(size > 0 && (((size >> 16) & 0xffff) != cols || (size & 0xffff) != rows)) changed = 1;
        if(changed && render() < 0) break;
        k = key();
        if(k == K_NONE) { changed = 0; continue; }
        if(k == K_MOUSE && mouse_release) { changed = 0; continue; }
        if(k == 'q' || k == CTRL('q') || k == CTRL('c') || k == K_EOF) break;
        changed = 1;
        message[0] = 0;
        switch(k) {
        case K_UP: case 'k': selected--; break;
        case K_DOWN: case 'j': selected++; break;
        case K_HOME: selected = 0; break;
        case K_END: selected = count - 1; break;
        case K_PGUP: selected -= page_rows; break;
        case K_PGDN: selected += page_rows; break;
        case K_LEFT: case 8: case 127: up(); break;
        case K_RIGHT: open_selected(1); break;
        case '\r': case '\n': case 'e': open_selected(0); break;
        case '/': load_directory("/", NULL); break;
        case 'r': case CTRL('l'): refresh_directory(); break;
        case '.': show_hidden = !show_hidden; refresh_directory(); break;
        case K_MOUSE: mouse(); break;
        }
    }
    restore_terminal();
    free(entries);
    return 0;
}
