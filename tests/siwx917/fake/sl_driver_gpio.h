#ifndef FAKE_SL_DRIVER_GPIO_H
#define FAKE_SL_DRIVER_GPIO_H
#include <stdint.h>
typedef uint32_t sl_status_t;
#define SL_STATUS_OK   0x0000u
#define SL_STATUS_FAIL 0x0001u
#define SL_GPIO_PORT_A 0
typedef struct { uint8_t port; uint8_t pin; } sl_gpio_t;
typedef uint8_t sl_gpio_mode_t;
sl_status_t sl_gpio_driver_set_pin_mode(sl_gpio_t *gpio, sl_gpio_mode_t mode, uint32_t output_value);

typedef struct {
    int      calls;
    uint8_t  last_port, last_pin, last_mode;
    sl_status_t fail;
} fake_gpio_state_t;
extern fake_gpio_state_t g_fake_gpio;
void fake_gpio_reset(void);
#endif
