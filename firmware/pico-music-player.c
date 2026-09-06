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

// Now-playing poll + display layout
#define POLL_INTERVAL_MS      1000
#define HTTP_RESPONSE_MAX     3072
#define FIELD_MAX             64

// Text bitmap line sizes (must match the bridge rendering)
#define BMP_ROW_BYTES         16   // 128 px / 8
#define TITLE_H               13
#define ARTIST_H              13
#define ALBUM_H               12
#define TITLE_BMP_BYTES       (BMP_ROW_BYTES * TITLE_H)
#define ARTIST_BMP_BYTES      (BMP_ROW_BYTES * ARTIST_H)
#define ALBUM_BMP_BYTES       (BMP_ROW_BYTES * ALBUM_H)

#define ICON_X                2
#define ICON_Y                1
#define TITLE_Y               0
#define ARTIST_Y              13
#define ALBUM_Y               26
#define BAR_Y                 38
#define BAR_H                 6
#define TIME_Y                44

// Heart (Favourite) animation layout
#define HEART_CY              30
#define HEART_OFFSET          32
#define HEART_TIP             5

typedef struct http_state {
    struct tcp_pcb *pcb;
    ip_addr_t remote_addr;
    const char *request;
    size_t request_len;
    volatile bool done;
    volatile bool ok;
    absolute_time_t deadline;
    char response[HTTP_RESPONSE_MAX];
    size_t response_len;
} http_state_t;

typedef struct {
    bool is_playing;
    long progress_ms;
    long duration_ms;
    UBYTE title_bmp[TITLE_BMP_BYTES];
    UBYTE artist_bmp[ARTIST_BMP_BYTES];
    UBYTE album_bmp[ALBUM_BMP_BYTES];
} track_snapshot_t;

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

static void fmt_time(long ms, char *buf, size_t bufsz) {
    long s = ms / 1000;
    if (s < 0) s = 0;
    snprintf(buf, bufsz, "%ld:%02ld", s / 60, s % 60);
}

static void draw_play(int x0, int y0) {
    const int w = 9;
    const int h = 11;
    const int half = h / 2;
    for (int r = 0; r < h; r++) {
        int t = r - half;
        if (t < 0) t = -t;
        int right = x0 + w - (t * w) / half;
        Paint_DrawLine((UWORD)x0, (UWORD)(y0 + r), (UWORD)right, (UWORD)(y0 + r),
                       WHITE, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    }
}

static void draw_pause(int x0, int y0) {
    Paint_DrawRectangle(x0, y0, x0 + 2, y0 + 10, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawRectangle(x0 + 5, y0, x0 + 7, y0 + 10, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
}

static void draw_heart(int cx, int cy, int scale) {
    for (int dy = -scale * 2; dy <= scale * 2; dy++) {
        float Y = -(float)dy / scale;
        for (int dx = -scale * 3; dx <= scale * 3; dx++) {
            float X = (float)dx / scale;
            float ypow = Y;
            for (int i = 1; i < HEART_TIP; i++) ypow *= Y;
            float t = X * X + Y * Y - 1.0f;
            if (t * t * t - X * X * ypow <= 0.0f) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < 128 && py >= 0 && py < 64)
                    Paint_DrawPoint(px, py, WHITE, DOT_PIXEL_1X1, DOT_FILL_AROUND);
            }
        }
    }
}

static void draw_hearts(int scale) {
    draw_heart(64 - HEART_OFFSET, HEART_CY, scale);
    draw_heart(64 + HEART_OFFSET, HEART_CY, scale);
}

static void animate_heart(void) {
    static const int scales[] = { 8, 13, 10, 14, 11, 13 }; // ba-bum pulse
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        Paint_Clear(BLACK);
        draw_hearts(scales[i]);
        OLED_1in3_C_Display(oled_image);
        sleep_ms(110);
    }
}

