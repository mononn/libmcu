/*
 * SPDX-FileCopyrightText: 2022 Kyunghwan Kwon <k@mononn.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "libmcu/ao_overrides.h"
#include "libmcu/compiler.h"

LIBMCU_WEAK
void ao_lock(void *ctx)
{
	unused(ctx);
}

LIBMCU_WEAK
void ao_unlock(void *ctx)
{
	unused(ctx);
}

LIBMCU_WEAK
void ao_timer_lock(void)
{
}

LIBMCU_WEAK
void ao_timer_unlock(void)
{
}

LIBMCU_WEAK
void ao_timer_lock_init(void)
{
}
