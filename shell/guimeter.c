/*
SurfOS Desktop: Meters for System Monitor and Disks
--------------------
File: guimeter.c Date: 10/17/26
--------------------
What System Monitor and Disks draw and move alike (shell/gui_sys.h): a size in kilobytes for
people, the share of a total (with 32-bit arithmetic only: the kernel has no 64-bit division),
a bar of what is used, and a list's selection moved by the keys. No state of its own. Track
SYS of docs/superpowers/plans/2026-10-17-gui-suite.md.
*/
#include "gui_sys.h"
#include <string.h>
#include <stdio.h>

/* as gui_bytes(): one decimal below ten of a unit */
void sys_kb(u32 kb, char *out, size_t n) {
    static const char *const units[] = { "MB", "GB", "TB" };
    u32 tenths, i;
    if(kb < 1024) { snprintf(out, n, "%u KB", kb); return; }
    for(i = 0; i < 2 && kb >= 1024 * 1024; i++) kb /= 1024;  /* the unit below the one shown, under 1024 * 1024 */
    tenths = kb * 10 / 1024;
    if(tenths < 100) snprintf(out, n, "%u.%u %s", tenths / 10, tenths % 10, units[i]);
    else snprintf(out, n, "%u %s", tenths / 10, units[i]);
}

u32 sys_part(u32 part, u32 total, u32 scale) {
    if(!total) return 0;
    if(part > total) part = total;
    while(total > 65535) { total >>= 1; part >>= 1; }   /* part * scale stays in 32 bits for any scale of a screen */
    return part * scale / total;
}

/* 10/18/26: coral from exactly 90% (used * 10 >= total * 9, in 64 bits: a product needs no division) */
void sys_bar(const struct gui_draw *d, int x, int y, int w, int h, u32 used, u32 total) {
    int fill = w > 2 ? (int)sys_part(used, total, (u32)(w - 2)) : 0;
    gd_rect(d, x, y, w, h, LINE);
    gd_rect(d, x + 1, y + 1, w - 2, h - 2, SYS_SHEET);
    if(fill > 0) gd_rect(d, x + 1, y + 1, fill, h - 2, (u64)used * 10 >= (u64)total * 9 ? CORAL : TEAL);
}

int sys_move(int selected, int count, int rows, int key) {
    int page = rows > 2 ? rows - 1 : 1;
    switch(key) {
    case GUI_UP: selected = selected < 0 ? 0 : selected - 1; break;
    case GUI_DOWN: selected = selected < 0 ? 0 : selected + 1; break;
    case GUI_PGUP: selected -= page; break;
    case GUI_PGDN: selected = selected < 0 ? page : selected + page; break;
    case GUI_HOME: selected = 0; break;
    case GUI_END: selected = count - 1; break;
    default: return -2;
    }
    return count ? gui_clamp(selected, 0, count - 1) : -1;
}

int sys_move_from(int selected, int hint, int count, int rows, int key) {
    if(selected >= 0 || hint < 0 || !count) return sys_move(selected, count, rows, key);
    hint = gui_clamp(hint, 0, count);                   /* between rows hint - 1 and hint */
    if(key == GUI_DOWN) return gui_clamp(hint, 0, count - 1);
    if(key == GUI_UP) return gui_clamp(hint - 1, 0, count - 1);
    return sys_move(gui_clamp(hint, 0, count - 1), count, rows, key);
}

int sys_view(int selected, int top, int count, int rows) {
    if(selected >= 0 && selected < top) top = selected;
    if(selected >= top + rows) top = selected - rows + 1;
    return gui_clamp(top, 0, count > rows ? count - rows : 0);
}
