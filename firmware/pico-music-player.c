#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"

#include "network_config.h"

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
    bool done;
    bool ok;
    absolute_time_t deadline;
} http_state_t;

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

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_state_t *s = (http_state_t *)arg;
    if (err != ERR_OK) {
        s->ok = false;
        s->done = true;
        return ERR_OK;
    }
    if (p == NULL) {
        // Server closed the connection after its response.
        s->ok = true;
        s->done = true;
        tcp_close(pcb);
        return ERR_OK;
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
    switch (taps) {
        case 1:  command = "play-pause";          break;
        case 2:  command = "next";                break;
        case 3:  command = "previous-or-restart"; break;
        case 4:  command = "minimalist";          break;
        case 5:  command = "party";               break;
        default: printf("calm down\n");           return;
    }
    printf("gesture: %d taps -> %s\n", taps, command);
    send_action(command);
}

static void long_press(void) {
    printf("gesture: add track to favourites -> favourite\n");
    send_action("favourite");
}

int main() {
    stdio_init_all();

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