/*
SurfOS riscv32 Self Test
--------------------
File: rvtest.c  Date: 10/15/26 (SurfCPU), after shell/selftest.c
--------------------
`selftest` and `heaptest` for the machine-mode port. The portable checks of the x86 self
test (tasks and their exit codes, sleeps, kernel timers, mutex, semaphore, event, the
formatter, the heap) run as they do there; the x86 ones that need PC hardware, user
programs or the network are left out. Added for riscv32, the brief's port checks
(surfcpu/docs/AGENT-INSTRUCTIONS.md section 10):

  * every register survives traps: arch/riscv32/rvregs.S fills them all with a pattern
    while timer interrupts arrive and tasks switch, or while it yields with ECALL, and
    two tasks do it at once with different patterns;
  * the stack is 16-byte aligned in tasks and in interrupt context (the psABI's rule);
  * sp is inside its task's stack, gp is the kernel's, tp is zero;
  * structure sizes and the ABI: ilp32 types, the boot ABI's layouts, signed char;
  * 64-bit arithmetic through libgcc and the formatter (the riscv32 divide path);
  * the CRC-32 test vectors of BOOT-ABI.md;
  * timer rollover of the 32-bit tick arithmetic, and mtime against the tick count;
    (the timing windows allow for an emulator stalled by a busy host: the ticks that
    come late are counted at once, arch/riscv32/timer.c)
  * a FAT round trip on the first mounted FAT volume, when there is one.

It prints "SELFTEST PASS" or "SELFTEST FAIL", as on x86, for the test harness.
*/

#include <blibc_common.h>
#include <surfos/types.h>
#include <surfos/task.h>
#include <surfos/wait.h>
#include <surfos/sync.h>
#include <surfos/ktimer.h>
#include <surfos/timer.h>
#include <surfos/klog.h>
#include <surfos/interrupt.h>
#include <surfos/irq.h>
#include <surfos/trap.h>
#include <sys/bdev.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <surfcpu/bootabi.h>
#include <riscv/csr.h>
#include <riscv/board.h>
#include <riscv/arch.h>

static int fails;

static void check(bool ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok) fails++;
}

/* one at a time, as shell/selftest.c */
static u_long busy_pid;

static bool claim(const char *what) {
    u_long flags = irq_save(), pid = busy_pid;
    surf_task *t = pid ? task_find(pid) : NULL;
    if(!pid || !t || t->state == TS_DEAD) {
        busy_pid = curTask->pid;
        irq_restore(flags);
        return true;
    }
    irq_restore(flags);
    printf("    %s: pid %lu is running a test: one at a time\n", what, pid);
    return false;
}

static void unclaim(void) {
    busy_pid = 0;
}

/**** heap (as shell/selftest.c's heap_round) ****/

static int heap_round(void) {
    enum { SLOTS = 192, ROUNDS = 20000 };
    struct { void *ptrs[SLOTS]; u_int sizes[SLOTS]; } *s = kcalloc(1, sizeof(*s));
    struct heap_stats before, after;
    u_int i, r, allocs = 0, frees = 0, maxlive = 0, live = 0, bad = 0;

    if(!s) {
        printf("heaptest: no memory for its tables: HEAPTEST FAIL\n");
        return 1;
    }
    kheap_stats(&before);
    srand(getticks() + 7);
    for(r = 0; r < ROUNDS && !task_interrupted(); r++) {
        i = rand() % SLOTS;
        if(s->ptrs[i]) {
            u_char *p = s->ptrs[i];
            u_int k;
            for(k = 0; k < s->sizes[i]; k++) if(p[k] != (u_char)(i + k)) { bad++; break; }
            kfree(s->ptrs[i]);
            s->ptrs[i] = NULL;
            frees++; live--;
        } else {
            u_int sz = (rand() % 16 == 0) ? 1 + rand() % 20000 : 1 + rand() % 512;
            u_char *p = kalloc(sz);
            u_int k;
            if(!p) { bad++; continue; }
            for(k = 0; k < sz; k++) p[k] = (u_char)(i + k);
            s->ptrs[i] = p; s->sizes[i] = sz;
            allocs++; live++;
            if(live > maxlive) maxlive = live;
        }
    }
    for(i = 0; i < SLOTS; i++) if(s->ptrs[i]) { kfree(s->ptrs[i]); s->ptrs[i] = NULL; frees++; }
    kheap_stats(&after);
    if(!kheap_check()) bad++;
    if(after.bytes_used != before.bytes_used || after.blocks_used != before.blocks_used) bad++;
    kfree(s);
    printf("heaptest: %u allocs, %u frees, %u live at peak, used %lu -> %lu bytes, %lu free blocks: %s\n",
           allocs, frees, maxlive, before.bytes_used, after.bytes_used, after.blocks_free, bad ? "HEAPTEST FAIL" : "HEAPTEST PASS");
    return bad ? 1 : 0;
}

