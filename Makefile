# =========================================================================
# RISC-V Command Processor Firmware Makefile
# Cross-compiles for RV32I Architecture & Generates PCIe Binaries
# =========================================================================

CROSS_COMPILE ?= riscv64-unknown-elf-
CC             = $(CROSS_COMPILE)gcc
OBJCOPY        = $(CROSS_COMPILE)objcopy
OBJDUMP        = $(CROSS_COMPILE)objdump

CFLAGS         = -march=rv32i -mabi=ilp32 -O2 -Wall -nostdlib -ffreestanding -Iinclude
LDFLAGS        = -T sections.lds -nostdlib

TARGET         = firmware
SRCS_C         = main.c
SRCS_ASM       = start.s
OBJS           = $(SRCS_ASM:.s=.o) $(SRCS_C:.c=.o)

all: $(TARGET).bin $(TARGET).hex $(TARGET).lst

%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) sections.lds
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET).hex: $(TARGET).bin
	hexdump -v -e '1/4 "%08x\n"' $< > $@

$(TARGET).lst: $(TARGET).elf
	$(OBJDUMP) -d $< > $@

flash: $(TARGET).bin
	@echo "Flashing firmware to FPGA via PCIe XDMA BAR0 Mailbox (0x0000)..."
	sudo dd if=$(TARGET).bin of=/dev/xdma0_user bs=4096 seek=0 status=progress conv=notrunc

clean:
	rm -f *.o *.elf *.bin *.hex *.lst

.PHONY: all flash clean
