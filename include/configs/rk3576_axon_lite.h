
/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2026 Vicharak India
 */

#ifndef __rk3576_AXON_LITE_H
#define __rk3576_AXON_LITE_H

#include <configs/vicharak_common_defs.h>

#define ROCKCHIP_DEVICE_SETTINGS \
		"stdout=serial,vidconsole\0" \
		"stderr=serial,vidconsole\0" \
		"stdin=usbkbd,serial\0" \
		VICHARAK_BOOT_MENU

#define CONFIG_SYS_MMC_ENV_DEV		0	/* eMMC */

#include <configs/rk3576_common.h>

#endif /* __rk3576_AXON_LITE_H */