int rv_heaptest(void) {
    int r;
    if(!claim("heaptest")) return 1;
    r = heap_round();
    unclaim();
    return r;
}

/**** registers across traps ****/

static u_long reg_expect(u_long seed, int n) {
    if(n == 10) return seed ^ 0x2a;
    if(n == 31) return 0;
    return (u_long)n * 0x01010101UL ^ seed;
}

/* the registers that differ from the pattern (x0 and sp are not compared) */
static int reg_diff(u_long seed, const u_long *out, char *first, size_t size) {
    int n, bad = 0;
    for(n = 1; n < 32; n++) {
        if(n == 2) continue;
        if(out[n] != reg_expect(seed, n)) {
            if(!bad) snprintf(first, size, "x%d = 0x%08lx, not 0x%08lx", n, out[n], reg_expect(seed, n));
            bad++;
        }
    }
    return bad;
}

struct regjob {
    u_long seed, spins, ticks;
    volatile u_long switches;           /* times this task was switched away from and back to */
    int bad, ecall, rounds;
    struct regjob *other;
    char first[64];
    u_long out[32];
};

/* Rounds of the pattern (short ones, so how fast the CPU is does not matter) until both tasks
   of the pair have been switched out and back at least twice: a register of one left in the
   other would show. 10/16/26: the limit is time, 300 ticks, not 400 rounds. 400 rounds of
   task A were sometimes less than three quanta in QEMU on an idle host, and A then ended
   before it had been switched out twice. The round limit only stops a stopped clock. */
static void reg_task(void *arg) {
    struct regjob *j = (struct regjob *)arg;
    u_long t0 = timer_irq_count, s0 = curTask->switches;
    while(!j->bad && (j->switches < 2 || j->other->switches < 2) && timer_irq_count - t0 < 300 && j->rounds < 100000) {
        if(j->ecall) rv_regs_ecall(j->seed + j->rounds, j->spins, j->out);
        else rv_regs_spin(j->seed + j->rounds, j->spins, j->out);
        j->bad = reg_diff(j->seed + j->rounds, j->out, j->first, sizeof(j->first));
        j->rounds++;
        j->switches = curTask->switches - s0;
    }
    j->ticks = timer_irq_count - t0;
}

