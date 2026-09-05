#include <stdint.h>

// Address Bases (Must match rtl/common/address.vh)
#define BRAM_RING_BUFFER_BASE 0x0001E000UL
#define BRAM_CPU_RESET_BASE   0x0001FFF0UL
#define GPU_REGS_BASE   0x10000000UL // AXI-Lite GPU Hardware Engine
#define GPU_IRAM_BASE   0x10001000UL // GPU Instruction RAM (4KB)
#define UART_BASE       0x20000000UL // Simple AXI-Lite UART
#define HDMI_I2C_BASE   0x50000000UL

#define QUEUE_SIZE 512

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
    uart_print("[IRQ] Unexpected Interrupt Received. Ignored.\n");
}

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    cuda_task_descriptor_t cmds[QUEUE_SIZE];
} vgpu_ring_buffer_t;

int main(void) {
    // 1. Initialize SiI9134 HDMI Display Chip Configuration
    // init_hdmi_sii9134(); // [WARNING] 0x50000000 is not mapped in AXI Crossbar yet! This will cause a Bus Error (DECERR) and trap the CPU!

    uart_print("\nHello from RISC-V (Non-Blocking Ring Buffer Mode)\n");

    /*
     * Asynchronous Ring Buffer (Command Queue)
     * We map the Ring Buffer directly to our BRAM space. The Host PC updates 
     * the 'tail' pointer when it adds new tasks. We (PicoRV32) update the 'head' 
     * pointer when we finish them.
     */
    volatile vgpu_ring_buffer_t *ring = (volatile vgpu_ring_buffer_t *)RING_BUFFER_BASE;
    uint32_t local_head = ring->head;

    // 2. Command Processor Main Polling Loop
    while (1) {
        uint32_t tail = ring->tail;

        // Check if there are new tasks from the Host
        if (local_head != tail) {
            cuda_task_descriptor_t task = ring->cmds[local_head];

            if (task.magic == 0x43554441) { // Check "CUDA" Magic
                
                // Dispatch Grid & Block Dimensions to Hardware Warp Scheduler
                REG_GRID_DIM_X  = task.grid_dim_x;
                REG_GRID_DIM_Y  = task.grid_dim_y;
                REG_BLOCK_DIM_X = task.block_dim_x;
                REG_BLOCK_DIM_Y = task.block_dim_y;
                REG_SRC_ADDR    = (uint32_t)task.dma_src_addr;
                REG_DST_ADDR    = (uint32_t)task.dma_dst_addr;

                uart_print("[Main] Dispatched Task!\n");

                // Trigger Hardware Warp Launch Doorbell
                REG_DOORBELL = 1;

                /* 
                 * Wait for GPU Compute to Finish
                 * Although this is a busy-wait for the GPU engine, it no longer blocks
                 * the Host CPU! The Host can queue up to 4 tasks in the Ring Buffer while 
                 * we are waiting here. Once we get multi-engine GPU hardware, we can 
                 * dispatch without blocking here too.
                 */
                while (REG_INT_STATUS == 0) {
                    __asm__ volatile ("nop");
                }

                // Acknowledge the GPU internal interrupt
                REG_INT_ACK = 1;
                uart_print("[Main] GPU Task Complete.\n");
            }

            // Move head forward to consume the task
            local_head = (local_head + 1) % QUEUE_SIZE;
            
            /*
             * Write back to BRAM Ring Buffer
             * This tells the Host CPU that we have finished the task.
             * The Host CPU's VGPU_IOC_DOORBELL loop is polling this value!
             */
            ring->head = local_head;
        } else {
            // No new tasks, print a heartbeat every ~1 second so we can see UART working
            uart_print("RISC-V Heartbeat...\r\n");
            delay_ms(1000);
        }
    }

    return 0;
}
