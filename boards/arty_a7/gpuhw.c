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
interrupt: fences are then waited for with sleeps.

Also the driver's entry in board.c's table (the +GPU: lines at boot, said once, whether there is
a GPU or not), its line in `drivers`, and the gfx command's kernel side (boards/arty_a7/gfx.c).

The driver starts in kmain() as the idle task, before the tick runs: there it neither yields nor
sleeps on a queue (nothing may run before the drivers are up), it spins on the machine timer.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/task.h>
#include <surfos/sync.h>
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

/* a task the scheduler may switch away from (not the idle task while the drivers start) */
static bool can_wait(void) {
    return curTask && curTask != idle_task && !in_interrupt() && irqs_enabled();
}

static u32 p_rd(void *c, u32 off) { (void)c; return mmio_read32(GREG(off)); }
static void p_wr(void *c, u32 off, u32 v) { (void)c; mmio_write32(GREG(off), v); }
static u32 p_vrd(void *c, u32 off) { (void)c; return mmio_read32(VREG(off)); }
static void p_vwr(void *c, u32 off, u32 v) { (void)c; mmio_write32(VREG(off), v); }
static u32 p_vram_rd(void *c, u32 off) { (void)c; return mmio_read32(VRAM(off)); }
static void p_vram_wr(void *c, u32 off, u32 v) { (void)c; mmio_write32(VRAM(off), v); }
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

static int p_irq_wait(void *c, u32 ms) {
    (void)c;
    if(!can_wait()) {
        spin_ms(ms);
        return 0;
    }
    return event_wait_timeout(&fence_event, ms) == -EINTR ? GPU_EINTR : 0;
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

static const struct gpu_port port_irq = {
    NULL, p_rd, p_wr, p_vrd, p_vwr, p_vram_rd, p_vram_wr, p_vram_rd8, p_vram_wr8, p_us, p_relax, p_sleep,
    p_interrupted, p_irq_arm, p_irq_wait, p_atomic_begin, p_atomic_end, p_text_lock, p_text_unlock, p_get_mode,
    p_set_mode, p_trylock, p_unlock, p_holder, p_alloc, p_free
};

static const struct gpu_port port_poll = {             /* no interrupt path: fences with sleeps */
    NULL, p_rd, p_wr, p_vrd, p_vwr, p_vram_rd, p_vram_wr, p_vram_rd8, p_vram_wr8, p_us, p_relax, p_sleep,
    p_interrupted, NULL, NULL, p_atomic_begin, p_atomic_end, p_text_lock, p_text_unlock, p_get_mode,
    p_set_mode, p_trylock, p_unlock, p_holder, p_alloc, p_free
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
        if(gpu.id) kprintf("+GPU: none: %s (it reads 0x%08lx)\n", gpu.why, (u_long)gpu.id);
        else kprintf("+GPU: none: %s\n", gpu.why);
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
    if(!gpu.present) {
        snprintf(buf, size, "none: %s", gpu.why ? gpu.why : "not probed");
        return buf;
    }
    snprintf(buf, size, "SurfGPU %s, %lu KiB VRAM, FIFO %lu; %lu words, %lu fences, %lu errors", gpu_id_name(gpu.id),
             (u_long)(gpu.vram_bytes / 1024), (u_long)gpu.fifo_depth, gpu.words, gpu.fences, gpu.errors);
    return buf;
}

/* gfx (boards/arty_a7/gfx.c) on this board's GPU */
int arty_cmd_gfx(int argc, char **argv) {
    return gfx_command(&gpu, argc, argv);
}
