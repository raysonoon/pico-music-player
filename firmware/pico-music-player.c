#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"

#include "network_config.h"

#include "OLED_1in3_c.h"
#include "GUI_Paint.h"

// Push button on GPIO15 (active-low: pressed reads 0)
#define BUTTON_PIN            15
#define DEBOUNCE_MS           25
#define LONG_PRESS_MS         1000
#define MULTI_TAP_WINDOW_MS   500

typedef struct http_state {
    struct tcp_pcb *pcb;
    ip_addr_t remote_addr;
    const char *request;
    size_t request_len;
    volatile bool done;
    volatile bool ok;
    absolute_time_t deadline;
    char response[64];
    size_t response_len;
} http_state_t;

static UBYTE *oled_image;

static void oled_init(void) {
    DEV_Module_Init();
    OLED_1in3_C_Init();
    OLED_1in3_C_Clear();

    UWORD size = ((OLED_1in3_C_WIDTH % 8 == 0) ? (OLED_1in3_C_WIDTH / 8)
                                               : (OLED_1in3_C_WIDTH / 8 + 1))
               * OLED_1in3_C_HEIGHT;
    oled_image = (UBYTE *)malloc(size);
    Paint_NewImage(oled_image, OLED_1in3_C_WIDTH, OLED_1in3_C_HEIGHT, 0, WHITE);
    Paint_Clear(BLACK);
    OLED_1in3_C_Display(oled_image);
}

static void oled_show_text(const char *line1, const char *line2) {
    Paint_Clear(BLACK);
    Paint_DrawString_EN(0, 6,  line1, &Font16, WHITE, BLACK);
    Paint_DrawString_EN(0, 36, line2, &Font12, WHITE, BLACK);
    OLED_1in3_C_Display(oled_image);
}

static err_t http_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    http_state_t *s = (http_state_t *)arg;
    if (err != ERR_OK) {
        s->ok = false;
        s->done = true;
        return ERR_OK;
    }
    err_t w = tcp_write(pcb, s->request, s->request_len, TCP_WRITE_FLAG_COPY);
    if (w != ERR_OK) {
        s->ok = false;
        s->done = true;
        tcp_abort(pcb);
        return ERR_OK;
    }
    tcp_output(pcb);
    return ERR_OK;
}

static bool http_status_ok(const char *response, size_t len) {
    const char *sp = memchr(response, ' ', len);
    if (!sp) return false;
    size_t off = (size_t)(sp - response);
    if (off + 4 > len) return false;   // need " 200" (space + 3 digits)
    char code[4] = {response[off + 1], response[off + 2], response[off + 3], '\0'};
    int status = atoi(code);
    return status >= 200 && status < 300;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_state_t *s = (http_state_t *)arg;
    if (err != ERR_OK) {
        s->ok = false;
        s->done = true;
        return ERR_OK;
    }
    if (p == NULL) {
        // Connection closed; evaluate the HTTP status code we received.
        s->ok = http_status_ok(s->response, s->response_len);
        s->done = true;
        tcp_close(pcb);
        return ERR_OK;
    }
    if (s->response_len < sizeof(s->response) - 1) {
        size_t avail = sizeof(s->response) - 1 - s->response_len;
        size_t n = (p->tot_len < avail) ? p->tot_len : avail;
        pbuf_copy_partial(p, s->response + s->response_len, n, 0);
        s->response_len += n;
        s->response[s->response_len] = '\0';
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void http_err(void *arg, err_t err) {
    http_state_t *s = (http_state_t *)arg;
    if (err != ERR_ABRT) {
        s->ok = false;
        s->done = true;
    }
}

static err_t http_poll(void *arg, struct tcp_pcb *pcb) {
    http_state_t *s = (http_state_t *)arg;
    if (time_reached(s->deadline)) {
        s->ok = false;
        s->done = true;
        tcp_abort(pcb);
    }
    return ERR_OK;
}

// Send a POST /action/<command> request to the bridge and print the outcome.
static bool send_action(const char *command) {
    static char request[256];
    static http_state_t st;

    int len = snprintf(request, sizeof(request),
                       "POST /action/%s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       command, BRIDGE_IP, BRIDGE_PORT);

    st = (http_state_t){0};
    st.request = request;
    st.request_len = (size_t)len;
    st.deadline = make_timeout_time_ms(5000);
    ip4addr_aton(BRIDGE_IP, &st.remote_addr);

    st.pcb = tcp_new_ip_type(IP_GET_TYPE(&st.remote_addr));
    if (!st.pcb) {
        printf("error: no pcb\n");
        return false;
    }

    tcp_arg(st.pcb, &st);
    tcp_poll(st.pcb, http_poll, 1);
    tcp_recv(st.pcb, http_recv);
    tcp_err(st.pcb, http_err);

    cyw43_arch_lwip_begin();
    err_t err = tcp_connect(st.pcb, &st.remote_addr, BRIDGE_PORT, http_connected);
    cyw43_arch_lwip_end();
    if (err != ERR_OK) {
        printf("error: connect\n");
        return false;
    }

    while (!st.done && !time_reached(st.deadline)) {
        sleep_ms(10);
    }

    if (!st.done) {
        printf("error: %s timed out\n", command);
        return false;
    }
    printf("%s -> %s\n", command, st.ok ? "ok" : "failed");
    return st.ok;
}

static void dispatch_gesture(int taps) {
    const char *command = NULL;
    const char *label = NULL;
    switch (taps) {
        case 1:  command = "play-pause";          label = "Play/Pause"; break;
        case 2:  command = "next";                label = "Next";       break;
        case 3:  command = "previous-or-restart"; label = "Previous";   break;
        case 4:  command = "minimalist";          label = "Minimalist"; break;
        case 5:  command = "party";               label = "Party";      break;
        default:
            oled_show_text("calm down", "");
            return;
    }
    oled_show_text(label, "");
    if (!send_action(command)) oled_show_text(label, "Failed");
}

static void long_press(void) {
    oled_show_text("Favourite", "");
    if (!send_action("favourite")) oled_show_text("Favourite", "Failed");
}

int main() {
    stdio_init_all();

    oled_init();
    oled_show_text("Pico Music", "Player");

    if (cyw43_arch_init()) {
        printf("error: cyw43 init failed\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();
    printf("connecting to wifi...\n");
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                           CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("warning: wifi connect failed, continuing without network\n");
    } else {
        printf("wifi connected\n");
    }

    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    bool button_pressed = false;
    absolute_time_t debounce_until = 0;
    absolute_time_t press_start;
    bool long_press_fired = false;
    int tap_count = 0;
    absolute_time_t multi_tap_deadline;

    while (true) {
        bool pressed = !gpio_get(BUTTON_PIN); // inverts the active-low reading i.e. true if pressed, false if released

        if (pressed != button_pressed && time_reached(debounce_until)) {
            button_pressed = pressed;
            debounce_until = make_timeout_time_ms(DEBOUNCE_MS);

            if (pressed) {
                press_start = get_absolute_time();
                long_press_fired = false;
            } else {
                if (!long_press_fired) {
                    tap_count++;
                    multi_tap_deadline = make_timeout_time_ms(MULTI_TAP_WINDOW_MS);
                }
            }
        }

        if (button_pressed && !long_press_fired &&
            absolute_time_diff_us(press_start, get_absolute_time()) >= LONG_PRESS_MS * 1000) {
            long_press_fired = true;
            tap_count = 0;
            long_press();
        }

        if (tap_count > 0 && time_reached(multi_tap_deadline)) {
            dispatch_gesture(tap_count);
            tap_count = 0;
        }

        sleep_ms(10);
    }
}