static void test_registers(void) {
    struct regjob *a = kcalloc(2, sizeof(struct regjob)), *b;
    surf_task *ta, *tb;
    u_long spins = 200000, t0;
    int status, tries;
    char line[160];
    if(!a) { check(false, "register test memory"); return; }
    b = a + 1;
    /* spinning in this task until at least 3 timer interrupts came in the middle */
    for(tries = 0; tries < 6; tries++) {
        a->seed = 0xA5A50000UL + tries;
        a->spins = spins;
        t0 = timer_irq_count;
        rv_regs_spin(a->seed, spins, a->out);
        a->ticks = timer_irq_count - t0;
        a->bad = reg_diff(a->seed, a->out, a->first, sizeof(a->first));
        if(a->bad || a->ticks >= 3) break;
        spins *= 4;
    }
    snprintf(line, sizeof(line), "every register kept across %lu timer interrupts (%lu rounds)%s%s", a->ticks, a->spins,
             a->bad ? ": " : "", a->bad ? a->first : "");
    check(!a->bad && a->ticks >= 3, line);

    /* yield by ECALL on every round: the trap path that switches tasks on purpose */
    a->seed = 0x5A5A1234UL;
    rv_regs_ecall(a->seed, 300, a->out);
    a->bad = reg_diff(a->seed, a->out, a->first, sizeof(a->first));
    snprintf(line, sizeof(line), "every register kept across 300 ECALLs (yield)%s%s", a->bad ? ": " : "", a->bad ? a->first : "");
    check(!a->bad, line);

    /* two tasks at once, different patterns, switched by the tick and by yield */
    memset(a, 0, 2 * sizeof(struct regjob));
    a->seed = 0x13579BDFUL; a->spins = 100000; a->ecall = 0; a->other = b;
    b->seed = 0x2468ACE0UL; b->spins = 4; b->ecall = 1; b->other = a;  /* 4 yields a round: B's count shows soon */
    ta = kthread_create("regs A", reg_task, a, PL_NORMAL, 0);
    tb = kthread_create("regs B", reg_task, b, PL_NORMAL, 0);
    if(ta) task_wait(ta->pid, &status);
    if(tb) task_wait(tb->pid, &status);
    snprintf(line, sizeof(line), "two tasks, spinning and yielding, keep their own registers (%d and %d rounds, %lu and %lu "
             "switches)%s%s", a->rounds, b->rounds, a->switches, b->switches, (a->bad || b->bad) ? ": " : "",
             a->bad ? a->first : b->first);
    check(ta && tb && !a->bad && !b->bad && a->switches >= 2 && b->switches >= 2, line);
    kfree(a);
}

/**** stack, gp, tp ****/

static volatile u_long irq_sp, irq_gp;
static volatile int irq_seen;
static void sample_sp(void *arg) {
    (void)arg;
    irq_sp = rv_read_sp();
    irq_gp = rv_read_gp();
    irq_seen = 1;
}

static u_long __attribute__((noinline)) deeper_sp(int n) {
    volatile char pad[24];
    pad[0] = (char)n;
    return n ? deeper_sp(n - 1) + pad[0] * 0 : rv_read_sp();
}

static void test_stack(void) {
    struct ktimer t;
    u_long sp = rv_read_sp(), gp = rv_read_gp();
    extern char __global_pointer$[];
    check(!(sp & 15) && !(deeper_sp(3) & 15), "the task's stack is 16-byte aligned, in nested calls too");
    check(sp > curTask->stack_top - KSTACK_SIZE && sp <= curTask->stack_top, "sp is inside the task's own stack");
    check(gp == (u_long)__global_pointer$ && rv_read_tp() == 0, "gp is the kernel's global pointer, tp is zero");
    irq_seen = 0;
    ktimer_init(&t, sample_sp, NULL);
    ktimer_start(&t, 10, false);
    sleep_ms(40);
    ktimer_cancel(&t);
    check(irq_seen && !(irq_sp & 15) && irq_gp == (u_long)__global_pointer$,
          "in the timer interrupt: the stack 16-byte aligned, gp the kernel's");
    check(*(u32 *)(curTask->stack_top - KSTACK_SIZE) == STACK_CANARY, "the stack's canary is intact");
}

/**** ABI ****/

static void test_abi(void) {
    struct { char c; long long ll; } s;
    char minus = (char)0xFF;
    check(sizeof(int) == 4 && sizeof(long) == 4 && sizeof(void *) == 4 && sizeof(long long) == 8 &&
          sizeof(size_t) == 4, "ilp32: int, long and pointers are 32 bits, long long 64");
    check((u_long)&s.ll - (u_long)&s == 8, "long long is 8-byte aligned in a struct (RV32, not i386's 4)");
    check(minus < 0, "char is signed (-fsigned-char, as the shared code expects)");
    check(sizeof(struct surf_boot_info) == 256 && sizeof(struct surf_img_header) == 64 &&
          offsetof(struct surf_boot_info, cmdline) == 0x80, "the boot ABI's structures: 256 and 64 bytes");
    check(sizeof(struct trapframe) == TF_SIZE && (TF_SIZE % 16) == 0 && offsetof(struct trapframe, mepc) == TF_MEPC,
          "the trap frame matches traps.S and keeps the stack aligned");
}

