#!/bin/sh

upgrade_tool db rk3399_loader_v1.30.130.bin
# upgrade_tool ul rkbin/rk3399_loader_v1.30.130.bin
# upgrade_tool wl 64 idbloader.img
upgrade_tool wl 16384 uboot.img
upgrade_tool wl 24576 trust.img 
upgrade_tool rd