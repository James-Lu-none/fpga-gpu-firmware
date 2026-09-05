#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define BRAM_USABLE_SIZE 0x1FFFF // 128 KB
#define BRAM_CPU_RESET_OFFSET 0x1FFF0
#define BRAM_IRQ_OFFSET       0x1FFE0

int main(int argc, char **argv) {
    const char *fw_path = "firmware.bin";
    uint32_t bar0_addr = 0xa0a10000; // BAR0 Physical Address

    // 1. Read firmware.bin
    FILE *f = fopen(fw_path, "rb");
    if (!f) {
        perror("fopen firmware file");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long fw_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fw_size > BRAM_USABLE_SIZE) {
        printf("Error: Firmware size (%ld bytes) exceeds usable BRAM size (0x%X bytes)!\n", fw_size, BRAM_USABLE_SIZE);
        fclose(f);
        return 1;
    }

    uint8_t *fw_buf = malloc(fw_size + 4);
    if (!fw_buf) {
        perror("malloc");
        fclose(f);
        return 1;
    }
    memset(fw_buf, 0, fw_size + 4);
    if (fread(fw_buf, 1, fw_size, f) != fw_size) {
        perror("fread");
        fclose(f);
        free(fw_buf);
        return 1;
    }
    fclose(f);

    printf("Loaded %s (%ld bytes).\n", fw_path, fw_size);
    printf("Preparing to write to FPGA BRAM at Physical Address: 0x%08X...\n", bar0_addr);

    // 2. Map PCIe BAR0 via /dev/mem
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("open /dev/mem failed (did you forget sudo?)");
        free(fw_buf);
        return 1;
    }

    size_t map_size = 0x20000; // 128KB PCIe BAR size
    off_t target = bar0_addr;
    size_t page_size = sysconf(_SC_PAGE_SIZE);
    void *map_base = mmap(0, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, target & ~(page_size - 1));
    if (map_base == (void *) -1) {
        perror("mmap failed");
        close(mem_fd);
        free(fw_buf);
        return 1;
    }

    void *virt_addr = (uint8_t *)map_base + (target & (page_size - 1));

    // 3. Assert CPU Soft Reset before loading firmware
    volatile uint32_t *cpu_reset_reg = (volatile uint32_t *)((uint8_t *)virt_addr + BRAM_CPU_RESET_OFFSET);
    *cpu_reset_reg = 0;
    __asm__ volatile("mfence" ::: "memory");
    printf("Asserted CPU soft reset.\n");
    sleep(1);

    // 4. Clear BRAM to 0x00
    // We must use 32-bit single writes with mfence because XDMA AXI-Lite doesn't support burst!
    printf("Clearing 128KB BRAM to 0x00...\n");
    volatile uint32_t *bram_words = (volatile uint32_t *)virt_addr;
    size_t total_words = (BRAM_USABLE_SIZE + 1) / 4;
    for (size_t i = 0; i < total_words; i++) {
        bram_words[i] = 0;
        __asm__ volatile("mfence" ::: "memory");
        (void)bram_words[i];
    }
    printf("BRAM cleared.\n");

    // 5. Write Firmware to BRAM
    // Write as 32-bit words
    uint32_t *fw_words = (uint32_t *)fw_buf;
    size_t num_words = (fw_size + 3) / 4; 

    for (size_t i = 0; i < num_words; i++) {
        bram_words[i] = fw_words[i];
        
        /* 
         * [CRITICAL FIX]
         * x86 CPUs aggressively use Write-Combining (WC) for memory mapped I/O,
         * merging sequential 32-bit writes into 16-byte or 64-byte PCIe Burst TLPs.
         * The Xilinx XDMA AXI-Lite interface does not support address-incrementing 
         * for PCIe burst writes, causing all merged writes to overwrite the same base address!
         * 
         * We use an `mfence` followed by a dummy read to force the CPU to flush the 
         * WC buffer and issue strictly single 32-bit PCIe TLPs.
         */
        __asm__ volatile("mfence" ::: "memory");
        (void)bram_words[i];
    }

    printf("Successfully wrote firmware to BRAM.\n");

    // 4. Byte-by-Byte Verification
    volatile uint8_t *bram_bytes = (volatile uint8_t *)virt_addr;
    int mismatch = 0;
    int matches = 0;
    for (size_t i = 0; i < fw_size; i++) {
        // dummy read to get the data out
        uint8_t val = bram_bytes[i];
        if (val != fw_buf[i]) {
            printf("Verification failed at byte offset 0x%zx: expected 0x%02X, got 0x%02X\n", i, fw_buf[i], val);
            mismatch++;
        }
        else {
            matches++;
        }
    }


    if (!mismatch) {
        printf("Byte-by-byte verification passed! %d/%ld bytes matched.\n", matches, fw_size);
        sleep(1);
        // Release cpu_soft_rst_n to boot firmware
        *cpu_reset_reg = 1;
        __asm__ volatile("mfence" ::: "memory");
        printf("CPU soft reset released. Firmware is booting...\n");
        sleep(1);
        

        volatile uint32_t *irq_reg = (volatile uint32_t *)((uint8_t *)virt_addr + BRAM_IRQ_OFFSET);
        printf("\nStarting PicoRV32 Liveness Test...\n");
        
        for (int i = 0; i < 3; i++) {
            printf("Host: Sending IRQ ping %d...\n", i);
            *irq_reg = 1; // Trigger AXI IRQ Sniffer and set bit 0 in BRAM
            __asm__ volatile("mfence" ::: "memory");
            
            int timeout = 100;
            while ((*irq_reg & 1) != 0 && timeout > 0) {
                usleep(10000);
                timeout--;
            }
            
            if (timeout == 0) {
                printf("TIMEOUT! CPU did not respond to IRQ %d.\n", i);
                break;
            } else {
                printf("SUCCESS! CPU received IRQ and cleared the flag!\n");
            }
            sleep(1);
        }
        printf("--- Liveness Test Finished ---\n");
    } else {
        printf("Byte-by-byte verification failed! %d/%ld bytes matched, %d bytes mismatched.\n", matches, fw_size, mismatch);
    }

    // Cleanup
    munmap(map_base, map_size);
    close(mem_fd);
    free(fw_buf);

    return mismatch ? 1 : 0;
}
