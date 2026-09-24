#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// One 4 KB flash sector reserved for the persistent network configuration.
// It must sit above the program image (__flash_binary_end) and below the top
// of flash. The app image is well under 1 MB, so 0x1F0000 is safe on the 2 MB
// Pico W flash.
#define NET_CONFIG_FLASH_OFFSET 0x1F0000u
#define NET_CONFIG_MAGIC        0xC0FFEE01u

#define NET_CONFIG_SSID_MAX     33
#define NET_CONFIG_PASSWORD_MAX 65
#define NET_CONFIG_IP_MAX       16

typedef struct {
    uint32_t magic;
    char     ssid[NET_CONFIG_SSID_MAX];
    char     password[NET_CONFIG_PASSWORD_MAX];
    char     bridge_ip[NET_CONFIG_IP_MAX];
    uint16_t bridge_port;
    uint32_t crc;
} wifi_config_t;

// Load the saved configuration from flash. Returns false when absent/invalid.
bool config_load(wifi_config_t *out);

// Persist the configuration to flash. Returns true on success.
bool config_save(const wifi_config_t *cfg);

// Callback invoked while setup mode is idle, used to animate the display.
typedef void (*net_config_display_fn)(void);

// USB-serial setup: wait for a host to send credentials, save them, then
// return so the caller can connect with them immediately. Fills *out.
// display_tick is called periodically while idle (may be NULL).
void config_setup_mode(wifi_config_t *out, net_config_display_fn display_tick);

#endif