/**** arithmetic and the formatter ****/

static void test_math(void) {
    volatile u64 a = 0x123456789ABCDEF0ULL, b = 1000000007ULL;
    volatile i64 n = -9000000000LL;
    volatile u32 x = 0xFFFFFFFFu, y = 0xFFFFFFFEu;
    char buf[64];
    check(a / b == 0x123456789ABCDEF0ULL / 1000000007ULL && a % b == 0x123456789ABCDEF0ULL % 1000000007ULL &&
          n / 7 == -1285714285LL && n % 7 == -5, "64-bit divide and remainder (libgcc)");
    check((u64)x * y == 0xFFFFFFFD00000002ULL, "32x32 to 64-bit multiply (mulhu)");
    snprintf(buf, sizeof buf, "%llu %lld %llx", (unsigned long long)a, (long long)n, (unsigned long long)a);
    check(!strcmp(buf, "1311768467463790320 -9000000000 123456789abcdef0"), "the formatter's 64-bit numbers");
    snprintf(buf, sizeof buf, "%5d|%-5d|%05d|%x|%s", 42, 42, 42, 0xbeef, "ok");
    check(!strcmp(buf, "   42|42   |00042|beef|ok"), "snprintf widths and flags");
    check(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strtol("-0x1F", NULL, 0) == -31 && atoi("123") == 123,
          "strcmp, strtol, atoi");
}

/* 10/16/26: arch/riscv32/memory.c moves words where it can: every alignment of both ends, every
   length up to 70, both directions of an overlap, against a byte at a time; nothing outside touched */
static void test_memory(void) {
    static u8 a[112], b[112], ref[112];
    int so, d, n, i, bad = 0;
    for(so = 0; so < 4 && !bad; so++) {
        for(d = -6; d <= 6 && !bad; d++) {
            for(n = 0; n <= 70 && !bad; n++) {
                int from = 16 + so, to = from + d;
                for(i = 0; i < 112; i++) a[i] = ref[i] = (u8)(i * 7 + n);
                memmove(a + to, a + from, (size_t)n);
                if(to < from) for(i = 0; i < n; i++) ref[to + i] = ref[from + i];
                else for(i = n - 1; i >= 0; i--) ref[to + i] = ref[from + i];
                if(memcmp(a, ref, 112)) bad = 1;
                for(i = 0; i < 112; i++) b[i] = ref[i] = 0xEE;
                memcpy(b + to, a, (size_t)n);
                for(i = 0; i < n; i++) ref[to + i] = a[i];
                if(memcmp(b, ref, 112)) bad = 2;
                memset(b + to, 0xA5, (size_t)n);
                for(i = 0; i < n; i++) ref[to + i] = 0xA5;
                if(memcmp(b, ref, 112)) bad = 3;
            }
        }
    }
    check(!bad, "memcpy, memmove (both overlaps) and memset at every alignment and length up to 70 (word moves)");
}

static void test_crc(void) {
    static const u8 seq[] = "123456789";
    u8 all[256];
    int i;
    for(i = 0; i < 256; i++) all[i] = (u8)i;
    check(crc32("", 0) == 0x00000000 && crc32("a", 1) == 0xE8B7BE43 && crc32(seq, 9) == 0xCBF43926 &&
          crc32(all, 256) == 0x29058C73, "CRC-32 test vectors of BOOT-ABI.md section 3");
}

/**** time ****/

/* 10/16/26: the tick count and mtime read together, mtime on both sides of the count, and read again
   when anything came between them (an interrupt, another task, or a busy host holding QEMU up, after
   which the count lags mtime until the late ticks are counted) */
static void read_clocks(u_long *ticks, u64 *mtime) {
    u64 before;
    int i;
    for(i = 0; i < 8; i++) {
        before = board_mtime();
        *ticks = getticks();
        *mtime = board_mtime();
        if(*mtime - before < timer_period() / 4) return;
    }
}

