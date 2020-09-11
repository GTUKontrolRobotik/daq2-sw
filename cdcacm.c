/*
 * This file is part of the libopencm3 project.
 *
 * Copyright (C) 2010 Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

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
#include "mcp492x.h"
#include "servo.h"

static const struct usb_device_descriptor dev = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = USB_CLASS_CDC,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x0483,
	.idProduct = 0x5740,
	.bcdDevice = 0x0200,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

/*
 * This notification endpoint isn't implemented. According to CDC spec its
 * optional, but its absence causes a NULL pointer dereference in Linux
 * cdc_acm driver.
 */
static const struct usb_endpoint_descriptor comm_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x83,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 16,
	.bInterval = 255,
}};

static const struct usb_endpoint_descriptor data_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x01,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 64,
	.bInterval = 1,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x82,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 64,
	.bInterval = 1,
}};

static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_mgmt;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdcacm_functional_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_mgmt = {
		.bFunctionLength =
			sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = 1,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 0,
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = 0,
		.bSubordinateInterface0 = 1,
	 },
};

static const struct usb_interface_descriptor comm_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 0,

	.endpoint = comm_endp,

	.extra = &cdcacm_functional_descriptors,
	.extralen = sizeof(cdcacm_functional_descriptors),
}};

static const struct usb_interface_descriptor data_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = data_endp,
}};

static const struct usb_interface ifaces[] = {{
	.num_altsetting = 1,
	.altsetting = comm_iface,
}, {
	.num_altsetting = 1,
	.altsetting = data_iface,
}};

static const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 2,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0x80,
	.bMaxPower = 0x32,

	.interface = ifaces,
};

static const char *usb_strings[] = {
	"Black Sphere Technologies",
	"CDC-ACM Demo",
	"DEMO",
};

/* Buffer to be used for control requests. */
uint8_t usbd_control_buffer[256];

static enum usbd_request_return_codes cdcacm_control_request(usbd_device *usbd_dev, struct usb_setup_data *req, uint8_t **buf,
		uint16_t *len, void (**complete)(usbd_device *usbd_dev, struct usb_setup_data *req))
{
	(void)complete;
	(void)buf;
	(void)usbd_dev;

	switch (req->bRequest) {
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE: {
		/*
		 * This Linux cdc_acm driver requires this to be implemented
		 * even though it's optional in the CDC spec, and we don't
		 * advertise it in the ACM functional descriptor.
		 */
		char local_buf[10];
		struct usb_cdc_notification *notif = (void *)local_buf;

		/* We echo signals back to host as notification. */
		notif->bmRequestType = 0xA1;
		notif->bNotification = USB_CDC_NOTIFY_SERIAL_STATE;
		notif->wValue = 0;
		notif->wIndex = 0;
		notif->wLength = 2;
		local_buf[8] = req->wValue & 3;
		local_buf[9] = 0;
		// usbd_ep_write_packet(0x83, buf, 10);
		return USBD_REQ_HANDLED;
		}
	case USB_CDC_REQ_SET_LINE_CODING:
		if (*len < sizeof(struct usb_cdc_line_coding))
			return USBD_REQ_NOTSUPP;
		return USBD_REQ_HANDLED;
	}
	return USBD_REQ_NOTSUPP;
}

#define IN_TYPE  0x44 //'D' dac
#define OUT_TYPE 0x45 //'E' enc
#define END      0x0A //Line feed
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

typedef struct {
	//uint8_t type;
	int16_t enc1;
	int16_t enc2;
	//uint8_t end;
} out_msg_t;

typedef struct {
	//uint8_t type;
	uint16_t dac1;
	uint16_t dac2;
	//uint8_t end;
} in_msg_t;
#pragma pack(pop)

//in_msg_t  in_msg  = { IN_TYPE,  0, 0, END };
//out_msg_t out_msg = { OUT_TYPE, 0, 0, END };
in_msg_t  in_msg  = { 0, 0 };
out_msg_t out_msg = { 0, 0 };
adc_msg_t adc_msg = { 0, 0 };
servo_msg_t servo_msg = { 0, 0, 0 };

