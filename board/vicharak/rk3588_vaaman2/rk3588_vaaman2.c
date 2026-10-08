/*
 * SPDX-License-Identifier:     GPL-2.0+
 *
 * (C) Copyright 2021 Rockchip Electronics Co., Ltd
 */

#include <common.h>
#include <console.h>

DECLARE_GLOBAL_DATA_PTR;

int rk_board_late_init(void)
{
	console_magic_match = is_mmc_magic_match();

	return 0;
}