static void test_time(void) {
    u_long t0, t1, dt = 0, per = timer_period();
    u64 m0, m1, dm = 0;
    u_long a = 0xFFFFFFF0UL, b = a + 0x20;      /* the tick counter wrapping, as getticks() - t0 does */
    bool ok = false;
    int tries;
    /* 10/16/26: up to three tries. A host that holds QEMU up right after the sleep spoils one try; a
       fault in the tick count spoils every try. Twice on a loaded Mac, mtime had moved more than
       16 periods after a count of 10-15 ticks: the count was read before a stall, mtime after it. */
    for(tries = 1; tries <= 3 && !ok; tries++) {
        read_clocks(&t0, &m0);
        sleep_ms(100);
        read_clocks(&t1, &m1);
        dt = t1 - t0;
        dm = m1 - m0;
        ok = dt >= 10 && dt <= 15 && dm >= (u64)per * 9 && dm <= (u64)per * 16;
    }
    check(dt >= 10 && dt <= 15, "sleep_ms(100) takes 10-15 ticks (late ticks on a busy host count at once)");
    check(dm >= (u64)per * 9 && dm <= (u64)per * 16, "mtime moved as much as the ticks did");
    if(tries > 2 || !ok)                        /* what the last try saw, when the first did not pass */
        printf("       %lu ticks, mtime %lu.%02lu periods, %d tries\n", dt, (u_long)(dm / per),
               (u_long)(dm % per * 100 / per), tries - 1);
    check(b - a == 0x20, "tick differences survive the 32-bit wrap");
}

static volatile u_int timer_fires;
static void count_fire(void *arg) { (void)arg; timer_fires++; }

static void test_timers(void) {
    struct ktimer t;
    u_int after_cancel;
    timer_fires = 0;
    ktimer_init(&t, count_fire, NULL);
    ktimer_start(&t, 10, true);
    sleep_ms(205);
    ktimer_cancel(&t);
    check(timer_fires >= 15 && timer_fires <= 22, "periodic 10 ms timer fires about 20 times in 205 ms (fewer when ticks come late)");
    after_cancel = timer_fires;
    sleep_ms(50);
    check(timer_fires == after_cancel, "cancelled timer stays quiet");
}

/**** tasks (as shell/selftest.c) ****/

static void exit_with_arg(void *arg) {
    task_exit((int)(u_long)arg);
}

struct sleeper {
    u_int wakes;
    u_long slept_ticks;
};

static void sleeper_task(void *arg) {
    struct sleeper *s = (struct sleeper *)arg;
    u_int i;
    for(i = 0; i < 5; i++) {
        u_long t0 = getticks();
        sleep_ms(30);
        s->slept_ticks += getticks() - t0;
        s->wakes++;
    }
}

static void test_tasks(void) {
    u_int before = task_count(), i, n, collected = 0;
    u_long pids[32];
    int status = 0;
    surf_task *t, *s1, *s2;
    struct sleeper sa = { 0, 0 }, sb = { 0, 0 };

    for(n = 0; n < 32; n++) {
        t = kthread_create("churn", exit_with_arg, (void *)(u_long)n, PL_NORMAL, 0);
        if(!t) break;
        pids[n] = t->pid;
    }
    check(n == 32, "32 kthreads created");
    for(i = 0; i < n; i++) {
        if(task_wait(pids[i], &status) < 0) break;
        collected++;
    }
    check(collected == 32 && task_count() == before, "all 32 collected by task_wait, task count back");
    t = kthread_create("exit42", exit_with_arg, (void *)42, PL_HIGH, 0);
    if(t) {
        u_long pid = t->pid;
        check(task_wait(pid, &status) == (int)pid && status == 42, "exit code 42 reaches task_wait");
    } else check(false, "exit code 42 reaches task_wait");

    s1 = kthread_create("sleeper 1", sleeper_task, &sa, PL_NORMAL, 0);
    s2 = kthread_create("sleeper 2", sleeper_task, &sb, PL_LOW, 0);
    if(s1) task_wait(s1->pid, &status);
    if(s2) task_wait(s2->pid, &status);
    check(s1 && s2 && sa.wakes == 5 && sb.wakes == 5 && sa.slept_ticks >= 15 && sb.slept_ticks >= 15,
          "two kernel tasks sleep and wake 5 times each");
    check(heartbeat_count > 0, "the heartbeat task keeps waking");
}

