#include "net_config.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"

#define CONFIG_FLASH_ADDR (XIP_BASE + NET_CONFIG_FLASH_OFFSET)

// CRC32 (reflected, polynomial 0xEDB88320) over the config payload fields.
static uint32_t config_crc(const wifi_config_t *cfg) {
    const uint8_t *p = (const uint8_t *)cfg + offsetof(wifi_config_t, ssid);
    size_t n = offsetof(wifi_config_t, crc) - offsetof(wifi_config_t, ssid);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

bool config_load(wifi_config_t *out) {
    const wifi_config_t *stored = (const wifi_config_t *)CONFIG_FLASH_ADDR;
    if (stored->magic != NET_CONFIG_MAGIC) return false;
    if (stored->crc != config_crc(stored)) return false;
    if (stored->ssid[0] == '\0' || stored->bridge_ip[0] == '\0') return false;
    if (stored->bridge_port == 0) return false;
    memcpy(out, stored, sizeof(*out));
    return true;
}

static void __no_inline_not_in_flash_func(config_do_write)(void *param) {
    const uint8_t *page = (const uint8_t *)param;
    flash_range_erase(NET_CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(NET_CONFIG_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
}

bool config_save(const wifi_config_t *cfg) {
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page, 0xFF, sizeof(page));
    wifi_config_t tmp = *cfg;
    tmp.magic = NET_CONFIG_MAGIC;
    tmp.crc = config_crc(&tmp);
    memcpy(page, &tmp, sizeof(tmp));
    return flash_safe_execute(config_do_write, page, UINT32_MAX) == PICO_OK;
}

// Read one newline-terminated line from USB serial (newline stripped).
// Returns the line length, or -1 on timeout.
static int read_line(char *buf, size_t cap, uint32_t timeout_ms) {
    size_t n = 0;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        int c = getchar_timeout_us(10000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (c == '\r' || c == '\n') {
            if (n == 0) continue;
            buf[n] = '\0';
            return (int)n;
        }
        if (n < cap - 1) buf[n++] = (char)c;
    }
    return -1;
}

// Host protocol: "CFG\n" -> "READY\n", then one tab-delimited line
// "SSID\tPASSWORD\tBRIDGE_IP\tPORT\n" -> "OK\n" or "ERR <reason>\n".
// On success the config is saved and the function returns so main() can
// connect with it immediately (no reboot - the host needs the "OK" ack).
// While idle, display_tick (if non-NULL) is called to animate the screen.
void config_setup_mode(wifi_config_t *out, net_config_display_fn display_tick) {
    char line[192];
    int banner_ticks = 0;
    printf("\nPICO-SETUP-READY\n");

    while (true) {
        if (read_line(line, sizeof(line), 500) < 0) {
            if (display_tick) display_tick();
            if (++banner_ticks >= 4) {   // re-advertise roughly every 2 s
                banner_ticks = 0;
                printf("PICO-SETUP-READY\n");
            }
            continue;
        }
        if (strcmp(line, "CFG") != 0) continue;

        printf("READY\n");
        if (read_line(line, sizeof(line), 30000) < 0) continue;

        char *ssid = line;
        char *pass = strchr(ssid, '\t');
        if (!pass) { printf("ERR format\n"); continue; }
        *pass++ = '\0';
        char *ip = strchr(pass, '\t');
        if (!ip) { printf("ERR format\n"); continue; }
        *ip++ = '\0';
        char *port = strchr(ip, '\t');
        if (!port) { printf("ERR format\n"); continue; }
        *port++ = '\0';

        int p = atoi(port);
        if (ssid[0] == '\0' || ip[0] == '\0' || p <= 0 || p > 65535) {
            printf("ERR invalid\n");
            continue;
        }

        wifi_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
        strncpy(cfg.password, pass, sizeof(cfg.password) - 1);
        strncpy(cfg.bridge_ip, ip, sizeof(cfg.bridge_ip) - 1);
        cfg.bridge_port = (uint16_t)p;

        if (!config_save(&cfg)) {
            printf("ERR flash\n");
            continue;
        }

        memcpy(out, &cfg, sizeof(cfg));
        printf("OK\n");
        stdio_flush();
        return;
    }
}
