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

// Push button on GPIO21 (active-low: pressed reads 0)
#define BUTTON_PIN            21
#define DEBOUNCE_MS           25
#define LONG_PRESS_MS         1000
#define MULTI_TAP_WINDOW_MS   700
#define FEEDBACK_HOLD_MS      1500

// Find-a-song tempo capture mode
#define FIND_ARM_TAPS         6       // 6+ taps arms find-a-song mode
#define FIND_WINDOW_MS        5000    // tempo capture window after first tap
#define FIND_MIN_TAPS         3       // minimum taps to compute a tempo
#define FIND_MAX_TAPS         16      // capture buffer size
#define FIND_IDLE_MS          5000    // auto-exit if mode entered but never tapped
#define TAP_BPM_MIN           40
#define TAP_BPM_MAX           250

// Now-playing poll + display layout
#define POLL_DEFAULT_MS       1000
#define POLL_IDLE_MS          3000
#define END_THRESHOLD_MS      3000
#define HTTP_RESPONSE_MAX     3072
#define FIELD_MAX             64

// Text bitmap line sizes (must match the bridge rendering)
#define BMP_ROW_BYTES         16   // 128 px / 8
#define TITLE_H               13
#define ARTIST_H              13
#define ALBUM_H               12
// Buffers are sized to the larger minimalist heights so they fit both modes
#define TITLE_BUF_H           21
#define ARTIST_BUF_H          15
#define ALBUM_BUF_H           15
#define TITLE_BMP_BYTES       (BMP_ROW_BYTES * TITLE_BUF_H)
#define ARTIST_BMP_BYTES      (BMP_ROW_BYTES * ARTIST_BUF_H)
#define ALBUM_BMP_BYTES       (BMP_ROW_BYTES * ALBUM_BUF_H)

#define ICON_X                2
#define ICON_Y                6
#define TITLE_Y               3
#define ARTIST_Y              16
#define ALBUM_Y               29
#define BAR_Y                 41
#define BAR_H                 6
#define TIME_Y                47

// Minimalist mode layout (must match the bridge rendering)
#define MINI_TITLE_H          TITLE_BUF_H
#define MINI_ARTIST_H         ARTIST_BUF_H
#define MINI_ALBUM_H          ALBUM_BUF_H
#define MINI_GLYPH_X          2
#define MINI_GLYPH_Y          11
#define MINI_TITLE_Y          5
#define MINI_ARTIST_Y         25
#define MINI_ALBUM_Y          40

// Heart (Favourite) animation layout
#define HEART_CY              30
#define HEART_OFFSET          26
#define HEART_TIP             5

// Tap-count eyes layout
#define EYE_Y                 24
#define EYE_OFFSET            26
#define EYE_R                 13
#define SMILE_Y               52
#define SMILE_SPAN            40

// Party mode: album art + tempo-synced border
#define ART_W                 40
#define ART_H                 40
#define ART_BYTES             (ART_W * ART_H / 8)
#define ART_X                 ((128 - ART_W) / 2)
#define ART_Y                 24
#define DEFAULT_BPM           120.0f
#define BPM_MIN               40.0f
#define BPM_MAX               250.0f
#define PARTY_FRAME_MS        33
#define PARTY_SEG_LEN         32
#define PARTY_BEATS_PER_LOOP  4
#define PARTY_PULSE_FRAC      20
#define BORDER_INSET          1

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
    long remaining_ms;
    float tempo;
    char mode[12];
    UBYTE title_bmp[TITLE_BMP_BYTES];
    UBYTE artist_bmp[ARTIST_BMP_BYTES];
    UBYTE album_bmp[ALBUM_BMP_BYTES];
} track_snapshot_t;

static int last_remaining_ms = 0;
static bool last_minimalist = false;
static track_snapshot_t g_snap;
static bool g_have_snap = false;
static float g_tempo_bpm = DEFAULT_BPM;
static UBYTE art_bmp[ART_BYTES];

