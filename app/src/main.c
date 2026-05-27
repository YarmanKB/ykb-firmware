#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <subsys/kb_handler.h>

#include <nrfx_clock.h>

#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

static void on_event(uint16_t key_index, bool pressed) {
    LOG_INF("Key %d %s", key_index, pressed ? "pressed" : "released");
}

KB_HANDLER_CB_DEFINE(main) = {
    .on_event = on_event,
};

int main(void) {
#if CONFIG_SOC_NRF5340_CPUAPP
    nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
#endif // CONFIG_SOC_NRF5340_CPUAPP
}
