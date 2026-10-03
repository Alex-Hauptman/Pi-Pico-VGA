#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "vga_rgb.pio"
#include "font8x8.h"

#define WIDTH 320
#define HEIGHT 240
#define UART_ID uart0

#define HSYNC_GPIO 16
#define VSYNC_GPIO 17
#define RGB_BASE 18

// 320x240 Framebuffer
uint8_t framebuffer[HEIGHT][WIDTH];
uint8_t black_line[WIDTH] = {0}; // Blanking line payload

PIO pio = pio0;
uint sm_rgb = 0;
int dma_chan;

volatile int scanline = 0;

// Simple Ring Buffer for Command Queue
#define CMD_QUEUE_SIZE 16
#define CMD_BUF_SIZE 64
char command_queue[CMD_QUEUE_SIZE][CMD_BUF_SIZE];
volatile int cmd_head = 0;
volatile int cmd_tail = 0;
char cmd_buffer[CMD_BUF_SIZE];
volatile int cmd_pos = 0;

// VGA Timing constants for 640x480 @ 60Hz (Pixel clocked at 25MHz)
// Since our width is 320, each framebuffer pixel will span 2 clock cycles.
void hsync_pulse_start() { gpio_put(HSYNC_GPIO, 0); }
void hsync_pulse_end()   { gpio_put(HSYNC_GPIO, 1); }

// DMA Interrupt Handler: Executes at the start of every horizontal scanline
void __not_in_flash_func(vga_dma_handler)() {
    // Clear interrupt flag
    dma_hw->ints0 = 1u << dma_chan;

    scanline++;
    
    // Standard VGA matrix framing layout:
    // 0-479: Active lines (We stretch 240 lines to 480 by repeating each line twice)
    // 480-489: Front Porch (before sync pulse)
    // 490-491: VSYNC Pulse (Low active)
    // 492-524: Back Porch (after sync pulse)
    
    if (scanline >= 525) {
        scanline = 0;
    }

    if (scanline < 480) {
        // Active display area: map scanline to 0-239 framebuffer
        int fb_y = scanline / 2;
        dma_channel_set_read_addr(dma_chan, framebuffer[fb_y], true);
    } else {
        // Blanking interval: stream zeroed out black data
        dma_channel_set_read_addr(dma_chan, black_line, true);
    }

    // Generate strict precise inline HSYNC timing sequences
    hsync_pulse_start();
    busy_wait_us_32(4); // 3.8us HSYNC width
    hsync_pulse_end();

    // Handle VSYNC Timing cleanly
    if (scanline == 490) {
        gpio_put(VSYNC_GPIO, 0); // Start Vertical Sync
    } else if (scanline == 492) {
        gpio_put(VSYNC_GPIO, 1); // End Vertical Sync
    }
}

// Graphics API
static inline uint8_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6);
}

void put_pixel(int x, int y, uint8_t color) {
    if ((unsigned)x >= WIDTH || (unsigned)y >= HEIGHT) return;
    framebuffer[y][x] = color;
}

void fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= WIDTH || y >= HEIGHT) return;
    if (x + w > WIDTH) w = WIDTH - x;
    if (y + h > HEIGHT) h = HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        memset(&framebuffer[y + j][x], color, w);
    }
}

void clear_screen(uint8_t color) {
    memset(framebuffer, color, sizeof(framebuffer));
}

void draw_char(int x, int y, char c, uint8_t color) {
    const uint8_t *bitmap = NULL;
    if (c == ' ') bitmap = font8x8_letters[0];
    else if (c >= 'A' && c <= 'Z') bitmap = font8x8_letters[1 + (c - 'A')];
    else if (c >= 'a' && c <= 'z') bitmap = font8x8_letters[27 + (c - 'a')];
    else if (c >= '0' && c <= '9') bitmap = font8x8_numbers[c - '0'];
    else if (c >= 33 && c <= 64) bitmap = font8x8_symbols[c - 33];

    if (!bitmap) return; // Unsupported character (may be extended with more symbols later)

    for (int row = 0; row < 8; row++) {
        uint8_t bits = bitmap[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << (7 - col))) {
                put_pixel(x + col, y + row, color);
            }
        }
    }
}

void draw_text(int x, int y, char *text, uint8_t color) {
    while (*text) {
        draw_char(x, y, *text++, color);
        x += 8; // space apart for next character
    }
}

