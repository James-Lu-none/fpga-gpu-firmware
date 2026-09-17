#include <stdint.h>

#define KERNEL_STAGING_BASE 0x00010000UL // Dynamic Kernel Staging Buffer (16KB)
#define RING_BUFFER_BASE    0x00018000UL
#define IRQ_BASE            0x00020000UL // mapped to ctrl_axi
#define GPU_REGS_BASE       0x10000000UL // AXI-Lite GPU Hardware Engine
#define GPU_IRAM_BASE       0x10001000UL // GPU Instruction RAM (4KB)
#define UART_BASE           0x20000000UL // Simple AXI-Lite UART
#define HDMI_I2C_BASE       0x50000000UL

#define QUEUE_SIZE 512

#define FPGAGPU_MAGIC_OCL   0x4F434C31UL // "OCL1" (OpenCL Packet Magic)

typedef struct {
    uint32_t magic;         // 0x4F434C31 ("OCL1")
    uint32_t opcode;        // 0x01: Launch, 0x10: Load Kernel
    uint32_t grid_dim_x;    // Grid Dimension X
    uint32_t grid_dim_y;    // Grid Dimension Y
    uint32_t block_dim_x;   // Block Dimension X
    uint32_t block_dim_y;   // Block Dimension Y
    uint64_t dma_src_addr;  // PCIe Host DMA / Kernarg Base Address
    uint64_t dma_dst_addr;  // PCIe Host DMA Destination Address
    uint32_t num_elements;  // Vector Element Count or Instruction Count
    uint32_t task_id;       // Task ID
    uint32_t kernel_entry;  // Kernel Entry PC (default 0)
    uint32_t reserved[3];   // 12 bytes padding, exactly 64 bytes total
} __attribute__((packed, aligned(64))) fpgagpu_dispatch_packet_t;

typedef fpgagpu_dispatch_packet_t cuda_task_descriptor_t; // Legacy alias

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

// Set ENABLE_GPU_DEBUG to 1 to enable detailed GPU telemetry over UART
#define ENABLE_GPU_DEBUG 1

#ifdef ENABLE_GPU_DEBUG
#define REG_DEBUG_TBS        (*(volatile uint32_t*)(GPU_REGS_BASE + 0x30))
#define REG_DEBUG_SM         (*(volatile uint32_t*)(GPU_REGS_BASE + 0x34))
#define REG_DEBUG_WARP       (*(volatile uint32_t*)(GPU_REGS_BASE + 0x38))
#define REG_DEBUG_WARP_EXTRA (*(volatile uint32_t*)(GPU_REGS_BASE + 0x3C))
#define REG_DEBUG_LSU        (*(volatile uint32_t*)(GPU_REGS_BASE + 0x40))
#define REG_DEBUG_LSU_ADDR   (*(volatile uint32_t*)(GPU_REGS_BASE + 0x44))
#define REG_DEBUG_L1_L2      (*(volatile uint32_t*)(GPU_REGS_BASE + 0x48))
#endif

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

// Simple Delay Loop (~1ms per count at 125MHz)
void delay_ms(uint32_t count) {
    while (count--) {
        for (volatile uint32_t i = 0; i < 25000; i++) {
            __asm__ volatile ("nop");
        }
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
    volatile uint32_t *irq = (volatile uint32_t*)IRQ_BASE;
    *irq &= ~(1 << 0);
    uart_print("[IRQ] Interrupt Received.\n");
}

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t reserved[14]; // 56 bytes padding to align cmds to 64 bytes
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

    // 2. Initialize GPU I-RAM with default OP_EXIT (0xFF000000) so unprogrammed kernels exit safely
    volatile uint32_t *iram = (volatile uint32_t *)GPU_IRAM_BASE;
    for (int i = 0; i < 256; i++) {
        iram[i] = 0xFF000000;
    }
    uart_print("[Main] Initialized GPU I-RAM.\n");

    // 3. Command Processor Main Polling Loop
    while (1) {
        uint32_t head = ring->head;
        uint32_t tail = ring->tail;

        // If Host reset the queue (e.g. driver reloaded or queue reset), resynchronize
        if (head == 0 && tail == 0 && local_head != 0) {
            local_head = 0;
        }

        // Check if there are new tasks from the Host
        if (local_head != tail) {
            cuda_task_descriptor_t task = ring->cmds[local_head];

            if (task.magic == FPGAGPU_MAGIC_OCL) {
                
                if (task.opcode == 0x10) { // VGPU_OPCODE_LOAD_KERNEL
                    volatile uint32_t *staging = (volatile uint32_t *)KERNEL_STAGING_BASE;
                    uint32_t count = task.num_elements;
                    if (count > 1024) count = 1024;
                    for (uint32_t k = 0; k < count; k++) {
                        iram[k] = staging[k];
                    }
                    for (uint32_t k = count; k < 1024; k++) {
                        iram[k] = 0xFF000000; // Pad remaining with EXIT
                    }
                    uart_print("[Main] Loaded Dynamic Kernel into I-RAM.\n");
                } else if (task.opcode == 1) { // VGPU_OPCODE_LAUNCH_KERNEL

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

                    // Wait for GPU Compute to Finish
                    uint32_t wait_loop = 0;
                    while (REG_INT_STATUS == 0) {
                        wait_loop++;
#ifdef ENABLE_GPU_DEBUG
                        if (wait_loop >= 1000000) {
                            uart_print("[GPU_DBG] TBS:0x");
                            uart_print_hex(REG_DEBUG_TBS);
                            uart_print(" SM:0x");
                            uart_print_hex(REG_DEBUG_SM);
                            uart_print(" W01:0x");
                            uart_print_hex(REG_DEBUG_WARP);
                            uart_print(" WEX:0x");
                            uart_print_hex(REG_DEBUG_WARP_EXTRA);
                            uart_print(" LSU:0x");
                            uart_print_hex(REG_DEBUG_LSU);
                            uart_print(" ADDR:0x");
                            uart_print_hex(REG_DEBUG_LSU_ADDR);
                            uart_print(" L1L2:0x");
                            uart_print_hex(REG_DEBUG_L1_L2);
                            uart_print("\n");
                            wait_loop = 0;
                        }
#else
                        if (wait_loop >= 2000000) {
                            uart_print("[Main] Waiting for GPU REG_INT_STATUS...\n");
                            wait_loop = 0;
                        }
#endif
                    }

                    // Acknowledge the GPU internal interrupt
                    REG_INT_ACK = 1;
                    uart_print("[Main] GPU Task Complete.\n");
                }
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
            // No new tasks, print heartbeat without blocking the polling loop
            static uint32_t idle_cnt = 0;
            if (++idle_cnt >= 2000000) {
                uart_print("[Main] RISC-V Heartbeat\n");
                idle_cnt = 0;
            }
        }
    }

    return 0;
}
