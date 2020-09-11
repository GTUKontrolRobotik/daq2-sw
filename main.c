#include "main.h"

void cdcacm_data_rx_cb(usbd_device *usbd_dev, uint8_t ep)
{
	(void)ep;
	(void)usbd_dev;

	char buf[64];
	int len = usbd_ep_read_packet(usbd_dev, 0x01, buf, 64);

	if (len == sizeof(servo_msg_t)) {
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

int main(void)
{
	int i;


	rcc_clock_setup_in_hse_8mhz_out_72mhz();
	systick_setup();

	//led
	rcc_periph_clock_enable(RCC_GPIOC);
	gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);
	gpio_clear(GPIOC, GPIO13);

	//usb
	usbd_dev = usbd_init(&st_usbfs_v1_usb_driver, &dev, &config, usb_strings,
			             3, usbd_control_buffer, sizeof(usbd_control_buffer));
	usbd_register_set_config_callback(usbd_dev, cdcacm_set_config);

	adc_setup();
    servo_init();

	msleep(5000);
	gpio_set(GPIOC, GPIO13);

	volatile uint8_t state = SLEEP;
	last_time = system_millis;

	while(1){
		usbd_poll(usbd_dev);
		if(incoming){
			if(state == SLEEP){
				//wake state
				state = WAKE;
				gpio_set(GPIOC, GPIO13);
			}
			//send back ( active ) state
			adc_msg.adc1 = read_adc_naiive(4);
			adc_msg.adc2 = read_adc_naiive(5);
			usbd_ep_write_packet(usbd_dev, 0x82, (char *)&adc_msg, sizeof(adc_msg_t));
			incoming = 0;
		}
		//if 200 ms no incoming data then sleep
		if(last_time + 200 < system_millis && state != SLEEP){
			//sleep state
			gpio_clear(GPIOC, GPIO13);
			state = SLEEP;
		}
	}
}
