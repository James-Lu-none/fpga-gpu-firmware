/* =========================================================================
   RISC-V PicoRV32 Linker Script for FPGA-GPU Command Processor
   Target: 16KB Dual-Port On-Chip BRAM (0x0000_0000 ~ 0x0000_3FFF)
   ========================================================================= */

OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    BRAM (rw x) : ORIGIN = 0x00000000, LENGTH = 16384 /* 16KB BRAM */
}

SECTIONS {
    . = 0x00000000;

    .text : {
        *(.text.start)
        *(.text*)
        *(.rodata*)
    } > BRAM

    .data : {
        *(.data*)
        *(.sdata*)
    } > BRAM

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        __bss_end = .;
    } > BRAM

    /* Stack pointer initialization at top of user BRAM space (below Mailbox 0x3F00) */
    _stack_top = 0x00003EF0;
}
