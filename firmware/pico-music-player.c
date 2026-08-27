#include <stdio.h>
#include "pico/stdlib.h"

// Push button on GPIO15 (active-low: pressed reads 0)
#define BUTTON_PIN            15
#define DEBOUNCE_MS           25
#define LONG_PRESS_MS         1000
#define MULTI_TAP_WINDOW_MS   300

static void dispatch_gesture(int taps) {
    switch (taps) {
        case 1:  printf("gesture: play/pause\n");               break;
        case 2:  printf("gesture: next track\n");               break;
        case 3:  printf("gesture: restart / previous track\n"); break;
        case 4:  printf("gesture: minimalist mode\n");          break;
        case 5:  printf("gesture: party mode\n");               break;
        default: printf("calm down\n");                         break;
    }
}

int main() {
    stdio_init_all();

    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    printf("pico-music-player started\n");

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
            printf("gesture: add track to favourites\n");
        }

        if (tap_count > 0 && time_reached(multi_tap_deadline)) {
            dispatch_gesture(tap_count);
            tap_count = 0;
        }

        sleep_ms(10);
    }
}
