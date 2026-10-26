/*
SurfOS board: SurfCPU on the Arty A7 -- SurfGPU's hardware side
--------------------
File: gpuhw.c   Date: 10/25/26 (SurfGPU G3)
--------------------
The port of gpu.h on the board (surfcpu/docs/GPU.md, MEMORY-MAP.md, every address and bit from
the generated soc.h): the gpu and video registers and the VRAM window, the machine timer for
time, yield() between polls and sleep_intr() for sleeps, Ctrl+C from the task, the GPU's
interrupt (intc source 5, STATUS.FENCE_EV with IE.FENCE: a FENCE sent with its IRQ flag) waking
the task that waits for a fence, interrupts off for the few stores of a display change, the text
console's copy of CTRL (textcon.c) under the console lock, a mutex for the claim, and the kernel
heap for the demo's copy of VRAM. A bitstream without FEATURES.IRQ gets the port without the
interrupt: fences are then waited for with sleeps. 10/26/26: a periodic kernel timer (every tick,
10 ms, from the timer interrupt) for gfx cube's looks at FRAME_COUNT.

Also the driver's entry in board.c's table (the +GPU: lines at boot, said once, whether there is
a GPU or not), its line in `drivers`, and the gfx command's kernel side (boards/arty_a7/gfx.c).

The driver starts in kmain() as the idle task, before the tick runs: there it neither yields nor
sleeps on a queue (nothing may run before the drivers are up), it spins on the machine timer.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
#include <surfos/ktimer.h>
#include <surfos/timer.h>                       /* HZ */
#include <surfos/irq.h>
#include <surfos/interrupt.h>
#include <mm/kalloc.h>
#include <asm/io.h>
#include <surfcpu/soc.h>
#include <riscv/board.h>
#include "arty.h"
#include "gpu.h"
#include "../../shell/shell.h"
#include <blibc_common.h>

#define GREG(off)   ((u_long)SURF_GPU_BASE + (off))
#define VREG(off)   ((u_long)SURF_VIDEO_BASE + (off))
#define VRAM(off)   ((u_long)SURF_VRAM_BASE + (off))

static struct gpu gpu;
static mutex_t gpu_lock = MUTEX_INIT("gpu");
static struct surf_task *volatile holder;      /* who has the claim (for the message only) */
static event_t fence_event;
static bool have_isr;
static struct ktimer look_timer;               /* gfx cube's looks, under the claim (one run at a time) */

/* a task the scheduler may switch away from (not the idle task while the drivers start) */
static bool can_wait(void) {
    return curTask && curTask != idle_task && !in_interrupt() && irqs_enabled();
}

static u32 p_rd(void *c, u32 off) { (void)c; return mmio_read32(GREG(off)); }
static void p_wr(void *c, u32 off, u32 v) { (void)c; mmio_write32(GREG(off), v); }
static u32 p_vrd(void *c, u32 off) { (void)c; return mmio_read32(VREG(off)); }
static void p_vwr(void *c, u32 off, u32 v) { (void)c; mmio_write32(VREG(off), v); }
/* words through the window four at a time: every instruction comes from DDR, so the loop's own
   cost is most of a word's */
static void p_vram_read(void *c, u32 off, u32 *dst, u32 words) {
    const volatile u32 *v = (const volatile u32 *)VRAM(off);
    (void)c;
    for(; words >= 4; words -= 4, v += 4, dst += 4) {
        u32 a = v[0], b = v[1], d = v[2], e = v[3];
        dst[0] = a;
        dst[1] = b;
        dst[2] = d;
        dst[3] = e;
    }
    for(; words; words--) *dst++ = *v++;
}

static void p_vram_write(void *c, u32 off, const u32 *src, u32 words) {
    volatile u32 *v = (volatile u32 *)VRAM(off);
    (void)c;
    for(; words >= 4; words -= 4, v += 4, src += 4) {
        u32 a = src[0], b = src[1], d = src[2], e = src[3];
        v[0] = a;
        v[1] = b;
        v[2] = d;
        v[3] = e;
    }
    for(; words; words--) *v++ = *src++;
}
static u8 p_vram_rd8(void *c, u32 off) { (void)c; return mmio_read8(VRAM(off)); }
static void p_vram_wr8(void *c, u32 off, u8 v) { (void)c; mmio_write8(VRAM(off), v); }

static u64 p_us(void *c) {
    u64 t = board_mtime();
    u32 hz = board_timebase_hz();
    (void)c;
    if(hz == 1000000u) return t;                    /* SURF_TIMEBASE_HZ: mtime counts microseconds */
    return t / hz * 1000000u + t % hz * 1000000u / hz;
}

static void spin_ms(u32 ms) {
    u64 end = p_us(NULL) + (u64)ms * 1000u;
    while(p_us(NULL) < end) ;
}

static void p_relax(void *c) {
    (void)c;
    if(can_wait()) yield();
}

static int p_sleep(void *c, u32 ms) {
    (void)c;
    if(!can_wait()) {
        spin_ms(ms);
        return 0;
    }
    return sleep_intr(ms) ? GPU_EINTR : 0;
}

static bool p_interrupted(void *c) { (void)c; return task_interrupted(); }

static void p_irq_arm(void *c) { (void)c; event_clear(&fence_event); }

/* 1 when the interrupt came (event_wait_timeout(): 0, set before the wait or by the handler's
   wake-up), 0 when the time passed, GPU_EINTR when Ctrl+C cut it short (10/26/26: the driver counts
   the waits the interrupt ended) */
