#include <stdint.h>

// MMIO Address Base Registers (Must match rtl/gpu_memory_map.vh)
#define GPU_REGS_BASE   0x10000000UL // AXI-Lite GPU Hardware Engine
#define GPU_IRAM_BASE   0x10001000UL // GPU Instruction RAM (4KB)
#define HDMI_I2C_BASE   0x50000000UL
#define MAILBOX_BASE    0x00003F00UL // Shared PCIe Direct BRAM Mailbox
#define UART_BASE       0x20000000UL // Simple AXI-Lite UART

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
#define REG_SRC_ADDR    (*(volatile uint32_t*)(GPU_REGS_BASE + 0x20))
#define REG_DST_ADDR    (*(volatile uint32_t*)(GPU_REGS_BASE + 0x24))

// UART Helper Functions
#define UART_RX         (*(volatile uint32_t*)(UART_BASE + 0x00))
#define UART_TX         (*(volatile uint32_t*)(UART_BASE + 0x04))
#define UART_STATUS     (*(volatile uint32_t*)(UART_BASE + 0x08))
#define UART_CTRL       (*(volatile uint32_t*)(UART_BASE + 0x0C))

void uart_putc(char c) {
    // Wait until TX FIFO is not full (Bit 3 of status is 0)
    while (UART_STATUS & 0x08) {
        __asm__ volatile ("nop");
    }
    UART_TX = c;
}

void uart_print(const char *str) {
    while (*str) {
        if (*str == '\n') uart_putc('\r');
        uart_putc(*str++);
    }
}

void uart_print_hex(uint32_t val) {
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(hex_chars[(val >> i) & 0xF]);
    }
}

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

/*
 * Custom memcpy for Baremetal RISC-V Firmware
 * The GCC compiler often implicitly calls memcpy for struct assignments 
 * (like task = ring->cmds[local_head]). Since we compile with -nostdlib, 
 * we must provide our own basic implementation.
 */
void *memcpy(void *dest, const void *src, unsigned int n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void irq_handler(void) {
    volatile cuda_task_descriptor_t *task = (volatile cuda_task_descriptor_t*)MAILBOX_BASE;

    if (task->magic == 0x43554441) { // Check "CUDA" Magic
        // 1. Dispatch Grid & Block Dimensions to Hardware Warp Scheduler
        REG_GRID_DIM_X  = task->grid_dim_x;
        REG_GRID_DIM_Y  = task->grid_dim_y;
        REG_BLOCK_DIM_X = task->block_dim_x;
        REG_BLOCK_DIM_Y = task->block_dim_y;
        REG_SRC_ADDR    = (uint32_t)task->dma_src_addr;
        REG_DST_ADDR    = (uint32_t)task->dma_dst_addr;

        uart_print("[IRQ] Received CUDA Task!\n");
        uart_print("      Opcode: "); uart_print_hex(task->opcode); uart_print("\n");
        uart_print("      Grid:   "); uart_print_hex(task->grid_dim_x); uart_print("x"); uart_print_hex(task->grid_dim_y); uart_print("\n");
        uart_print("      Block:  "); uart_print_hex(task->block_dim_x); uart_print("x"); uart_print_hex(task->block_dim_y); uart_print("\n");

        // 2. Trigger Hardware Warp Launch Doorbell
        REG_DOORBELL = 1;

        // 3. Wait for GPU Compute to Finish
        while (REG_INT_STATUS == 0) {
            __asm__ volatile ("nop");
        }

        // 4. Acknowledge the GPU internal interrupt
        REG_INT_ACK = 1;

        // 5. Mark Task Done in Mailbox
        task->num_elements = 0x2; // Task Done Status Code
        
        uart_print("[IRQ] Task Complete. Notifying Host...\n");

        // 6. Trigger Host PCIe Interrupt (usr_irq_req) via CPU Trap
        // Note: The host must reset the RISC-V core before sending the next task.
        __builtin_trap(); 
    }
}

int main(void) {
    // 1. Initialize SiI9134 HDMI Display Chip Configuration
    init_hdmi_sii9134();

    uart_print("\nHello from RISC-V\n");

    // 2. Command Processor Idle Loop (Hardware IRQ Driven)
    while (1) {
        __asm__ volatile ("wfi"); // Wait For Interrupt
    }

    return 0;
}