static mutex_t test_mutex = MUTEX_INIT("selftest");
static volatile u_long shared_counter;

static void mutex_worker(void *arg) {
    u_int i;
    (void)arg;
    for(i = 0; i < 2000; i++) {
        u_long v;
        mutex_lock(&test_mutex);
        v = shared_counter;
        if((i % 7) == 0) yield();
        shared_counter = v + 1;
        mutex_unlock(&test_mutex);
    }
}

static void test_mutex_fn(void) {
    surf_task *a, *b;
    int status;
    shared_counter = 0;
    a = kthread_create("mutexA", mutex_worker, NULL, PL_NORMAL, 0);
    b = kthread_create("mutexB", mutex_worker, NULL, PL_NORMAL, 0);
    if(a) task_wait(a->pid, &status);
    if(b) task_wait(b->pid, &status);
    check(a && b && shared_counter == 4000, "two tasks under a mutex count to exactly 4000");
}

/* 10/18/26 (audit B66): a mutex's waiter woken another way (wake_up_task()) just before its holder lets go is out of
   the line: mutex_unlock() found nobody in line and left the mutex free, and the waiter went back in line and slept on
   the free mutex until somebody else took it and let go (the review's R11b). Now it takes the free mutex when it runs */
static mutex_t kw_lock = MUTEX_INIT("selftest-wake");
static volatile int kw_got;

static void kw_waiter(void *arg) {
    (void)arg;
    mutex_lock(&kw_lock);
    kw_got = 1;
    mutex_unlock(&kw_lock);
}

static void test_mutex_wake(void) {
    surf_task *w;
    u_long pid, f;
    int st, waited;
    bool got, freed;
    kw_got = 0;
    mutex_lock(&kw_lock);
    w = kthread_create("mutexwake", kw_waiter, NULL, PL_NORMAL, 0);
    if(!w) { mutex_unlock(&kw_lock); check(false, "a mutex's waiter woken another way: a thread"); return; }
    pid = w->pid;
    for(waited = 0; !wq_count(&kw_lock.waiters) && waited < 1000; waited += 5) sleep_ms(5);   /* (it waits in line) */
    f = irq_save();
    wake_up_task(w);                                 /* woken another way ... */
    mutex_unlock(&kw_lock);                          /* ... and the mutex let go of before it runs: nobody in line */
    irq_restore(f);
    for(waited = 0; !kw_got && waited < 500; waited += 10) sleep_ms(10);
    got = kw_got != 0;
    freed = mutex_trylock(&kw_lock);                 /* (the code before: free, its waiter asleep in line) */
    if(freed) mutex_unlock(&kw_lock);                /* (which hands it to that waiter) */
    if(task_wait(pid, &st) != (int)pid) printf("        (the waiter did not end)\n");
    printf("        (the waiter %s within 500 ms of the letting go%s)\n", got ? "had the mutex" : "did NOT have the mutex",
           !got && freed ? "; the mutex was free meanwhile, its waiter asleep in line" : "");
    check(got, "a mutex's waiter woken another way just before the letting go takes the free mutex (no mutex left free "
          "with its waiter asleep in line)");
}

#define RING 8
static u_int ring[RING], ring_head, ring_tail;
static semaphore_t items, slots;

static void producer(void *arg) {
    u_int i;
    (void)arg;
    for(i = 1; i <= 500; i++) {
        sem_wait(&slots);
        ring[ring_head++ % RING] = i;
        sem_post(&items);
    }
}

static void test_semaphore(void) {
    surf_task *p;
    u_long sum = 0;
    u_int i;
    int status;
    ring_head = ring_tail = 0;
    sem_init(&items, 0);
    sem_init(&slots, RING);
    p = kthread_create("producer", producer, NULL, PL_NORMAL, 0);
    for(i = 0; i < 500; i++) {
        sem_wait(&items);
        sum += ring[ring_tail++ % RING];
        sem_post(&slots);
    }
    if(p) task_wait(p->pid, &status);
    check(sum == 500UL * 501 / 2, "500 items through an 8-slot ring arrive in order");
}

static event_t test_event;
static void setter(void *arg) {
    (void)arg;
    sleep_ms(20);
    event_set(&test_event);
}

