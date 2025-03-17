
#include <common.h>
#include <asm/io.h>
#include <command.h>
#include <display_options.h>
#include <dm.h>
#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <misc.h>
#include <asm/arch/rockchip_smccc.h>


#define T_CSB_P_S		0
#define T_PGENB_P_S		0
#define T_LOAD_P_S		0
#define T_ADDR_P_S		0
#define T_STROBE_P_S		(0 + 110) /* 1.1us */
#define T_CSB_P_L		(0 + 110 + 1000 + 20) /* 200ns */
#define T_PGENB_P_L		(0 + 110 + 1000 + 20)
#define T_LOAD_P_L		(0 + 110 + 1000 + 20)
#define T_ADDR_P_L		(0 + 110 + 1000 + 20)
#define T_STROBE_P_L		(0 + 110 + 1000) /* 10us */
#define T_CSB_R_S		0
#define T_PGENB_R_S		0
#define T_LOAD_R_S		0
#define T_ADDR_R_S		2
#define T_STROBE_R_S		(2 + 3)
#define T_CSB_R_L		(2 + 3 + 3 + 3)
#define T_PGENB_R_L		(2 + 3 + 3 + 3)
#define T_LOAD_R_L		(2 + 3 + 3 + 3)
#define T_ADDR_R_L		(2 + 3 + 3 + 2)
#define T_STROBE_R_L		(2 + 3 + 3)

#define T_CSB_P			0x28
#define T_PGENB_P		0x2c
#define T_LOAD_P		0x30
#define T_ADDR_P		0x34
#define T_STROBE_P		0x38
#define T_CSB_R			0x3c
#define T_PGENB_R		0x40
#define T_LOAD_R		0x44
#define T_ADDR_R		0x48
#define T_STROBE_R		0x4c

#define RK1808_USER_MODE	BIT(0)
#define RK1808_INT_FINISH	BIT(0)
#define RK1808_AUTO_ENB		BIT(0)
#define RK1808_AUTO_RD		BIT(1)
#define RK1808_A_SHIFT		16
#define RK1808_A_MASK		0x3ff
#define RK1808_NBYTES		4

#define RK3399_A_SHIFT          16
#define RK3399_A_MASK           0x3ff
#define RK3399_NFUSES           32
#define RK3399_BYTES_PER_FUSE   4
#define RK3399_STROBSFTSEL      BIT(9)
#define RK3399_RSB              BIT(7)
#define RK3399_PD               BIT(5)
#define RK3399_PS 			  BIT(4)
#define RK3399_PGENB            BIT(3)
#define RK3399_LOAD             BIT(2)
#define RK3399_STROBE           BIT(1)
#define RK3399_CSB              BIT(0)

#define RK3288_A_SHIFT          6
#define RK3288_A_MASK           0x3ff
#define RK3288_NFUSES           32
#define RK3288_BYTES_PER_FUSE   1
#define RK3288_PGENB            BIT(3)
#define RK3288_LOAD             BIT(2)
#define RK3288_STROBE           BIT(1)
#define RK3288_CSB              BIT(0)

#define RK3328_INT_STATUS	0x0018
#define RK3328_DOUT		0x0020
#define RK3328_AUTO_CTRL	0x0024
#define RK3328_INT_FINISH	BIT(0)
#define RK3328_AUTO_ENB		BIT(0)
#define RK3328_AUTO_RD		BIT(1)

typedef int (*EFUSE_READ)(struct udevice *dev, int offset, void *buf, int size);
typedef int (*EFUSE_WRITE)(struct udevice *dev,int offset,const 	void *buf,int size);


struct rockchip_efuse_regs {
	u32 ctrl;      /* 0x00  efuse control register */
	u32 dout;      /* 0x04  efuse data out register */
	u32 rf;        /* 0x08  efuse redundancy bit used register */
	u32 _rsvd0;
	u32 jtag_pass; /* 0x10  JTAG password */
	u32 strobe_finish_ctrl;
		       /* 0x14	efuse strobe finish control register */
	u32 int_status;/* 0x18 */
	u32 reserved;  /* 0x1c */
	u32 dout2;     /* 0x20 */
	u32 auto_ctrl; /* 0x24 */
};

struct rockchip_efuse_platdata {
	void __iomem *base;
	struct clk *clk;
};

struct efuse_rw_data {
	EFUSE_READ read_ptr;
	EFUSE_WRITE write_ptr;
};