static int p_irq_wait(void *c, u32 ms) {
    int r;
    (void)c;
    if(!can_wait()) {
        spin_ms(ms);
        return 0;
    }
    r = event_wait_timeout(&fence_event, ms);
    return r == -EINTR ? GPU_EINTR : r == 0 ? 1 : 0;
}

static u_long p_atomic_begin(void *c) { (void)c; return irq_save(); }
static void p_atomic_end(void *c, u_long flags) { (void)c; irq_restore(flags); }
static int p_text_lock(void *c) { (void)c; return arty_text_lock(); }
static void p_text_unlock(void *c, int how) { (void)c; arty_text_unlock(how); }
static u32 p_get_mode(void *c) { (void)c; return arty_text_mode(); }
static void p_set_mode(void *c, u32 mode) { (void)c; arty_text_set_mode(mode); }

static bool p_trylock(void *c) {
    (void)c;
    if(!mutex_trylock(&gpu_lock)) return false;
    holder = curTask;
    return true;
}

static void p_unlock(void *c) {
    (void)c;
    holder = NULL;
    mutex_unlock(&gpu_lock);
}

static const char *p_holder(void *c, char *buf, u_int size) {
    struct surf_task *t = holder;
    (void)c;
    if(t) snprintf(buf, size, "'%s', pid %lu", t->name, (u_long)t->pid);
    else snprintf(buf, size, "another task");
    return buf;
}

static void *p_alloc(void *c, u32 bytes) { (void)c; return kalloc(bytes); }
static void p_free(void *c, void *p) { (void)c; kfree(p); }

/* fn from the timer interrupt every tick (ktimer.h: interrupts off; it reads two registers) */
static void p_tick_start(void *c, void (*fn)(void *arg), void *arg) {
    (void)c;
    ktimer_init(&look_timer, fn, arg);
    ktimer_start(&look_timer, 1000 / HZ, true);
}

static void p_tick_stop(void *c) {
    (void)c;
    ktimer_cancel(&look_timer);
}

static const struct gpu_port port_irq = {
    NULL, p_rd, p_wr, p_vrd, p_vwr, p_vram_read, p_vram_write, p_vram_rd8, p_vram_wr8, p_us, p_relax, p_sleep,
    p_interrupted, p_irq_arm, p_irq_wait, p_atomic_begin, p_atomic_end, p_text_lock, p_text_unlock, p_get_mode,
    p_set_mode, p_trylock, p_unlock, p_holder, p_alloc, p_free, p_tick_start, p_tick_stop
};

static const struct gpu_port port_poll = {             /* no interrupt path: fences with sleeps */
    NULL, p_rd, p_wr, p_vrd, p_vwr, p_vram_read, p_vram_write, p_vram_rd8, p_vram_wr8, p_us, p_relax, p_sleep,
    p_interrupted, NULL, NULL, p_atomic_begin, p_atomic_end, p_text_lock, p_text_unlock, p_get_mode,
    p_set_mode, p_trylock, p_unlock, p_holder, p_alloc, p_free, p_tick_start, p_tick_stop
};

/* the GPU's interrupt: only IE.FENCE is enabled, so it is a FENCE with its IRQ flag; clearing
   FENCE_EV drops the level */
static int gpu_isr(u_int irq, void *arg) {
    u32 st = mmio_read32(GREG(SURF_GPU_STATUS));
    (void)irq;
    (void)arg;
    if(st & SURF_GPU_STATUS_FENCE_EV_MASK) {
        mmio_write32(GREG(SURF_GPU_STATUS), SURF_GPU_STATUS_FENCE_EV_MASK);
        gpu.irqs++;
        event_set(&fence_event);
    }
    return 0;
}

/**** the driver table's entry (board.c) ****/

int arty_gpu_init(void) {
    u32 features = arty_features();
    bool irq = (features & SURF_SYSINFO_FEATURES_GPU_MASK) && (features & SURF_SYSINFO_FEATURES_IRQ_MASK);
    int r;
    event_init(&fence_event);
    if(irq) {                                   /* before IE.FENCE is set (gpu_probe()) */
        add_irq_handler(SURF_IRQ_GPU, gpu_isr, NULL);
        have_isr = true;
    }
    r = gpu_probe(&gpu, irq ? &port_irq : &port_poll, features);
    if(r == GPU_EABSENT) {
        if(have_isr) {
            del_irq_handler(SURF_IRQ_GPU, gpu_isr, NULL);
            have_isr = false;
        }
        kprintf("+GPU: none: %s\n", gpu.why);
        return 1;
    }
    kprintf("+GPU: SurfGPU %s (gpu.ID 0x%08lx), VRAM %lu KiB at 0x%08lx\n+GPU: a %lu-word command FIFO; fences %s\n",
            gpu_id_name(gpu.id), (u_long)gpu.id, (u_long)(gpu.vram_bytes / 1024), (u_long)SURF_VRAM_BASE,
            (u_long)gpu.fifo_depth, gpu.irq ? "wait on intc source 5 (IE.FENCE)" : "wait with sleeps (no interrupts)");
    if(r) {
        kprintf("+GPU: the engine did not finish its reset: gfx reset tries again\n");
        return -1;
    }
    return 0;
}

const char *arty_gpu_describe(char *buf, size_t size) {
    return gpu_describe(&gpu, buf, (u_int)size);
}

/* gfx (boards/arty_a7/gfx.c) on this board's GPU */
int arty_cmd_gfx(int argc, char **argv) {
    return gfx_command(&gpu, argc, argv);
}
