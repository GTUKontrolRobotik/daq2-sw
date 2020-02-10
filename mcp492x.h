#ifndef MCP492X
#define MCP492X

#include <stdint.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/stm32/gpio.h>

#define LDAC GPIO10
#define DACCS1 GPIO8
#define DACCS2 GPIO12
#define DACCS3 GPIO13
#define CHIP1 0
#define CHIP2 1
#define CHIP3 2
#define CH_V 0
#define CH_I 1

extern void msleep(uint32_t delay);

void dac_write(int chip_id, int channel, uint16_t value);


#endif