volatile uint8_t incoming = 0;
volatile uint32_t last_time;
volatile uint32_t system_millis;
static void cdcacm_data_rx_cb(usbd_device *usbd_dev, uint8_t ep)
{
	(void)ep;
	(void)usbd_dev;

	char buf[64];
	int len = usbd_ep_read_packet(usbd_dev, 0x01, buf, 64);

	if (len == sizeof(in_msg_t)) {
		memcpy(&in_msg, buf, sizeof(in_msg_t)); 
		dac_write(0,0,in_msg.dac1);
		dac_write(0,1,in_msg.dac2);
		last_time = system_millis;
		incoming = 1;
	} else if (len == sizeof(servo_msg_t)) {
		memcpy(&servo_msg, buf, sizeof(servo_msg_t)); 
        servo_set_position(SERVO_CH1, servo_msg.servo1);
        servo_set_position(SERVO_CH2, servo_msg.servo2);
        servo_set_position(SERVO_CH3, servo_msg.servo3);
		incoming = 1;
	} else {
		if(buf[0] == 'T'){
			buf[0] = 'h';
			buf[1] = 'e';
			buf[2] = 'l';
			buf[3] = 'l';
			buf[4] = 'o';
			buf[5] = '\n';
			usbd_ep_write_packet(usbd_dev, 0x82, buf, 6);
		}
	}
}

static void cdcacm_set_config(usbd_device *usbd_dev, uint16_t wValue)
{
	(void)wValue;
	(void)usbd_dev;

	usbd_ep_setup(usbd_dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64, cdcacm_data_rx_cb);
	usbd_ep_setup(usbd_dev, 0x82, USB_ENDPOINT_ATTR_BULK, 64, NULL);
	usbd_ep_setup(usbd_dev, 0x83, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

	usbd_register_control_callback(
				usbd_dev,
				USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
				USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
				cdcacm_control_request);
}


usbd_device *usbd_dev;

void sys_tick_handler(void)
{
	system_millis++;
}

void msleep(uint32_t delay)
{
	uint32_t wake = system_millis + delay;
	while (wake > system_millis)
		usbd_poll(usbd_dev);
}

static void systick_setup(void)
{
	/* 72MHz / 8 => 9000000 counts per second. */
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
	/* 9000000/9000 = 1000 overflows per second - every 1ms one interrupt */
	systick_set_reload(8999); //8999 1ms
	systick_interrupt_enable();
	systick_counter_enable();
}

static void tim_init(void){
	//rcc_periph_clock_enable(RCC_TIM1);
	rcc_periph_clock_enable(RCC_TIM2);
	rcc_periph_clock_enable(RCC_TIM3);
	//rcc_periph_clock_enable(RCC_TIM4);
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_AFIO);

	/*TIM4 CH1 = B6 CH2 = B7 */
	/*gpio_set_mode(GPIOB, GPIO_MODE_INPUT,
	              GPIO_CNF_INPUT_FLOAT,
		      GPIO6 | GPIO7); */

	/*TIM2 CH1 = A0 CH2 = A1 */
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
	              GPIO_CNF_INPUT_FLOAT,
		      GPIO0 | GPIO1); 

	/*TIM3 CH1 = B4 CH2 = B5 */
	gpio_set_mode(GPIOB, GPIO_MODE_INPUT,
	              GPIO_CNF_INPUT_FLOAT,
		      GPIO4 | GPIO5); 
	//AFIO_MAPR |= AFIO_MAPR_TIM3_REMAP_PARTIAL_REMAP;
	gpio_primary_remap(0, AFIO_MAPR_TIM3_REMAP_PARTIAL_REMAP);

	/*TIM1 CH1 = A8 CH2 = A9 */
	/*gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
	              GPIO_CNF_INPUT_FLOAT,
	              GPIO8 | GPIO9); */

	/*timer_set_period(TIM1, 4095);
	timer_slave_set_mode(TIM1, 0x3); //encoder
	timer_ic_set_input(TIM1, TIM_IC1, TIM_IC_IN_TI1);
	timer_ic_set_input(TIM1, TIM_IC2, TIM_IC_IN_TI2);
	timer_enable_counter(TIM1);*/

	timer_set_period(TIM3, 0xFFFF);
	timer_slave_set_mode(TIM3, 0x3); //encoder
	timer_ic_set_input(TIM3, TIM_IC1, TIM_IC_IN_TI1);
	timer_ic_set_input(TIM3, TIM_IC2, TIM_IC_IN_TI2);
	timer_enable_counter(TIM3);

	/*timer_set_period(TIM4, 4095);
	timer_slave_set_mode(TIM4, 0x3); //encoder
	timer_ic_set_input(TIM4, TIM_IC1, TIM_IC_IN_TI1);
	timer_ic_set_input(TIM4, TIM_IC2, TIM_IC_IN_TI2);
	timer_enable_counter(TIM4);*/

	timer_set_period(TIM2, 0xFFFF);
	timer_slave_set_mode(TIM2, 0x3); //encoder
	timer_ic_set_input(TIM2, TIM_IC1, TIM_IC_IN_TI1);
	timer_ic_set_input(TIM2, TIM_IC2, TIM_IC_IN_TI2);
	timer_enable_counter(TIM2);
}
static void dac_init(void){
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_AFIO);
	rcc_periph_clock_enable(RCC_SPI1);
	/* B3 = SCK, B4 = MISO, B5 = MOSI, B8 = SS */
	//AFIO_MAPR |= AFIO_MAPR_SPI1_REMAP;
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
		      GPIO5 | GPIO7);
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
		      GPIO3 | GPIO5);
	gpio_set_mode(GPIOB, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT,
		      GPIO4);
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL,
		      GPIO8);

	spi_reset(SPI1);
	spi_init_master(SPI1, SPI_CR1_BAUDRATE_FPCLK_DIV_4, SPI_CR1_CPOL_CLK_TO_0_WHEN_IDLE,
			SPI_CR1_CPHA_CLK_TRANSITION_1, SPI_CR1_DFF_16BIT, SPI_CR1_MSBFIRST);
	//spi_set_clock_phase_0(SPI1);
	//spi_set_clock_polarity_0(SPI1);
	spi_enable_software_slave_management(SPI1);
	spi_set_unidirectional_mode(SPI1);
	spi_set_full_duplex_mode(SPI1);
	spi_set_nss_high(SPI1);
	spi_enable(SPI1);
}