typedef enum { MODE_NORMAL, MODE_FIND_SONG } app_mode_t;
static app_mode_t app_mode = MODE_NORMAL;
static absolute_time_t find_times[FIND_MAX_TAPS];
static int find_tap_count = 0;
static absolute_time_t find_window_end;
static absolute_time_t find_idle_deadline;
static bool find_window_active = false;

static UBYTE *oled_image;

static absolute_time_t feedback_until;

static void hold_feedback(void) {
    feedback_until = make_timeout_time_ms(FEEDBACK_HOLD_MS);
}

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

// Draw a string horizontally centered at row y, using a given font.
static void paint_centered(int y, const char *s, sFONT *f) {
    int w = (int)strlen(s) * (int)f->Width;
    int x = (128 - w) / 2;
    if (x < 0) x = 0;
    Paint_DrawString_EN((UWORD)x, (UWORD)y, s, f, WHITE, BLACK);
}

// Show a single personality message, auto-sizing the font to fit the width.
// Font16 <= 11 chars, Font12 <= 18 chars, otherwise Font8.
static void oled_show_msg(const char *msg) {
    Paint_Clear(BLACK);
    size_t n = strlen(msg);
    sFONT *f;
    int fy;
    if (n <= 11) {
        f = &Font16; fy = (64 - f->Height) / 2;
    } else if (n <= 18) {
        f = &Font12; fy = (64 - f->Height) / 2;
    } else {
        f = &Font8;  fy = (64 - f->Height) / 2;
    }
    paint_centered(fy, msg, f);
    OLED_1in3_C_Display(oled_image);
}

// Find-a-song feedback: Font12 phrase (1-2 lines) with optional Font8 footer below.
static void find_song_show(const char *pa, const char *pb, const char *footer) {
    Paint_Clear(BLACK);
    int n = (pa && pa[0]) + (pb && pb[0]);
    int has_footer = (footer && footer[0]);
    int y;
    if (has_footer) {
        y = (44 - n * 12) / 2;   // center phrase in the area above the footer
        if (y < 0) y = 0;
    } else {
        y = (64 - n * 12) / 2;   // center phrase vertically on the whole screen
    }
    if (pa && pa[0]) {
        paint_centered(y, pa, &Font12);
        y += 12;
    }
    if (pb && pb[0]) paint_centered(y, pb, &Font12);
    if (has_footer) paint_centered(50, footer, &Font8);
    OLED_1in3_C_Display(oled_image);
}

static void fmt_time(long ms, char *buf, size_t bufsz) {
    long s = ms / 1000;
    if (s < 0) s = 0;
    snprintf(buf, bufsz, "%ld:%02ld", s / 60, s % 60);
}

