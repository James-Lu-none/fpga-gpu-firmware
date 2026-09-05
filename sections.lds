/* =========================================================================
   RISC-V PicoRV32 Linker Script for FPGA-GPU Command Processor
   Target: 64KB Dual-Port On-Chip BRAM (0x0000_0000 ~ 0x000_FFFF)
   ========================================================================= */

OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    BRAM (rw x) : ORIGIN = 0x00000000, LENGTH = 65536 /* 64KB BRAM Firmware Region */
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

    /* Stack pointer initialization at top of 64KB Firmware Region */
    _stack_top = 0x00010000;
}
