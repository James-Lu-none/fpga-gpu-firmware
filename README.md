# FPGA-GPU RISC-V Command Processor (CP) Firmware

This repository contains the standalone RISC-V C/Assembly firmware for the **PicoRV32 Command Processor SoC** on the FPGA GPGPU Accelerator.

## Memory Map

- **`0x0000_0000 ~ 0x0000_3EFF`**: Instruction & Data BRAM (16KB Dual-Port)
- **`0x0000_3F00 ~ 0x0000_3FFF`**: Direct PCIe BAR0 Mailbox Window (64-byte `cuda_task_descriptor_t`)
- **`0x4000_0000`**: GPU Compute Core & Hardware Warp Scheduler MMIO Registers
- **`0x5000_0000`**: SiI9134 HDMI Controller Hardware I2C Register

## Building the Firmware

To cross-compile the RISC-V binary on Linux:

```bash
make
```

Outputs generated:
- `firmware.elf`: Executable Linked Image
- `firmware.bin`: Raw Binary Image (for PCIe Dynamic Flashing)
- `firmware.hex`: Verilog `$readmemh` Hex File
- `firmware.lst`: RISC-V Disassembly listing

## Flashing over PCIe (Dynamic Thermal Upload)

To upload the compiled firmware dynamically to the FPGA over PCIe without re-synthesizing Bitstream:

```bash
make flash
```

Or manually via `dd`:

```bash
sudo dd if=firmware.bin of=/dev/xdma0_user bs=4096 seek=0 status=progress conv=notrunc
```
