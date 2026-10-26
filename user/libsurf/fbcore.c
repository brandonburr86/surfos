/*
SurfOS user library: the framebuffer renderer
--------------------
File: fbcore.c  Date: 10/25/26
--------------------
The kernel's software renderer (driver/fbcore.c, include/sys/framebuffer.h), built into libsurf
for the drawing kit of programs' windows (guidraw.c, guikit.c; libgui, gui.h): it draws into a
window's image in the program's own memory, never into the framebuffer.
*/

#include "../../driver/fbcore.c"