// Hardware Init
void vga_init() {
    gpio_init(HSYNC_GPIO);
    gpio_set_dir(HSYNC_GPIO, GPIO_OUT);
    gpio_put(HSYNC_GPIO, 1);

    gpio_init(VSYNC_GPIO);
    gpio_set_dir(VSYNC_GPIO, GPIO_OUT);
    gpio_put(VSYNC_GPIO, 1);

    uint offset = pio_add_program(pio, &vga_rgb_program);
    vga_rgb_program_init(pio, sm_rgb, offset, RGB_BASE);

    // System clock scaling setup: Standard 25MHz base for clean VGA delivery
    // 320 pixels across an active horizontal line requires a clock divider mapping 
    // to stretch the duration across the active timing width.
    float div = (float)clock_get_hz(clk_sys) / 25000000.0f;
    sm_config_set_clkdiv(&(pio->sm[sm_rgb].config), div);

    // Configure Direct Memory Access Engine
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dma_c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dma_c, DMA_SIZE_8);
    channel_config_set_read_increment(&dma_c, true);
    channel_config_set_write_increment(&dma_c, false);
    channel_config_set_dreq(&dma_c, pio_get_dreq(pio, sm_rgb, true));

    dma_channel_configure(
        dma_chan, &dma_c,
        &pio->txf[sm_rgb],
        framebuffer[0],
        WIDTH,
        false
    );

    irq_set_exclusive_handler(DMA_IRQ_0, vga_dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(dma_chan, true);

    dma_channel_start(dma_chan);
}

// Queue Processing
void enqueue_command(char *cmd) {
    int next = (cmd_head + 1) % CMD_QUEUE_SIZE;
    if (next != cmd_tail) {
        strncpy(command_queue[cmd_head], cmd, CMD_BUF_SIZE - 1);
        command_queue[cmd_head][CMD_BUF_SIZE - 1] = 0;
        cmd_head = next;
    }
}

void poll_uart() {
    while (uart_is_readable(UART_ID)) {
        char c = uart_getc(UART_ID);
        if (c == '\n' || c == '\r') {
            if (cmd_pos > 0) {
                cmd_buffer[cmd_pos] = 0;
                enqueue_command(cmd_buffer);
                cmd_pos = 0;
            }
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buffer[cmd_pos++] = c;
        } else {
            cmd_pos = 0; 
        }
    }
}

// Optimized string Tokenizer replacing slow sscanf calls
void execute_command(char *cmd) {
    char *token = strtok(cmd, " ");
    if (!token) return;

    if (strcmp(token, "PPX") == 0) {
        char *pX = strtok(NULL, " ");
        char *pY = strtok(NULL, " ");
        char *pC = strtok(NULL, " ");
        if (pX && pY && pC) {
            put_pixel(atoi(pX), atoi(pY), atoi(pC));
        }
    } else if (strcmp(token, "FRECT") == 0) {
        char *pX = strtok(NULL, " ");
        char *pY = strtok(NULL, " ");
        char *pW = strtok(NULL, " ");
        char *pH = strtok(NULL, " ");
        char *pC = strtok(NULL, " ");
        if (pX && pY && pW && pH && pC) {
            fill_rect(atoi(pX), atoi(pY), atoi(pW), atoi(pH), atoi(pC));
        }
    } else if (strcmp(token, "TEXT") == 0) {
        char *pX = strtok(NULL, " ");
        char *pY = strtok(NULL, " ");
        char *pC = strtok(NULL, " ");
        char *pText = strtok(NULL, "\n");
        if (pX && pY && pC && pText) {
            draw_text(atoi(pX), atoi(pY), pText, atoi(pC));
        }
    } else if (strcmp(token, "CLEAR") == 0) {
        char *pC = strtok(NULL, " ");
        if (pC) clear_screen(atoi(pC));
    }
}

void process_commands() {
    while (cmd_tail != cmd_head) {
        char *cmd = command_queue[cmd_tail];
        cmd_tail = (cmd_tail + 1) % CMD_QUEUE_SIZE;
        execute_command(cmd);
    }
}

int main() {
    stdio_init_all();

    uart_init(UART_ID, 115200);
    gpio_set_function(0, GPIO_FUNC_UART); // TX
    gpio_set_function(1, GPIO_FUNC_UART); // RX

    vga_init();
    clear_screen(rgb(0, 0, 255)); // Initialize with a solid Blue Screen

    while (1) {
        poll_uart();
        process_commands();
    }
}