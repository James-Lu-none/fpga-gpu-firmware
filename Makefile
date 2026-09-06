# =========================================================================
# RISC-V Command Processor Firmware Makefile
# Cross-compiles for RV32I Architecture & Generates PCIe Binaries
# =========================================================================

CROSS_COMPILE ?= riscv64-unknown-elf-
CC             = $(CROSS_COMPILE)gcc
OBJCOPY        = $(CROSS_COMPILE)objcopy
OBJDUMP        = $(CROSS_COMPILE)objdump
HOSTCC         = gcc

# Changed optimization to -Os to prevent bloated/unsupported instructions
CFLAGS         = -march=rv32i -mabi=ilp32 -Os -Wall -nostdlib -ffreestanding -Iinclude
LDFLAGS        = -T sections.lds -nostdlib

TARGET         = firmware
SRCS_C         = main.c
SRCS_ASM       = start.S
OBJS           = $(SRCS_ASM:.S=.o) $(SRCS_C:.c=.o)

all: $(TARGET).bin $(TARGET).hex $(TARGET).lst load_fw

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) sections.lds
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# Uses objcopy for safe Verilog hex layout instead of host-dependent hexdump
$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O verilog --verilog-data-width=4 $< $@

$(TARGET).lst: $(TARGET).elf
	$(OBJDUMP) -d $< > $@

load_fw: load_fw.c
	$(HOSTCC) -Wall -O2 $< -o $@

clean:
	rm -f *.o *.elf *.bin *.hex *.lst load_fw

.PHONY: all flash clean
