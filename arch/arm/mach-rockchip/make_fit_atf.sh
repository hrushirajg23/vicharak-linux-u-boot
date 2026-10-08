#!/bin/bash
#
# Copyright (C) 2020 Rockchip Electronics Co., Ltd
#
# SPDX-License-Identifier:     GPL-2.0+
#

source ./${srctree}/arch/arm/mach-rockchip/fit_nodes.sh

gen_header
gen_uboot_node
gen_bl31_node
# VSAF: upstream gated this behind CONFIG_ANDROID_BOOTLOADER, which the
# Linux (non-Android) axon defconfig never sets, so OP-TEE silently never
# made it into the FIT on a stock Linux build. gen_bl32_node() already
# no-ops safely on its own if TEE_LOAD_ADDR is unset, so the guard was
# redundant even for its original purpose. Call unconditionally.
gen_bl32_node
gen_mcu_node
gen_loadable_node
gen_kfdt_node
gen_fdt_node
gen_arm64_configurations
