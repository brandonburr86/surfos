/* hello: the first user program. Prints its arguments and exits with 42. */
#include <surf.h>

int main(int argc, char **argv) {
    int i;
    printf("Hello from user mode, pid %d\n", getpid());
    for(i = 0; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 42;
}