static void draw_progress_bar(int percent) {
    Paint_DrawRectangle(0, BAR_Y, 127, BAR_Y + BAR_H - 1, WHITE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    if (percent > 0) {
        int fill = (percent * 126) / 100;
        if (fill < 1) fill = 1;
        if (fill > 126) fill = 126;
        Paint_DrawRectangle(1, BAR_Y + 1, fill, BAR_Y + BAR_H - 2, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    }
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
    if (off + 4 > len) return false;
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

static bool http_request(const char *method, const char *path,
                         char *body_out, size_t body_cap) {
    static char request[256];
    static http_state_t st;

    int len;
    if (strcmp(method, "POST") == 0) {
        len = snprintf(request, sizeof(request),
                       "POST %s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       path, BRIDGE_IP, BRIDGE_PORT);
    } else {
        len = snprintf(request, sizeof(request),
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       path, BRIDGE_IP, BRIDGE_PORT);
    }

    memset(&st, 0, sizeof(st));
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

    if (!st.done || !st.ok) {
        printf("error: %s %s failed\n", method, path);
        return false;
    }

    if (body_out && body_cap > 0) {
        const char *body = strstr(st.response, "\r\n\r\n");
        body_out[0] = '\0';
        if (body) {
            body += 4;
            size_t n = 0;
            while (body[n] && n < body_cap - 1) {
                body_out[n] = body[n];
                n++;
            }
            body_out[n] = '\0';
        }
    }
    return true;
}

static bool send_action(const char *command) {
    char path[64];
    snprintf(path, sizeof(path), "/action/%s", command);
    return http_request("POST", path, NULL, 0);
}

static const char *json_find(const char *json, const char *key) {
    char needle[FIELD_MAX];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool json_bool_field(const char *json, const char *key) {
    const char *p = json_find(json, key);
    if (!p) return false;
    return strncmp(p, "true", 4) == 0;
}

static long json_int_field(const char *json, const char *key) {
    const char *p = json_find(json, key);
    if (!p) return 0;
    return strtol(p, NULL, 10);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void json_hex_field(const char *json, const char *key, UBYTE *out, size_t out_bytes) {
    const char *p = json_find(json, key);
    if (!p || *p != '"') {
        memset(out, 0, out_bytes);
        return;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n < out_bytes) {
        int hi = hexval(*p);
        int lo = *(p + 1) ? hexval(*(p + 1)) : -1;
        if (hi >= 0 && lo >= 0) {
            out[n++] = (UBYTE)((hi << 4) | lo);
            p += 2;
        } else {
            p++;
        }
    }
    while (n < out_bytes) out[n++] = 0;
}

static bool parse_track(const char *body, track_snapshot_t *t) {
    if (!body || !body[0]) return false;
    t->is_playing = json_bool_field(body, "is_playing");
    t->progress_ms = json_int_field(body, "progress_ms");
    t->duration_ms = json_int_field(body, "duration_ms");
    json_hex_field(body, "title_bmp", t->title_bmp, TITLE_BMP_BYTES);
    json_hex_field(body, "artist_bmp", t->artist_bmp, ARTIST_BMP_BYTES);
    json_hex_field(body, "album_bmp", t->album_bmp, ALBUM_BMP_BYTES);
    return true;
}

static void blit_bitmap(const UBYTE *bmp, int y0, int h) {
    for (int r = 0; r < h; r++) {
        for (int i = 0; i < BMP_ROW_BYTES; i++) {
            oled_image[(y0 + r) * BMP_ROW_BYTES + i] |= bmp[r * BMP_ROW_BYTES + i];
        }
    }
}

static void render_track(const track_snapshot_t *t) {
    Paint_Clear(BLACK);

    if (t->is_playing) {
        draw_play(ICON_X, ICON_Y);
    } else if (t->duration_ms > 0) {
        draw_pause(ICON_X, ICON_Y);
    }

    blit_bitmap(t->title_bmp, TITLE_Y, TITLE_H);
    blit_bitmap(t->artist_bmp, ARTIST_Y, ARTIST_H);
    blit_bitmap(t->album_bmp, ALBUM_Y, ALBUM_H);

    int percent = 0;
    if (t->duration_ms > 0) {
        percent = (int)((t->progress_ms * 100) / t->duration_ms);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
    }
    draw_progress_bar(percent);

    char cur[16], dur[16], timebuf[32];
    fmt_time(t->progress_ms, cur, sizeof(cur));
    fmt_time(t->duration_ms, dur, sizeof(dur));
    snprintf(timebuf, sizeof(timebuf), "%s / %s", cur, dur);
    Paint_DrawString_EN(0, TIME_Y, timebuf, &Font8, WHITE, BLACK);

    OLED_1in3_C_Display(oled_image);
}

static void poll_track(void) {
    static char body[HTTP_RESPONSE_MAX];
    track_snapshot_t snap;

    if (!http_request("GET", "/track", body, sizeof(body))) {
        oled_show_text("Bridge offline", "");
        return;
    }
    if (!parse_track(body, &snap)) {
        oled_show_text("Bad response", "");
        return;
    }
    render_track(&snap);
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
    animate_heart();
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

    absolute_time_t next_poll = get_absolute_time();

    while (true) {
        bool pressed = !gpio_get(BUTTON_PIN);

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

        if (time_reached(next_poll)) {
            next_poll = make_timeout_time_ms(POLL_INTERVAL_MS);
            poll_track();
        }

        sleep_ms(10);
    }
}
