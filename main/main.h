#include "driver/gpio.h"
#define NUM_COMPONENTS       8


#define RETRACTOR_PIN        GPIO_NUM_7
#define DETERGENT_VALVE_PIN  GPIO_NUM_8
#define COLD_VALVE_PIN       GPIO_NUM_5
#define DRAIN_PUMP_PIN       GPIO_NUM_19
#define HOT_VALVE_PIN        GPIO_NUM_9
#define SOFT_VALVE_PIN       GPIO_NUM_18
#define MOTOR_ON_PIN         GPIO_NUM_4
#define MOTOR_DIRECTION_PIN  GPIO_NUM_10
#define FLOW_SENSOR_PIN      GPIO_NUM_0

void init_all_gpio(void);