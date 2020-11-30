/*
 * Copyright (c) 2016 Linaro Limited.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <kernel.h>
#include <device.h>
#include <init.h>
#include <drivers/pinmux.h>
#include <sys/sys_io.h>

#include <pinmux/stm32/pinmux_stm32.h>

/* pin assignments for STM32F429I-DISC1 board */
static const struct pin_config pinconf[] = {
#if DT_NODE_HAS_STATUS(DT_NODELABEL(usart1), okay) && CONFIG_SERIAL
	{ STM32_PIN_PA9, STM32F4_PINMUX_FUNC_PA9_USART1_TX },
	{ STM32_PIN_PA10, STM32F4_PINMUX_FUNC_PA10_USART1_RX },
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart4), okay) && CONFIG_SERIAL
	{ STM32_PIN_PC10, STM32F4_PINMUX_FUNC_PC10_UART4_TX },
	{ STM32_PIN_PC11, STM32F4_PINMUX_FUNC_PC11_UART4_RX },
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(usart2), okay) && CONFIG_SERIAL
	{ STM32_PIN_PD5, STM32F4_PINMUX_FUNC_PD5_USART2_TX },
	{ STM32_PIN_PD6, STM32F4_PINMUX_FUNC_PD6_USART2_RX },
#endif
#if CONFIG_CAN
	{ STM32_PIN_PB8, STM32F4_PINMUX_FUNC_PB8_CAN1_RX },
	{ STM32_PIN_PB9, STM32F4_PINMUX_FUNC_PB9_CAN1_TX },
#endif
#if CONFIG_I2C
	{ STM32_PIN_PB6, STM32F4_PINMUX_FUNC_PB6_I2C1_SCL },
	{ STM32_PIN_PB7, STM32F4_PINMUX_FUNC_PB7_I2C1_SDA },
#endif
};

static int pinmux_stm32_init(struct device *port)
{
	ARG_UNUSED(port);

	stm32_setup_pins(pinconf, ARRAY_SIZE(pinconf));

	return 0;
}

SYS_INIT(pinmux_stm32_init, PRE_KERNEL_1,
		CONFIG_PINMUX_STM32_DEVICE_INITIALIZATION_PRIORITY);