static void test_event_fn(void) {
    surf_task *s;
    u_long t0;
    int rc, status;
    event_init(&test_event);
    t0 = getticks();
    rc = event_wait_timeout(&test_event, 50);
    check(rc == -1 && getticks() - t0 >= 5 && getticks() - t0 <= 10, "event_wait_timeout(50 ms) times out on time");
    s = kthread_create("setter", setter, NULL, PL_NORMAL, 0);
    rc = event_wait_timeout(&test_event, 500);
    check(rc == 0 && test_event.set, "event set by another task wakes the waiter");
    if(s) task_wait(s->pid, &status);
}

/**** FAT on the first mounted FAT volume ****/

#define FAT_VIEWS 16

/* 10/18/26 (audit B73): a kernel thread killed while it holds a file of the volume it opened (here waiting for a lock of
   the test's own) was killed at once, and the file was left open for good. The file counts for the kill now
   (surf_task.fs_refs, fs/vfs.c): the kill comes once the thread has closed it, and the fault policy's predicate does not
   count it (task_in_fs(): no lock is held). The unlink after it finds the file closed. 10/19/26 (audit B76): and the
   kill does not come in the 60 ms of clean-up after the close, where the first look after the thread let go put it */
static mutex_t vh_lock = MUTEX_INIT("rvtest-vh");
static volatile int vh_ready, vh_cleaned;
static volatile u_long vh_closed;

static void vh_file(void *arg) {
    struct file *f;
    u_long t0;
    if(vfs_open((const char *)arg, O_RDONLY, &f) != 0) return;
    vh_ready = 1;
    mutex_lock(&vh_lock);
    mutex_unlock(&vh_lock);
    vh_closed = uptime_ms();
    vfs_close(f);
    for(t0 = uptime_ms(); uptime_ms() - t0 < 60; ) ;     /* (10/19/26, B76) its clean-up: nothing counted */
    vh_cleaned = 1;
}

static void test_vfs_hold(const char *path) {
    surf_task *t;
    u_long pid, late = 0;
    int st, waited;
    bool counted = false, put_off = false;
    vh_ready = vh_cleaned = 0;
    vh_closed = 0;
    mutex_lock(&vh_lock);
    t = kthread_create("vhfile", vh_file, (void *)path, PL_NORMAL, 0);
    pid = t ? t->pid : 0;
    for(waited = 0; t && !vh_ready && waited < 1000; waited += 5) sleep_ms(5);
    sleep_ms(50);                                    /* (it waits for the test's lock) */
    if(t && vh_ready) {
        counted = t->fs_refs == 1 && !task_in_fs(t);
        kill_task(t);
        sleep_ms(300);
        put_off = task_wait_opts(pid, &st, WNOHANG) == 0;   /* (still there: not killed at once) */
    }
    mutex_unlock(&vh_lock);
    if(put_off) {
        task_wait(pid, &st);
        late = uptime_ms() - vh_closed;
    } else if(t && !vh_ready) task_wait(pid, &st);
    if(put_off) printf("        (its file counted for the kill: %s; ended %lu ms after it closed it, its clean-up "
                       "%s)\n", counted ? "yes" : "NO", late, vh_cleaned ? "done" : "CUT SHORT");
    else printf("        (its file counted for the kill: %s; killed AT ONCE)\n", counted ? "yes" : "NO");
    check(counted && put_off && vh_closed && late < 250, "B73: a kernel thread holding a file of the volume, killed "
          "while it waits for another lock, is killed once it has closed the file");
    check(put_off && vh_cleaned, "B76: and the kill does not come in its clean-up after the close: it ends by itself");
}

/* 10/19/26 (audit B80): the same thread, the test's lock let go at once after the kill: it closes the file before the
   first look, 20 ms after the kill, which killed it at once there, in its clean-up (only a look marked a task as one
   holding something; kill_task() itself puts the kill of a thread holding something off): the kill marks it now, and
   the first look gives it its grace */
