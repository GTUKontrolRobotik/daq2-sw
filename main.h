#ifndef __MAIN_H_
#define __MAIN_H_

#include <stdlib.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/adc.h>
#include <string.h>
#include "servo.h"
#include "usb.h"

#define SLEEP 1
#define WAKE 2

#pragma pack(push, 1)
typedef struct {
	uint16_t servo1;
	uint16_t servo2;
	uint16_t servo3;
} servo_msg_t;

typedef struct {
	uint16_t adc1;
	uint16_t adc2;
} adc_msg_t;
#pragma pack(pop)

adc_msg_t adc_msg = { 0, 0 };
servo_msg_t servo_msg = { 0, 0, 0 };

volatile uint8_t incoming = 0;
volatile uint32_t last_time;
volatile uint32_t system_millis;

/* Buffer to be used for control requests. */
uint8_t usbd_control_buffer[256];
usbd_device *usbd_dev;
void cdcacm_data_rx_cb(usbd_device *usbd_dev, uint8_t ep);

#endif
