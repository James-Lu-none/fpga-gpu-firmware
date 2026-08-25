/* =========================================================================
   RISC-V Command Processor (CP) Firmware for FPGA-GPU
   Direct PCIe BRAM Mailbox & Hardware Warp Scheduler Interface
   ========================================================================= */

#include <stdint.h>

// MMIO Address Base Registers
#define GPU_REGS_BASE   0x40000000UL
#define HDMI_I2C_BASE   0x50000000UL
#define MAILBOX_BASE    0x00003F00UL // Shared PCIe Direct BRAM Mailbox (256-byte window)

// CUDA Task Descriptor Structure (64-Byte Aligned)
typedef struct {
    uint32_t magic;         // 0x43554441 ("CUDA")
    uint32_t opcode;        // 1: Add, 2: Mul, 3: Render, 4: SMEM_Write, 5: SMEM_Accumulate
    uint32_t grid_dim_x;    // Grid Dimension X
    uint32_t grid_dim_y;    // Grid Dimension Y
    uint32_t block_dim_x;   // Block Dimension X
    uint32_t block_dim_y;   // Block Dimension Y
    uint64_t dma_src_addr;  // PCIe Host DMA Source Address
    uint64_t dma_dst_addr;  // PCIe Host DMA Destination Address
    uint32_t num_elements;  // Vector Element Count
    uint32_t reserved[7];   // Padding to 64 bytes
} cuda_task_descriptor_t;

// GPU Slave Register & Warp Scheduler Offsets
#define REG_DOORBELL    (*(volatile uint32_t*)(GPU_REGS_BASE + 0x00))
#define REG_INT_STATUS  (*(volatile uint32_t*)(GPU_REGS_BASE + 0x04))
#define REG_INT_ACK     (*(volatile uint32_t*)(GPU_REGS_BASE + 0x08))
#define REG_GRID_DIM_X  (*(volatile uint32_t*)(GPU_REGS_BASE + 0x0C))
#define REG_GRID_DIM_Y  (*(volatile uint32_t*)(GPU_REGS_BASE + 0x10))
#define REG_BLOCK_DIM_X (*(volatile uint32_t*)(GPU_REGS_BASE + 0x14))
#define REG_BLOCK_DIM_Y (*(volatile uint32_t*)(GPU_REGS_BASE + 0x18))
#define REG_OPCODE      (*(volatile uint32_t*)(GPU_REGS_BASE + 0x1C))

// Simple Delay Loop
void delay_ms(uint32_t count) {
    for (volatile uint32_t i = 0; i < count * 2000; i++) {
        __asm__ volatile ("nop");
    }
}

// SiI9134 Hardware I2C Register Helper
void hdmi_i2c_write(uint8_t reg_addr, uint8_t reg_val) {
    volatile uint32_t *i2c_ctrl = (volatile uint32_t*)(HDMI_I2C_BASE);
    *i2c_ctrl = ((uint32_t)reg_addr << 8) | reg_val;
}

// Initialize SiI9134 HDMI Controller
void init_hdmi_sii9134(void) {
    hdmi_i2c_write(0x08, 0x35); // Power Normal
    delay_ms(10);
    hdmi_i2c_write(0x05, 0x00); // System Reset
    hdmi_i2c_write(0x09, 0x00); // Input Video: RGB444 24-bit
    hdmi_i2c_write(0x0A, 0x00); // Auto Video Mode
    hdmi_i2c_write(0x3C, 0x01); // HDMI Output Enable
}

// Hardware Interrupt Handler (Triggered by Host PCIe Write to Mailbox 0x3F00)
void irq_handler(void) {
    volatile cuda_task_descriptor_t *task = (volatile cuda_task_descriptor_t*)MAILBOX_BASE;

    if (task->magic == 0x43554441) { // Check "CUDA" Magic
        // 1. Dispatch Grid & Block Dimensions to Hardware Warp Scheduler
        REG_GRID_DIM_X  = task->grid_dim_x;
        REG_GRID_DIM_Y  = task->grid_dim_y;
        REG_BLOCK_DIM_X = task->block_dim_x;
        REG_BLOCK_DIM_Y = task->block_dim_y;
        REG_OPCODE      = task->opcode;

        // 2. Trigger Hardware Warp Launch Doorbell
        REG_DOORBELL = 1;

        // 3. Mark Task Done in Mailbox
        task->num_elements = 0x2; // Task Done Status Code
    }
}

// RISC-V Main Execution Entry
int main(void) {
    // 1. Initialize SiI9134 HDMI Display Chip Configuration
    init_hdmi_sii9134();

    // 2. Command Processor Idle Loop (Hardware IRQ Driven)
    while (1) {
        __asm__ volatile ("wfi"); // Wait For Interrupt
    }

    return 0;
}
