GLOBAL dma_stuff

dma_stuff:
    pop edx
redo:
    in   al,dx
    mov bl,al
    in al,dx
    mov  bh,al
    in al,dx
    mov ah,al
    in al,dx
    xchg ah,al
    sub  bx,ax
    cmp  bx,40h
    jg redo
    cmp bx,0FFC0h
    jl redo
    ret

; tell the linker this object needs no executable stack
SECTION .note.GNU-stack noalloc noexec nowrite progbits
