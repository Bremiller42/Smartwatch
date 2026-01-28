#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

void touch_int_debug_task(void *arg)
{
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_3, GPIO_PULLUP_ONLY);

    while (1) {
        printf("TOUCH_INT GPIO3 = %d\n", gpio_get_level(GPIO_NUM_3));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}