#!/bin/sh
# Print the version of every tool in the SurfCPU toolchain image, one per line
# (copied into the image as surfcpu-versions; its output is /etc/surfcpu-toolchain.txt).
v() { printf '%-28s %s\n' "$1" "$(sh -c "$2" 2>&1 | head -n 1)"; }
v image-arch          'uname -m'
v debian              'cat /etc/debian_version'
v verilator           'verilator --version'
v iverilog            'iverilog -V 2>&1 | head -n 1'
v yosys               'yosys -V'
v sby                 'sby --help >/dev/null 2>&1 && echo present || echo missing'
v riscv-gcc           'riscv64-unknown-elf-gcc --version'
v riscv-binutils      'riscv64-unknown-elf-as --version'
v rv32im-libgcc       'riscv64-unknown-elf-gcc -march=rv32im_zicsr_zifencei -mabi=ilp32 -print-libgcc-file-name'
v rv32im-multilib     'riscv64-unknown-elf-gcc -march=rv32im_zicsr_zifencei -mabi=ilp32 -print-multi-directory'
v picolibc            'dpkg-query -W -f "\${Version}" picolibc-riscv64-unknown-elf'
v spike               'cat /opt/spike/COMMIT'
v qemu-system-riscv32 'qemu-system-riscv32 --version'
v python3             'python3 --version'
v mtools              'mtools --version'
v dosfstools          'mkfs.fat --help 2>&1 | head -n 1'
v z3                  'z3 --version'
v boolector           'boolector --version'
v make                'make --version'
