/* count: print numbers with a delay; shows preemption and sleep from user mode */
#include <surf.h>

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 5, ms = argc > 2 ? atoi(argv[2]) : 200, i;
    u32 t0 = uptime();
    for(i = 1; i <= n; i++) {
        printf("%d ", i);
        sleep(ms);
    }
    printf("\ncounted to %d in %u ms (pid %d)\n", n, uptime() - t0, getpid());
    return 0;
}
