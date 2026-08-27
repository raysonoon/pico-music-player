#include "pico/stdlib.h"

int main() {
    // 1. Define the GPIO pin
    const uint LED_PIN = 15;

    // 2. Initialize the GPIO peripheral
    gpio_init(LED_PIN);

    // 3. Set the pin direction to output
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // 4. Infinite loop
    while (true) {
        gpio_put(LED_PIN, 1);    // Turn LED ON (High)
        sleep_ms(2000);          // Wait 2000 ms (2 seconds)
        
        gpio_put(LED_PIN, 0);    // Turn LED OFF (Low)
        sleep_ms(2000);          // Wait 2000 ms (2 seconds)
    }

    return 0;
}