static void draw_play(int x0, int y0) {
    const int w = 9;
    const int h = 9;
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
    Paint_DrawRectangle(x0, y0, x0 + 2, y0 + 9, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawRectangle(x0 + 5, y0, x0 + 7, y0 + 9, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
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

static void draw_eye(int cx, int cy, int r) {
    Paint_DrawCircle(cx, cy, r, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
}

static void draw_smile(int cx, int cy, int span, int rise) {
    int half = span / 2;
    for (int x = -half; x <= half; x++) {
        int y = cy - (x * x * rise + half * half / 2) / (half * half);
        Paint_DrawPoint(cx + x, y, WHITE, DOT_PIXEL_1X1, DOT_FILL_AROUND);
    }
}

static void draw_squiggle(int cx, int cy, int span) {
    const int amp = 4;
    const int hw  = 8;
    int half = span / 2;
    for (int i = 0; i <= span; i++) {
        int x = i - half;
        int h = i / hw;
        int p = i % hw;
        int t = p * 2 - (hw - 1);
        int v = amp - (amp * t * t) / ((hw - 1) * (hw - 1));
        int y = (h % 2 == 0) ? (cy - v) : (cy + v);
        Paint_DrawPoint(cx + x, y, WHITE, DOT_PIXEL_1X1, DOT_FILL_AROUND);
    }
}

static void render_eyes(int taps) {
    static const int rise[] = { 0, 2, 4, 7, 10 };
    Paint_Clear(BLACK);
    draw_eye(64 - EYE_OFFSET, EYE_Y, EYE_R);
    draw_eye(64 + EYE_OFFSET, EYE_Y, EYE_R);
    if (taps >= 6) {
        draw_squiggle(64, SMILE_Y, SMILE_SPAN);
    } else {
        int r = (taps >= 1 && taps <= 5) ? rise[taps - 1] : 0;
        draw_smile(64, SMILE_Y, SMILE_SPAN, r);
    }
    OLED_1in3_C_Display(oled_image);
}

// Love-struck face: pulsing heart-shaped eyes + a small grin below.
static void render_love_face(int scale) {
    Paint_Clear(BLACK);
    draw_heart(64 - HEART_OFFSET, EYE_Y, scale);
    draw_heart(64 + HEART_OFFSET, EYE_Y, scale);
    draw_smile(64, SMILE_Y, SMILE_SPAN, 8);
    OLED_1in3_C_Display(oled_image);
}

static void animate_love_face(void) {
    static const int scales[] = { 7, 9, 7, 10, 8, 9 }; // heart-eye ba-bum pulse
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        render_love_face(scales[i]);
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
                         char *body_out, size_t body_cap, uint32_t timeout_ms) {
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
    st.deadline = make_timeout_time_ms(timeout_ms);
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

static bool json_str_field(const char *json, const char *key, char *out, size_t out_cap);

static bool send_action(const char *command, char *result, size_t result_cap) {
    char path[64];
    snprintf(path, sizeof(path), "/action/%s", command);
    char body[256];
    if (!http_request("POST", path, body, sizeof(body), 5000)) return false;
    if (result && result_cap > 0) {
        result[0] = '\0';
        json_str_field(body, "result", result, result_cap);
    }
    return true;
}

static bool send_find_song(int bpm, char *result, size_t result_cap) {
    // find-song is a multi-step bridge operation (recommendation + queue + skip)
    // that can exceed 5s, so give it a dedicated longer timeout.
    char path[64];
    snprintf(path, sizeof(path), "/action/find-song?bpm=%d", bpm);
    char body[256];
    if (!http_request("POST", path, body, sizeof(body), 12000)) return false;
    if (result && result_cap > 0) {
        result[0] = '\0';
        json_str_field(body, "result", result, result_cap);
    }
    return true;
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

static float json_float_field(const char *json, const char *key) {
    const char *p = json_find(json, key);
    if (!p) return 0.0f;
    return strtof(p, NULL);
}

static bool json_str_field(const char *json, const char *key, char *out, size_t out_cap) {
    const char *p = json_find(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n < out_cap - 1) out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
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
    t->remaining_ms = json_int_field(body, "remaining_ms");
    t->tempo = json_float_field(body, "tempo");
    json_str_field(body, "mode", t->mode, sizeof(t->mode));
    if (!t->mode[0]) strcpy(t->mode, "default");

    if (strcmp(t->mode, "minimalist") == 0) {
        json_hex_field(body, "title_bmp", t->title_bmp, BMP_ROW_BYTES * MINI_TITLE_H);
        json_hex_field(body, "artist_bmp", t->artist_bmp, BMP_ROW_BYTES * MINI_ARTIST_H);
        json_hex_field(body, "album_bmp", t->album_bmp, BMP_ROW_BYTES * MINI_ALBUM_H);
    } else {
        json_hex_field(body, "title_bmp", t->title_bmp, BMP_ROW_BYTES * TITLE_H);
        json_hex_field(body, "artist_bmp", t->artist_bmp, BMP_ROW_BYTES * ARTIST_H);
        json_hex_field(body, "album_bmp", t->album_bmp, BMP_ROW_BYTES * ALBUM_H);
    }
    return true;
}

static void blit_bitmap(const UBYTE *bmp, int y0, int h) {
    for (int r = 0; r < h; r++) {
        for (int i = 0; i < BMP_ROW_BYTES; i++) {
            oled_image[(y0 + r) * BMP_ROW_BYTES + i] |= bmp[r * BMP_ROW_BYTES + i];
        }
    }
}

static void draw_track_content(const track_snapshot_t *t, bool show) {
    Paint_Clear(BLACK);
    if (!show) return;

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
}

static void render_track(const track_snapshot_t *t) {
    if (strcmp(t->mode, "minimalist") == 0) {
        Paint_Clear(BLACK);
        if (t->is_playing) {
            draw_play(MINI_GLYPH_X, MINI_GLYPH_Y);
        } else if (t->duration_ms > 0) {
            draw_pause(MINI_GLYPH_X, MINI_GLYPH_Y);
        }
        blit_bitmap(t->title_bmp, MINI_TITLE_Y, MINI_TITLE_H);
        blit_bitmap(t->artist_bmp, MINI_ARTIST_Y, MINI_ARTIST_H);
        blit_bitmap(t->album_bmp, MINI_ALBUM_Y, MINI_ALBUM_H);
        OLED_1in3_C_Display(oled_image);
        return;
    }
    draw_track_content(t, true);
    OLED_1in3_C_Display(oled_image);
}

static void perimeter_point(int s, int *x, int *y) {
    const int W = 128 - 2 * BORDER_INSET;
    const int H = 64 - 2 * BORDER_INSET;
    int P = 2 * (W + H);
    s %= P;
    if (s < W) {
        *x = BORDER_INSET + s; *y = BORDER_INSET;
    } else if (s < W + H) {
        *x = BORDER_INSET + W - 1; *y = BORDER_INSET + (s - W);
    } else if (s < 2 * W + H) {
        *x = BORDER_INSET + W - 1 - (s - W - H); *y = BORDER_INSET + H - 1;
    } else {
        *x = BORDER_INSET; *y = BORDER_INSET + H - 1 - (s - 2 * W - H);
    }
}

static void draw_party_frame(int elapsed_ms) {
    float bpm = g_tempo_bpm;
    if (bpm < BPM_MIN) bpm = BPM_MIN;
    if (bpm > BPM_MAX) bpm = BPM_MAX;
    int beat_ms = (int)(60000.0f / bpm);
    if (beat_ms < 1) beat_ms = 1;
    int bar_ms = PARTY_BEATS_PER_LOOP * beat_ms;

    int phase = elapsed_ms % beat_ms;
    bool on_beat = (phase * 100 < beat_ms * PARTY_PULSE_FRAC);

    // Entire track content blinks on the beat.
    draw_track_content(&g_snap, !on_beat);

    // Rotating bright segment, clockwise, one loop per bar; pulses on the beat.
    const int P = 2 * ((128 - 2 * BORDER_INSET) + (64 - 2 * BORDER_INSET));
    int head = (int)(((int64_t)(elapsed_ms % bar_ms) * P) / bar_ms % P);
    DOT_PIXEL seg_w = on_beat ? DOT_PIXEL_3X3 : DOT_PIXEL_1X1;
    for (int d = 0; d < PARTY_SEG_LEN; d++) {
        int x, y;
        perimeter_point((head + d) % P, &x, &y);
        Paint_DrawPoint((UWORD)x, (UWORD)y, WHITE, seg_w, DOT_FILL_AROUND);
    }

    OLED_1in3_C_Display(oled_image);
}

static bool fetch_art(void) {
    static char body[HTTP_RESPONSE_MAX];
    if (!http_request("GET", "/art", body, sizeof(body), 5000)) return false;
    const char *p = json_find(body, "art_bmp");
    if (!p) return false;
    json_hex_field(body, "art_bmp", art_bmp, ART_BYTES);
    for (int i = 0; i < ART_BYTES; i++) {
        if (art_bmp[i]) return true;
    }
    return false;
}

static void render_art_screen(const char *label) {
    Paint_Clear(BLACK);
    for (int r = 0; r < ART_H; r++) {
        for (int c = 0; c < ART_W; c++) {
            if (art_bmp[r * (ART_W / 8) + c / 8] & (0x80 >> (c % 8))) {
                int x = ART_X + c;
                int y = ART_Y + r;
                oled_image[y * BMP_ROW_BYTES + x / 8] |= (0x80 >> (x % 8));
            }
        }
    }
    paint_centered(4, label, &Font12);
    OLED_1in3_C_Display(oled_image);
}

static void poll_track(void) {
    static char body[HTTP_RESPONSE_MAX];

    if (!http_request("GET", "/track", body, sizeof(body), 5000)) {
        oled_show_msg("craving connection");
        return;
    }
    if (!parse_track(body, &g_snap)) {
        oled_show_text("Bad response", "");
        return;
    }
    g_have_snap = true;
    last_minimalist = (strcmp(g_snap.mode, "minimalist") == 0);
    last_remaining_ms = (int)g_snap.remaining_ms;
    g_tempo_bpm = g_snap.tempo;
    if (strcmp(g_snap.mode, "party") != 0) {
        render_track(&g_snap);
    }
}

static int compute_tap_bpm(const absolute_time_t *times, int n) {
    if (n < 2) return 0;
    uint64_t sum_us = 0;
    for (int i = 1; i < n; i++) {
        sum_us += (uint64_t)absolute_time_diff_us(times[i - 1], times[i]);
    }
    uint64_t avg_us = sum_us / (n - 1);
    if (avg_us == 0) return 0;
    int bpm = (int)(60000000ULL / avg_us);
    if (bpm < TAP_BPM_MIN) bpm = TAP_BPM_MIN;
    if (bpm > TAP_BPM_MAX) bpm = TAP_BPM_MAX;
    return bpm;
}

typedef struct {
    const char *a;
    const char *b;
} phrase_t;

// Personality phrase for a tapped tempo, matching the bridge's 80/115/140 bands.
// Long phrases split across two Font12 lines (b == NULL for short ones).
static phrase_t bpm_phrase(int bpm) {
    if (bpm < 80)  return (phrase_t){ "takin' a", "chill pill" };
    if (bpm < 115) return (phrase_t){ "catchin' the", "groove" };
    if (bpm < 140) return (phrase_t){ "fast & curious", NULL };
    return (phrase_t){ "FULL SPEED AHEAD", NULL };
}

static void enter_find_song_mode(void) {
    app_mode = MODE_FIND_SONG;
    find_window_active = false;
    find_tap_count = 0;
    find_idle_deadline = make_timeout_time_ms(FIND_IDLE_MS);
    Paint_Clear(BLACK);
    paint_centered(8,  "feeling grumpy?", &Font12);
    paint_centered(20, "let's find a song", &Font12);
    paint_centered(44, "tap to start", &Font12);
    OLED_1in3_C_Display(oled_image);
}

static void find_song_tap(void) {
    absolute_time_t now = get_absolute_time();
    if (!find_window_active) {
        find_window_active = true;
        find_tap_count = 0;
        find_window_end = make_timeout_time_ms(FIND_WINDOW_MS);
    }
    if (find_tap_count < FIND_MAX_TAPS) {
        find_times[find_tap_count++] = now;
    }
    int bpm = compute_tap_bpm(find_times, find_tap_count);
    if (bpm > 0) {
        phrase_t p = bpm_phrase(bpm);
        char buf[16];
        snprintf(buf, sizeof(buf), "~%d BPM", bpm);
        find_song_show(p.a, p.b, buf);
    } else {
        find_song_show("keep tapping...", NULL, NULL);
    }
}

// Copy the current snapshot album art into dst (zeros if unavailable).
static void capture_art(UBYTE *dst) {
    memset(dst, 0, ART_BYTES);
    fetch_art();
    memcpy(dst, art_bmp, ART_BYTES);
}

static void finish_find_song(void) {
    int bpm = compute_tap_bpm(find_times, find_tap_count);
    phrase_t p = (bpm > 0) ? bpm_phrase(bpm) : (phrase_t){ "keep tapping...", NULL };
    if (find_tap_count >= FIND_MIN_TAPS) {
        // Snapshot the old art before the skip, then wait for /art to reflect
        // the newly recommended track (bounded ~5s, no fixed delay).
        UBYTE prev[ART_BYTES];
        capture_art(prev);
        char result[32];
        bool ok = send_find_song(bpm, result, sizeof(result)) && result[0];
        if (ok) {
            for (int i = 0; i < 50; i++) {
                sleep_ms(100);
                fetch_art();
                if (memcmp(prev, art_bmp, ART_BYTES) != 0) break;
            }
            render_art_screen("found ur jam!");
        } else {
            find_song_show(p.a, p.b, "no vibes matched :(");
        }
    } else {
        find_song_show(p.a, p.b, "don't ghost me :(");
    }
    hold_feedback();
    app_mode = MODE_NORMAL;
}

static void find_song_tick(void) {
    if (find_window_active && time_reached(find_window_end)) {
        find_window_active = false;
        finish_find_song();
    } else if (!find_window_active && time_reached(find_idle_deadline)) {
        app_mode = MODE_NORMAL;
    }
}

static void dispatch_gesture(int taps) {
    const char *command = NULL;
    const char *fallback = NULL;
    switch (taps) {
        case 1:  command = "play-pause";          fallback = "Play/Pause"; break;
        case 2:  command = "next";                fallback = "Next";       break;
        case 3:  command = "previous-or-restart"; fallback = "Previous";   break;
        case 4:  command = "minimalist";          fallback = "Minimalist"; break;
        case 5:  command = "party";               fallback = "Party";      break;
        default:
            enter_find_song_mode();
            return;
    }
    char result[32];
    bool ok = send_action(command, result, sizeof(result)) && result[0];

    if (strcmp(command, "party") == 0) {
        if (!ok) {
            oled_show_text("Party", "Failed");
        } else if (strcmp(result, "let's party!") == 0) {
            oled_show_msg("let's party!");
        } else {
            oled_show_msg(result);  // "that was fire!"
        }
        hold_feedback();
        return;
    }

    if (ok)
        oled_show_msg(result);
    else
        oled_show_text(fallback, "Failed");
    hold_feedback();
}

static void long_press(void) {
    hold_feedback();
    if (send_action("favourite", NULL, 0)) {
        animate_love_face();
    } else {
        oled_show_text("Favourite", "Failed");
        hold_feedback();
    }
}

int main() {
    stdio_init_all();

    oled_init();
    oled_show_text("Pico", "Music Player");

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
    absolute_time_t next_party_frame = 0;

    while (true) {
        bool pressed = !gpio_get(BUTTON_PIN);

        if (pressed != button_pressed && time_reached(debounce_until)) {
            button_pressed = pressed;
            debounce_until = make_timeout_time_ms(DEBOUNCE_MS);

            if (app_mode == MODE_NORMAL) {
                if (pressed) {
                    press_start = get_absolute_time();
                    long_press_fired = false;
                } else if (!long_press_fired) {
                    tap_count++;
                    multi_tap_deadline = make_timeout_time_ms(MULTI_TAP_WINDOW_MS);
                    render_eyes(tap_count);
                }
            } else if (!pressed) {
                find_song_tap();
            }
        }

        if (app_mode == MODE_NORMAL) {
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
        } else {
            find_song_tick();
        }

        if (app_mode == MODE_NORMAL && time_reached(next_poll)) {
            int interval = POLL_DEFAULT_MS;
            if (last_minimalist && !(last_remaining_ms > 0 && last_remaining_ms <= END_THRESHOLD_MS))
                interval = POLL_IDLE_MS;
            next_poll = make_timeout_time_ms(interval);
            if (tap_count == 0 && time_reached(feedback_until)) {
                poll_track();
            }
        }

        if (app_mode == MODE_NORMAL && g_have_snap && strcmp(g_snap.mode, "party") == 0 &&
            tap_count == 0 && time_reached(feedback_until) &&
            time_reached(next_party_frame)) {
            next_party_frame = make_timeout_time_ms(PARTY_FRAME_MS);
            int elapsed_ms = (int)to_ms_since_boot(get_absolute_time());
            draw_party_frame(elapsed_ms);
        }

        sleep_ms(10);
    }
}
