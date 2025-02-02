/*
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/ps2.h>
#include <zephyr/syscall_handler.h>

static inline int z_vrfy_ps2_config(const struct device *dev,
				    ps2_callback_t callback_isr,
					ps2_resend_callback_t resend_callback_isr)

{
	Z_OOPS(Z_SYSCALL_DRIVER_PS2(dev, config));
	Z_OOPS(Z_SYSCALL_VERIFY_MSG(callback_isr == NULL,
				    "callback not be set from user mode"));
	Z_OOPS(Z_SYSCALL_VERIFY_MSG(resend_callback_isr == NULL,
				    "resend callback not be set from user mode"));
	return z_impl_ps2_config(dev, callback_isr, resend_callback_isr);
}
#include <syscalls/ps2_config_mrsh.c>

static inline int z_vrfy_ps2_write(const struct device *dev, uint8_t value)
{
	Z_OOPS(Z_SYSCALL_DRIVER_PS2(dev, write));
	return z_impl_ps2_write(dev, value);
}
#include <syscalls/ps2_write_mrsh.c>

static inline int z_vrfy_ps2_read(const struct device *dev, uint8_t *value)
{
	Z_OOPS(Z_SYSCALL_DRIVER_PS2(dev, read));
	Z_OOPS(Z_SYSCALL_MEMORY_WRITE(value, sizeof(uint8_t)));
	return z_impl_ps2_read(dev, value);
}
#include <syscalls/ps2_read_mrsh.c>

static inline int z_vrfy_ps2_enable_callback(const struct device *dev)
{
	Z_OOPS(Z_SYSCALL_DRIVER_PS2(dev, enable_callback));
	return z_impl_ps2_enable_callback(dev);
}
#include <syscalls/ps2_enable_callback_mrsh.c>

static inline int z_vrfy_ps2_disable_callback(const struct device *dev)
{
	Z_OOPS(Z_SYSCALL_DRIVER_PS2(dev, disable_callback));
	return z_impl_ps2_disable_callback(dev);
}
#include <syscalls/ps2_disable_callback_mrsh.c>
