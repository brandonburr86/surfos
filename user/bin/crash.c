/* crash: misbehave in the named way; the kernel must survive and only this process die. */
#include <surf.h>

static void bad_pointer(void) { *(volatile int *)0 = 1; }
static void kernel_write(void) { *(volatile int *)0x100000 = 1; }       /* the kernel's text */
static void cli(void) { asm volatile("cli"); }
static void hlt(void) { asm volatile("hlt"); }
static void io(void) { asm volatile("inb $0x60, %%al" ::: "eax"); }
static void yield_gate(void) { asm volatile("int $0x40"); }
static void bad_syscall(void) { printf("write(1, 0x100000, 16) = %d\n", write(1, (void *)0x100000, 16)); }
static int recurse(int n) { volatile char pad[512]; pad[0] = (char)n; return n ? recurse(n - 1) + pad[0] : 0; }
static void divide(void) { volatile int z = 0; printf("%d\n", 1 / z); }

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "help";
    if(!strcmp(what, "null")) bad_pointer();
    else if(!strcmp(what, "kernel")) kernel_write();
    else if(!strcmp(what, "cli")) cli();
    else if(!strcmp(what, "hlt")) hlt();
    else if(!strcmp(what, "io")) io();
    else if(!strcmp(what, "int40")) yield_gate();
    else if(!strcmp(what, "efault")) { bad_syscall(); return 0; }
    else if(!strcmp(what, "stack")) { printf("deep recursion survived (stack grew), sum %d\n", recurse(2000)); return 0; }
    else if(!strcmp(what, "div")) divide();
    else if(!strcmp(what, "loop")) for(;;);
    else { printf("usage: crash null|kernel|cli|hlt|io|int40|efault|stack|div|loop\n"); return 1; }
    printf("crash: still alive after %s?!\n", what);
    return 2;
}
