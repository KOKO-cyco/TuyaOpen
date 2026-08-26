#include <string.h>
#include "sl_driver_gpio.h"
fake_gpio_state_t g_fake_gpio;
void fake_gpio_reset(void) { memset(&g_fake_gpio, 0, sizeof(g_fake_gpio)); }
sl_status_t sl_gpio_driver_set_pin_mode(sl_gpio_t *gpio, sl_gpio_mode_t mode, uint32_t out)
{
    (void)out;
    g_fake_gpio.calls++;
    g_fake_gpio.last_port = gpio->port;
    g_fake_gpio.last_pin  = gpio->pin;
    g_fake_gpio.last_mode = mode;
    return g_fake_gpio.fail ? g_fake_gpio.fail : SL_STATUS_OK;
}
