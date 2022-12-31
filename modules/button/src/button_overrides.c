/*
 * SPDX-FileCopyrightText: 2022 Kyunghwan Kwon <k@mononn.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "libmcu/button_overrides.h"
#include "libmcu/compiler.h"
#include <pthread.h>

LIBMCU_WEAK
void button_lock(void)
{
}

LIBMCU_WEAK
void button_unlock(void)
{
}