static void test_vfs_hold_soon(const char *path) {
    surf_task *t;
    u_long pid, kl = 0, tk = 0;
    int st = 1, waited;
    vh_ready = vh_cleaned = 0;
    vh_closed = 0;
    mutex_lock(&vh_lock);
    t = kthread_create("vhsoon", vh_file, (void *)path, PL_NORMAL, 0);
    pid = t ? t->pid : 0;
    for(waited = 0; t && !vh_ready && waited < 1000; waited += 5) sleep_ms(5);
    sleep_ms(50);                                    /* (it waits for the test's lock) */
    if(t && vh_ready) {
        tk = uptime_ms();
        kill_task(t);                                /* put off: it holds the file */
    }
    mutex_unlock(&vh_lock);                          /* it lets go now */
    if(t) task_wait(pid, &st);
    if(vh_closed && tk) kl = vh_closed - tk;
    printf("        (it closed the file %lu ms after its kill: its clean-up %s, status %d)\n", kl,
           vh_cleaned ? "done" : "CUT SHORT", st);
    check(t && vh_ready && vh_cleaned && st == 0, "B80: and one that lets go of the file before the first look after its "
          "kill finishes its clean-up and ends by itself");
}

static void test_fat(void) {
    struct vfs_mount_view *views;
    struct file *f;
    struct stat st;
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX];
    u8 *w, *rb;
    int r, n, i, found = -1;

    views = (struct vfs_mount_view *)kcalloc(FAT_VIEWS, sizeof(struct vfs_mount_view));
    if(!views) { check(false, "FAT test memory"); return; }
    n = vfs_mount_views(views, FAT_VIEWS);
    for(i = 0; i < n && i < FAT_VIEWS; i++) {
        if(views[i].type && !strncmp(views[i].type, "fat", 3) && !views[i].readonly) { found = i; break; }
    }
    if(found < 0) {
        printf("  skip FAT (no FAT volume mounted read-write)\n");
        kfree(views);
        return;
    }
    strlcpy(dir, views[found].path, sizeof(dir));
    kfree(views);
    w = (u8 *)kalloc(10240);
    rb = (u8 *)kalloc(10240);
    if(!w || !rb) { check(false, "FAT test buffers"); kfree(w); kfree(rb); return; }
    for(i = 0; i < 10240; i++) w[i] = (u8)(i * 13 + (i >> 8));
    snprintf(path, sizeof(path), "%s/selftest.bin", dir);
    r = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if(r == 0) { r = vfs_write(f, w, 10240); vfs_close(f); }
    check(r == 10240, "write 10 KB to selftest.bin on the FAT volume");
    r = vfs_open(path, O_RDONLY, &f);
    if(r == 0) { memset(rb, 0, 10240); r = vfs_read(f, rb, 10240); vfs_close(f); }
    check(r == 10240 && !memcmp(w, rb, 10240), "read it back identically");
    test_vfs_hold(path);                             /* 10/18/26 (audit B73) */
    test_vfs_hold_soon(path);                        /* 10/19/26 (audit B80) */
    check(vfs_unlink(path) == 0 && vfs_stat(path, &st) == -ENOENT, "unlink removes it");
    kfree(w);
    kfree(rb);
}

static void (*const tests[])(void) = {
    test_abi, test_registers, test_stack, test_math, test_memory, test_crc, test_time, test_timers, test_tasks,
    test_mutex_fn, test_mutex_wake, test_semaphore, test_event_fn, test_fat
};

int rv_selftest(void) {
    u_int i;
    if(!claim("selftest")) return 1;
    fails = 0;
    printf("\nSurfOS self test (riscv32, %s)\n----------------\n", board_name());
    check(heap_round() == 0, "heap");
    for(i = 0; i < sizeof(tests) / sizeof(tests[0]) && !task_interrupted(); i++) tests[i]();
    check(klog_length() > 0, "kernel log has the boot messages");
    if(task_interrupted()) printf("SELFTEST STOPPED (cut short by a kill or Ctrl+C after %u of %u tests)\n\n", i,
                                  (u_int)(sizeof(tests) / sizeof(tests[0])));
    else printf("%s (%s)\n\n", fails ? "SELFTEST FAIL" : "SELFTEST PASS", fails ? "see above" : "all checks passed");
    unclaim();
    return fails || task_interrupted();
}
