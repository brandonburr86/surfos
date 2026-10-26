;SurfOS Trap Entry Stubs
;(C)2004 Brandon Burr, rebuilt 10/2026 (roadmap K1)
;-----------------------------------------------
; One stub per vector. Each pushes an error code (the CPU's, or 0) and its vector
; number, then trap_common saves the registers, builds a struct trapframe
; (include/surfos/trap.h) and calls trap_dispatch(). The C side returns the frame to
; resume, which is a different task's frame when the scheduler ran.

[BITS 32]

GLOBAL isr_stub_table
GLOBAL double_fault_entry
EXTERN trap_dispatch
EXTERN double_fault_task

%macro TRAP_NOERR 1
isr%1:
    push dword 0            ; dummy error code, keeps every frame the same shape
    push dword %1
    jmp trap_common
%endmacro

%macro TRAP_ERR 1
isr%1:
    push dword %1           ; the CPU already pushed the error code
    jmp trap_common
%endmacro

SECTION .text

; vectors whose exception pushes an error code: #DF #TS #NP #SS #GP #PF #AC #CP and the
; two reserved ones Intel documents with error codes (29, 30)
%assign v 0
%rep 256
  %if v == 8 || v == 10 || v == 11 || v == 12 || v == 13 || v == 14 || v == 17 || v == 21 || v == 29 || v == 30
    TRAP_ERR v
  %else
    TRAP_NOERR v
  %endif
%assign v v+1
%endrep

ALIGN 4
trap_common:
    push gs
    push fs
    push es
    push ds
    pusha
    mov ax, 0x10            ; kernel data segment, whatever ring we came from
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp                ; struct trapframe *
    call trap_dispatch
    add esp, 4
    mov esp, eax            ; frame to resume
    popa
    pop ds
    pop es
    pop fs
    pop gs
    add esp, 8              ; vector and error code
    iret

; 10/25/26 (robustness, L16): vector 8 is a task gate (kernel/gdt.c, kernel/interrupt.c). The CPU arrives here in
; the double fault's own task, on its own stack, with interrupts off and the error code (0) pushed; what it
; interrupted is in the main TSS. The report halts the machine and never returns.
ALIGN 4
double_fault_entry:
    call double_fault_task
.halt:
    cli
    hlt
    jmp .halt

SECTION .rodata
ALIGN 4
isr_stub_table:
%assign v 0
%rep 256
    dd isr%+v
%assign v v+1
%endrep

; tell the linker this object needs no executable stack
SECTION .note.GNU-stack noalloc noexec nowrite progbits