static void adc_setup(void)
{
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_AFIO);
	rcc_periph_clock_enable(RCC_ADC1);
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, GPIO4);
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, GPIO5);

	/* Make sure the ADC doesn't run during config. */
	adc_power_off(ADC1);

	/* We configure everything for one single conversion. */
	adc_disable_scan_mode(ADC1);
	adc_set_single_conversion_mode(ADC1);
	adc_disable_external_trigger_regular(ADC1);
	adc_set_right_aligned(ADC1);
	adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_28DOT5CYC);

	adc_power_on(ADC1);

	/* Wait for ADC starting up. */
	int i;
	for (i = 0; i < 800000; i++) /* Wait a bit. */
		__asm__("nop");

	adc_reset_calibration(ADC1);
	adc_calibrate(ADC1);
}

static uint16_t read_adc_naiive(uint8_t channel)
{
	uint8_t channel_array[16];
	channel_array[0] = channel;
	adc_set_regular_sequence(ADC1, 1, channel_array);
	adc_start_conversion_direct(ADC1);
	while (!adc_eoc(ADC1));
	uint16_t reg16 = adc_read_regular(ADC1);
	return reg16;
}

#define SLEEP 1
#define WAKE 2

int main(void)
{
	int i;


	rcc_clock_setup_in_hse_8mhz_out_72mhz();
	systick_setup();

	rcc_periph_clock_enable(RCC_GPIOC);

	gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);
	gpio_clear(GPIOC, GPIO13);

	usbd_dev = usbd_init(&st_usbfs_v1_usb_driver, &dev, &config, usb_strings, 3, usbd_control_buffer, sizeof(usbd_control_buffer));
	usbd_register_set_config_callback(usbd_dev, cdcacm_set_config);

	//for (i = 0; i < 0x800000; i++)
	//	__asm__("nop");


	adc_setup();
    servo_init();
	//dac_init();
	//dac_write(0,0,2047); //2047 = 0V at opamp
	//dac_write(0,1,2047);

	//tim_init();
	msleep(5000);
	gpio_set(GPIOC, GPIO13);

	//reset the encoder 
	//timer_set_counter(TIM3, 0x7FFF);
	//timer_set_counter(TIM2, 0x7FFF);

	volatile uint8_t state = SLEEP;
	last_time = system_millis;

	while(1){
		usbd_poll(usbd_dev);
		if(incoming){
			if(state == SLEEP){
				//wake state
				state = WAKE;
				gpio_set(GPIOC, GPIO13);
				//reset encoders
				//timer_set_counter(TIM3, 0x7FFF);
				//timer_set_counter(TIM2, 0x7FFF);
			}
			//send back ( active ) state
			//out_msg.enc1 = timer_get_counter(TIM3) - 0x7FFF;
			//out_msg.enc2 = timer_get_counter(TIM2) - 0x7FFF;
			adc_msg.adc1 = read_adc_naiive(4);
			adc_msg.adc2 = read_adc_naiive(5);
			usbd_ep_write_packet(usbd_dev, 0x82, (char *)&adc_msg, sizeof(adc_msg_t));
			incoming = 0;
		}
		//if 500ms no incoming data then sleep
		if(last_time + 500 < system_millis && state != SLEEP){
			//sleep state
			//dac_write(0,0,2047); //2047 = 0V at opamp
			//dac_write(0,1,2047);
			gpio_clear(GPIOC, GPIO13);
			state = SLEEP;
		}
	}
}
