/*
SurfOS User Address Spaces
--------------------
File: uvm.c     Date: 10/3/26 (roadmap P1)
--------------------
See include/mm/uvm.h.
*/

#include <surfos/types.h>
#include <surfos/console.h>
#include <surfos/panic.h>
#include <surfos/irq.h>
#include <mm/memory.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <mm/kalloc.h>
#include <mm/uvm.h>
#include <blibc_common.h>

extern u_long *page_directory;                  /* mm/paging.c: the kernel's */
static struct uvm *current_uvm;                 /* NULL while the kernel directory is loaded */

static inline void invlpg(u_long virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline void lcr3(u_long phys) {
    asm volatile("movl %0, %%cr3" : : "r"(phys) : "memory");
}

u_long kernel_pdir_phys(void) {
    return (u_long)page_directory;              /* in .bss, inside the identity map */
}

/* a page-aligned, zeroed, mapped 4 KB block from the heap; *raw is what to kfree() */
static u_long *alloc_table_page(void **raw) {
    u8 *p = (u8 *)kalloc(2 * PAGE_SIZE);
    u_long *aligned;
    if(!p) return NULL;
    aligned = (u_long *)PAGE_ALIGN_UP((u_long)p);
    memset(aligned, 0, PAGE_SIZE);              /* also faults the heap page in, so it has a frame */
    *raw = p;
    return aligned;
}

struct uvm *uvm_create(void) {
    struct uvm *m = (struct uvm *)kcalloc(1, sizeof(struct uvm));
    u_int i;
    if(!m) return NULL;
    m->pdir = alloc_table_page(&m->pdir_raw);
    if(!m->pdir) { kfree(m); return NULL; }
    memcpy(m->pdir, page_directory, PAGE_SIZE); /* kernel mappings: the same tables */
    for(i = 0; i < USER_PDE_COUNT; i++) m->pdir[USER_PDE_FIRST + i] = 0;
    m->pdir_phys = vmm_get_phys((u_long)m->pdir);
    m->stack_low = USER_END;
    return m;
}

void uvm_destroy(struct uvm *m) {
    u_int i, j;
    if(!m) return;
    if(m == current_uvm) panic("uvm_destroy: the address space is in use");
    for(i = 0; i < USER_PDE_COUNT; i++) {
        if(!m->tables[i]) continue;
        for(j = 0; j < 1024; j++) {
            if(m->tables[i][j] & PTE_P) pmm_free(m->tables[i][j] & PTE_FRAME);
        }
        kfree(m->tables_raw[i]);
    }
    kfree(m->pdir_raw);
    kfree(m);
}

void uvm_switch(struct uvm *m) {
    current_uvm = m;
    lcr3(m ? m->pdir_phys : kernel_pdir_phys());
}

struct uvm *uvm_current(void) {
    return current_uvm;
}

static u_long *table_for(struct uvm *m, u_long virt, bool create) {
    u_int pde = virt >> 22, i;
    if(virt < USER_START || virt >= USER_END) return NULL;
    i = pde - USER_PDE_FIRST;
    if(!m->tables[i]) {
        if(!create) return NULL;
        m->tables[i] = alloc_table_page(&m->tables_raw[i]);
        if(!m->tables[i]) return NULL;
        m->pdir[pde] = vmm_get_phys((u_long)m->tables[i]) | PTE_P | PTE_W | PTE_U;
    }
    return m->tables[i];
}

int uvm_map_page(struct uvm *m, u_long virt, u_long flags) {
    u_long *tbl, frame;
    virt = PAGE_ALIGN_DOWN(virt);
    if(m != current_uvm) return -1;             /* zeroing needs the mapping to be live */
    tbl = table_for(m, virt, true);
    if(!tbl) return -1;
    if(tbl[(virt >> 12) & 1023] & PTE_P) return 0;   /* already there */
    frame = pmm_alloc();
    if(!frame) return -1;
    tbl[(virt >> 12) & 1023] = frame | PTE_P | PTE_U | (flags & PTE_W);
    invlpg(virt);
    memset((void *)virt, 0, PAGE_SIZE);
    m->pages++;
    return 0;
}

int uvm_map_range(struct uvm *m, u_long start, u_long len, u_long flags) {
    u_long a, end = PAGE_ALIGN_UP(start + len);
    if(start >= end || end > USER_END || start < USER_START) return -1;
    for(a = PAGE_ALIGN_DOWN(start); a < end; a += PAGE_SIZE) {
        if(uvm_map_page(m, a, flags) != 0) return -1;
    }
    return 0;
}

u_long uvm_lookup(struct uvm *m, u_long virt) {
    u_long *tbl = table_for(m, virt, false), pte;
    if(!tbl) return 0;
    pte = tbl[(virt >> 12) & 1023];
    if(!(pte & PTE_P)) return 0;
    return (pte & PTE_FRAME) | (virt & (PAGE_SIZE - 1));
}

int uvm_grow_stack(struct uvm *m, u_long addr) {
    u_long a;
    if(addr < USER_END - USER_STACK_MAX || addr >= m->stack_low) return -1;
    for(a = PAGE_ALIGN_DOWN(addr); a < m->stack_low; a += PAGE_SIZE) {
        if(uvm_map_page(m, a, PTE_W) != 0) return -1;
    }
    m->stack_low = PAGE_ALIGN_DOWN(addr);
    return 0;
}

bool uvm_check(struct uvm *m, u_long start, u_long len, bool write) {
    u_long a, end = start + len;
    if(!m || end < start || start < USER_START || end > USER_END) return false;
    if(!len) return true;
    for(a = PAGE_ALIGN_DOWN(start); a < end; a += PAGE_SIZE) {
        u_long *tbl = table_for(m, a, false), pte = tbl ? tbl[(a >> 12) & 1023] : 0;
        if(!(pte & PTE_P)) {
            if(uvm_grow_stack(m, a) != 0) return false;   /* a buffer on the not yet touched stack */
            continue;
        }
        if(write && !(pte & PTE_W)) return false;
    }
    return true;
}

bool uvm_check_str(struct uvm *m, const char *s, u_long max) {
    u_long a = (u_long)s, n;
    if(!m || a < USER_START || a >= USER_END) return false;
    for(n = 0; n < max && a + n < USER_END; n++) {
        if((((a + n) & (PAGE_SIZE - 1)) == 0 || n == 0) && !uvm_check(m, a + n, 1, false)) return false;
        if(s[n] == 0) return true;
    }
    return false;
}

u_long uvm_sbrk(struct uvm *m, long incr) {
    u_long old = m->brk, nb;
    if(!m->brk_start) return 0;
    if(incr == 0) return old;
    if(incr < 0) {                               /* give nothing back, just move the mark */
        if((u_long)(-incr) > old - m->brk_start) return 0;
        m->brk = old + incr;
        return old;
    }
    nb = old + incr;
    if(nb < old || nb > USER_END - USER_STACK_MAX) return 0;
    if(PAGE_ALIGN_UP(nb) > PAGE_ALIGN_UP(old)) {
        if(uvm_map_range(m, PAGE_ALIGN_UP(old), PAGE_ALIGN_UP(nb) - PAGE_ALIGN_UP(old), PTE_W) != 0) return 0;
    }
    m->brk = nb;
    return old;
}
