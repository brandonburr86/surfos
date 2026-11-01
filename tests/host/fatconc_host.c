/* tests/host/test_fatconc.c's host side (10/16/26, round fs-fix-2: the review of B5-B11's conc_host.c, as it was).
   review harness (conc): the host side, built with the host's own headers: threads, the "interrupt"
   lock, the table lock and condition SurfOS mutexes are built on, wait queues as one generation
   counter, a watchdog that reports a wait that never ends. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <string.h>

static pthread_mutex_t irq_m;
static pthread_mutex_t tbl_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tbl_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gen_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gen_cv = PTHREAD_COND_INITIALIZER;
static unsigned long gen;
static __thread unsigned long my_gen;
static __thread int my_waiting;
static __thread const char *my_name = "main";
static __thread const char *my_where = "";
volatile unsigned long host_progress;
static volatile int stop_watch;

void host_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&irq_m, &a);
}
unsigned long conc_irq_save(void) { pthread_mutex_lock(&irq_m); return 0x200; }
void conc_irq_restore(unsigned long f) { (void)f; pthread_mutex_unlock(&irq_m); }

void host_tbl_lock(void) { pthread_mutex_lock(&tbl_m); }
void host_tbl_unlock(void) { pthread_mutex_unlock(&tbl_m); }
void host_tbl_wait(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_cond_timedwait(&tbl_cv, &tbl_m, &ts);
}
void host_tbl_broadcast(void) { pthread_cond_broadcast(&tbl_cv); }

void host_wait_prepare(void) {
    pthread_mutex_lock(&gen_m);
    my_gen = gen;
    my_waiting = 1;
    pthread_mutex_unlock(&gen_m);
}
void host_wake_all(void) {
    pthread_mutex_lock(&gen_m);
    gen++;
    pthread_cond_broadcast(&gen_cv);
    pthread_mutex_unlock(&gen_m);
}
void host_yield(void) {
    if(!my_waiting) { sched_yield(); return; }
    pthread_mutex_lock(&gen_m);
    while(gen == my_gen) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&gen_cv, &gen_m, &ts);
    }
    my_waiting = 0;
    pthread_mutex_unlock(&gen_m);
}
void host_sleep_us(unsigned us) { if(us) usleep(us); }
void host_set_name(const char *n) { my_name = n; }
void host_where(const char *w) { my_where = w; }
const char *host_name(void) { return my_name; }

struct tinfo { void (*fn)(void *); void *arg; const char *name; };
static void *trampoline(void *p) {
    struct tinfo t = *(struct tinfo *)p;
    free(p);
    my_name = t.name;
    t.fn(t.arg);
    return NULL;
}
void *host_thread(void (*fn)(void *), void *arg, const char *name) {
    pthread_t *th = malloc(sizeof(*th));
    struct tinfo *t = malloc(sizeof(*t));
    t->fn = fn; t->arg = arg; t->name = name;
    pthread_create(th, NULL, trampoline, t);
    return th;
}
void host_join(void *th) { pthread_join(*(pthread_t *)th, NULL); free(th); }

/* the watchdog: no operation finished for limit seconds: a wait that never ends */
static void (*watch_dump)(void);
static void *watch(void *p) {
    unsigned long last = host_progress;
    int idle = 0, limit = *(int *)p;
    while(!stop_watch) {
        sleep(1);
        if(host_progress != last) { last = host_progress; idle = 0; continue; }
        if(++idle >= limit) {
            printf("WATCHDOG: no operation finished for %d s: a wait that never ends\n", limit);
            if(watch_dump) watch_dump();
            fflush(stdout);
            _exit(3);
        }
    }
    return NULL;
}
static pthread_t watch_th;
static int watch_limit;
void host_watchdog(int seconds, void (*dump)(void)) {
    watch_limit = seconds;
    watch_dump = dump;
    pthread_create(&watch_th, NULL, watch, &watch_limit);
}
void host_watchdog_stop(void) { stop_watch = 1; pthread_join(watch_th, NULL); }
unsigned long host_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static pthread_mutex_t out_m = PTHREAD_MUTEX_INITIALIZER;
void host_out_lock(void) { pthread_mutex_lock(&out_m); }
void host_out_unlock(void) { pthread_mutex_unlock(&out_m); }
void *host_mutex_new(void) { pthread_mutex_t *m = malloc(sizeof(*m)); pthread_mutex_init(m, NULL); return m; }
void host_mutex_lock(void *m) { pthread_mutex_lock((pthread_mutex_t *)m); }
void host_mutex_unlock(void *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }
