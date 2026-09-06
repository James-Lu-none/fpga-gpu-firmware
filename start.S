# =========================================================================
# PicoRV32 RISC-V Startup Assembly & Hardware IRQ Vector Entry
# Target: RISC-V RV32I Architecture
# Uses official picorv32 custom_ops.S instruction macros
# =========================================================================

#include "custom_ops.S"

.section .text.start
.global _start
.global irq_entry

_start:
    # 1. Reset Vector (Address 0x0000_0000)
    j real_start
    nop
    nop
    nop

    # 2. Interrupt Vector Entry Point (Address 0x0000_0010 - PROGADDR_IRQ)
.org 0x00000010
irq_entry:
    # Save CPU Registers to Stack
    addi sp, sp, -64
    sw   x1,  0(sp)
    sw   x5,  4(sp)
    sw   x6,  8(sp)
    sw   x7, 12(sp)
    sw   x10, 16(sp)
    sw   x11, 20(sp)
    sw   x12, 24(sp)
    sw   x13, 28(sp)
    sw   x14, 32(sp)
    sw   x15, 36(sp)
    sw   x16, 40(sp)
    sw   x17, 44(sp)

    # Call C Hardware IRQ Handler
    jal  irq_handler

    # Restore CPU Registers from Stack
    lw   x1,  0(sp)
    lw   x5,  4(sp)
    lw   x6,  8(sp)
    lw   x7, 12(sp)
    lw   x10, 16(sp)
    lw   x11, 20(sp)
    lw   x12, 24(sp)
    lw   x13, 28(sp)
    lw   x14, 32(sp)
    lw   x15, 36(sp)
    lw   x16, 40(sp)
    lw   x17, 44(sp)
    addi sp, sp, 64

    # PicoRV32 retirq official custom instruction macro
    picorv32_retirq_insn()

real_start:
    # Initialize Stack Pointer
    la   sp, _stack_top

    # Unmask Hardware Interrupt 0 (irq[0] - Mailbox PCIe Doorbell)
    # PicoRV32 official custom instruction: qR = maskirq(qA) -> set bit 0 to 0 (enable irq0)
    li   t0, 0xFFFFFFFE
    picorv32_maskirq_insn(t0, t0)

    # Jump to C main()
    jal  main

    # Dead loop if main returns
1:  j    1b
