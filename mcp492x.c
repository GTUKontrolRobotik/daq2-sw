#include "mcp492x.h"

void dac_write(int chip_id, int channel, uint16_t value){
	uint16_t data = 0;
	if(channel == 1) data = 1 << 15;
	data = data | ( 0 << 14); //buffered = 0;
	data = data | ( 1 << 13); //gain = 1;
	data = data | ( 1 << 12); //don't shutdown = 1;
	data = data | ( value & 0x0FFF);

	switch(chip_id){
		case CHIP1:
		gpio_clear(GPIOB, DACCS1);
		break;
		case CHIP2:
		gpio_clear(GPIOB, DACCS2);
		break;
		case CHIP3:
		gpio_clear(GPIOB, DACCS3);
		break;
	}
	spi_xfer(SPI1, data);
	switch(chip_id){
		case CHIP1:
		gpio_set(GPIOB, DACCS1);
		break;
		case CHIP2:
		gpio_set(GPIOB, DACCS2);
		break;
		case CHIP3:
		gpio_set(GPIOB, DACCS3);
		break;
	}
	//msleep(1);
	//gpio_set(GPIOB, LDAC);

}
