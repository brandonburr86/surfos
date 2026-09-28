;SurfOS Assembly Interrupt Service Routine Stubs
;(C)2004 Brandon Burr
;-----------------------------------------------

[BITS 32]

GLOBAL outb
GLOBAL outw
GLOBAL outd
GLOBAL timerISR
GLOBAL isrParPort
GLOBAL keybISR
GLOBAL defISR
GLOBAL inb
GLOBAL inw
GLOBAL ind

GLOBAL isrStartIRQ
GLOBAL isrEndIRQ
EXTERN handleIRQ

EXTERN timer_handler
EXTERN switch_int_task
EXTERN keyboard_handler
EXTERN parPortISR
GLOBAL reset

%define port [ebp + 8]
%define data [ebp + 12]

%macro PUSHREGS 0
    push gs
    push fs
    push es
    push ds
    pusha
%endmacro

%macro POPREGS 0
    popa
   pop ds
   pop es
   pop fs
   pop gs
%endmacro


ALIGN 4
reset:
    cli
   WaitOutReady:
   in al,64h
   test al,00000010b ; this bit indicates input buffer full
   jnz WaitOutReady
   mov al,0FEh       ; Pulse "reset" = 8042 pin 0
   out 64h,al        ; The PC will reboot now

ALIGN 4
outb:
    push ebp
    mov ebp, esp
    push eax
    push edx
    mov edx, port
    mov eax, data
    out dx, al
    pop edx
    pop eax
    leave
    ret

ALIGN 4
outw:
    push ebp
    mov ebp, esp
    push eax
    push edx
    mov edx, port
    mov eax, data
    out dx, ax
    pop edx
    pop eax
    leave
    ret

ALIGN 4
outd:
    push ebp
    mov ebp, esp
    push eax
    push edx
    mov edx, port
    mov eax, data
    out dx, eax
    pop edx
    pop eax
    leave
    ret

ALIGN 4
inb:
    push ebp
    mov ebp, esp
    push edx
    mov edx, port
    xor eax, eax
    in al, dx
    pop edx
    leave
    ret

ALIGN 4
inw:
    push ebp
    mov ebp, esp
    push edx
    mov edx, port
    xor eax, eax
    in ax, dx
    pop edx
    leave
    ret

ALIGN 4
ind:
    push ebp
    mov ebp, esp
    push edx
    mov edx, port
    xor eax, eax
    in eax, dx
    pop edx
    leave
    ret


EXTERN getCurESP
EXTERN getNextESP
EXTERN dump_regs
EXTERN reboot
EXTERN schedule

ALIGN 4
timerISR:
    cli
    cld

    PUSHREGS

    mov al, 0x20
    out 0x20,al

   push esp ;the old stack pointer
   call timer_handler
   add esp, 4

    mov esp, eax

    POPREGS

   sti
   iret


  GLOBAL task_yield
ALIGN 4


task_yield:
    cli
    cld

    PUSHREGS

   push esp
   call schedule
   add esp, 4

    mov esp, eax

    POPREGS

   sti
   iret

ALIGN 4
preempt:

    pop eax
    mov esp, eax

    POPREGS
   sti
   iret

ALIGN 4
isrParPort:
    cli
    cld
    PUSHREGS

   call parPortISR
   mov al, 0x20
    out 0x20,al

   POPREGS
   sti
   iret

ALIGN 4

keybISR:
    PUSHREGS

    call keyboard_handler
    mov al,0x20
    out 0x20,al

    POPREGS
    iret

%macro SWAP_INT_TASK 0
    push esp
    call switch_int_task
    add esp,4
    mov esp, eax
%endmacro

ALIGN 4
isrStartIRQ:
%assign irq 1
%rep 15
    push ebx
    mov ebx, irq
    jmp isrIRQ
%assign irq irq+1
%endrep
isrEndIRQ:

isrIRQ:
    cld
    cli
    PUSHREGS

    SWAP_INT_TASK

    push ebx        ;tell handleIRQ what IRQ (EAX saved by isrIRQ[x])
    call handleIRQ
    pop ebx         ;undo push

    SWAP_INT_TASK

    POPREGS
    pop ebx

    sti
    iret



; EXCEPTIONS
GLOBAL ex0
EXTERN exDivZero
ALIGN 4
ex0:

    PUSHREGS

    push esp
   call exDivZero
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex1
EXTERN exDebug
ALIGN 4
ex1:

    PUSHREGS

    push esp
   call exDebug
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS

   iret

GLOBAL ex2
EXTERN exNMI
ALIGN 4
ex2:
    PUSHREGS

    push esp
   call exNMI
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex3
EXTERN exBreakpoint
ALIGN 4
ex3:

    PUSHREGS

   push esp
   call exBreakpoint
   add esp, 4

    POPREGS

   iret

GLOBAL ex4
EXTERN exOverflow
ALIGN 4
ex4:

    PUSHREGS

    push esp
   call exOverflow
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex5
EXTERN exBoundRange
ALIGN 4
ex5:
    PUSHREGS

    push esp
   call exBoundRange
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex6
EXTERN exInvalOpcode
ALIGN 4
ex6:
    PUSHREGS

   push esp
   call exInvalOpcode
   add esp, 4

   mov al, 0x20
    out 0x20,al

    POPREGS

   iret

GLOBAL ex7
EXTERN exDevNotAvailable
ALIGN 4
ex7:
    PUSHREGS

    push esp
   call exDevNotAvailable
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex8
EXTERN exDoubleFault
ALIGN 4
ex8:
    PUSHREGS

    push esp
   call exDoubleFault
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex9
EXTERN exCoprocSeg
ALIGN 4
ex9:

    PUSHREGS

    push esp
   call exCoprocSeg
   add esp, 4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex10
EXTERN exInvalTSS
ALIGN 4
ex10:
    PUSHREGS

    push esp
   call exInvalTSS
   add esp, 4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex11
EXTERN exSegNotPresent
ALIGN 4
ex11:
    PUSHREGS

    push esp
   call exSegNotPresent
   add esp, 4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex12
EXTERN exStackFault
ALIGN 4
ex12:

    PUSHREGS

    push esp
   call exStackFault
   add esp, 4

   mov al, 0x20
    out 0x20,al

   POPREGS

   iret

GLOBAL ex13
EXTERN exGPF
ALIGN 4
ex13:
   add esp,4
    PUSHREGS

   push esp
   call exGPF
   add esp,4

   mov al, 0x20
    out 0x20,al

    POPREGS

   iret

GLOBAL ex14
EXTERN exPageFault
ALIGN 4
ex14:
    PUSHREGS

   call exPageFault

   POPREGS

    add esp,4
   iret

GLOBAL ex15
EXTERN exFPError
ALIGN 4
ex15:
    PUSHREGS

    push esp
   call exFPError
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex16
EXTERN exAlignCheck
ALIGN 4
ex16:
    PUSHREGS

    push esp
   call exAlignCheck
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex17
EXTERN exMachineCheck
ALIGN 4
ex17:
    PUSHREGS

    push esp
   call exMachineCheck
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS
   iret

GLOBAL ex18
EXTERN exSIMDFP
ALIGN 4
ex18:
    PUSHREGS

    push esp
   call exSIMDFP
   add esp,4

   mov al, 0x20
    out 0x20,al

   POPREGS

   iret
