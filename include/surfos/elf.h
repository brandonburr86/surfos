/*
SurfOS ELF32
----------------------
File: elf.h     Date: 10/3/26 (roadmap P2)
----------------------
Just enough of the ELF format to load a static i386 executable.
*/

#ifndef _SURFOS_ELF_H
#define _SURFOS_ELF_H

#include <surfos/types.h>

#define ELF_MAGIC "\177ELF"
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

struct elf32_ehdr {
    u8 e_ident[16];
    u16 e_type, e_machine;
    u32 e_version, e_entry, e_phoff, e_shoff, e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf32_phdr {
    u32 p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};

#endif
