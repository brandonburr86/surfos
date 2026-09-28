;SurfOS Assembly Kernel Entry
;(C)2004 Brandon Burr
;-----------------------------------------------

[BITS 32]

;start of kernel

PG1 equ 0x1007

mov ax, 0x10            ; Set ds, es, ss to data selector(base 0x0)
mov ds, ax
mov ax, 0x10
mov es, ax
mov ax, 0x10
mov ss, ax
mov esp, 0xFFFF         ; Stack 0x0000:0xFFFF to 0x0000:0x0000

[global page_setup_start]
page_setup_start:
    mov ecx, 1024*5
    mov eax, 0x0
    mov edi, 0x0
    cld
    rep stosd

    mov eax, 0x0
    mov dword [eax], PG1

    add eax, 4
    mov dword [eax], PG1 + 0x1000

    add eax, 4
    mov dword [eax], PG1 + 0x2000

    add eax, 4
    mov dword [eax], PG1 + 0x3000

    mov edi, 0x1000
    mov eax, 0x0007

    cld

fill_pte:
    stosd
    add eax, 0x1000
    cmp eax, 0x1000007
    jnz fill_pte

    mov eax, 0x0
    mov cr3, eax

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
  mov eax, 0xB800
  mov dword [eax],'A'
  hlt
 ret

        push dword main_ret         ; If main returns goto to main_ret

        jmp 0x8:0x7400; 0x8:0x7400

main_ret:
        mov byte al, '*'
        mov byte [es:0xb8A02], al

surf_dummy:
       jmp surf_dummy


;----------------------------------------------------------------------------
; function
;----------------------------------------------------------------------------

surf_pmode_print_character:

        pushad                          ; Save registers

        cmp al, 10                      ; Linefeed character is 10
        jnz surf_not_line_feed

        add byte [surf_yposition], 1
        jmp surf_pmode_print_character_done

surf_not_line_feed:
        cmp al, 13                      ; Carriage return character is 13
        jnz surf_not_carriage_return

        mov byte [surf_xposition], 0
        jmp surf_pmode_print_character_done

surf_not_carriage_return:
        mov ecx, eax                    ; save character and attribute

        mov ebx, 0
        mov bl, [surf_xposition]
        shl bl, 1                       ; calculate x offset

        mov eax, 0
        mov al, [surf_yposition]
        mov edx, 160
        mul edx                         ; calculate y offset

        mov edi, 0xb8000                ; start of video memory; modified by us to 0x0(0xb8000)
        add edi, eax                    ; add y offset
        add edi, ebx                    ; add x offset

        mov ax, cx                      ; restore character and attribute
        cld                             ; forward direction
        stosw                           ; write character and attribute

        add byte [surf_xposition], 1


surf_pmode_print_character_done:
        popad                           ; restore registers
        ret

surf_pmode_print_string:
; input ds:esi = points to zero terminated string

        lodsb
        cmp al, 0
        jz surf_pmode_print_string_done
        mov ah, 0x0F                    ; white text, black background
        call surf_pmode_print_character
        jmp surf_pmode_print_string

surf_pmode_print_string_done:
        ret

surf_hardware_move_cursor:

        pushad                          ; save registers

        mov ebx, 0
        mov bl, [surf_xposition]             ; get x offset

        mov eax, 0
        mov al, [surf_yposition]
        mov edx, 80
        mul edx                         ; calculate y offset

        add ebx, eax                    ; calculate index

        ; select to write low byte of index
        mov al, 0xf
        mov dx, 0x03d4
        out dx, al

        ; write it
        mov al, bl
        mov dx, 0x03d5
        out dx, al

        ; select to write high byte of index
        mov al, 0xe
        mov dx, 0x03d4
        out dx, al

        ; write it
        mov al, bh
        mov dx, 0x03d5
        out dx, al

        popad                           ; restore registers

        ret

;----------------------------------------------------------------------------
; data
;----------------------------------------------------------------------------

    surf_xposition db 0
    surf_yposition db 5


        times 1024-($-$$) db 0           ; padding
