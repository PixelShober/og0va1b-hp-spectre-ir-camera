// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2024-2025 Linaro Ltd

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define OG0VA1B_LINK_FREQ_500MHZ	(500 * HZ_PER_MHZ)
#define OG0VA1B_LINK_FREQ_400MHZ	(400 * HZ_PER_MHZ)
#define OG0VA1B_MCLK_FREQ_24MHZ		(24 * HZ_PER_MHZ)
#define OG0VA1B_MCLK_FREQ_19_2MHZ	(19200000UL)

#define OG0VA1B_REG_CHIP_ID		CCI_REG24(0x300a)
#define OG0VA1B_CHIP_ID			0xc75645

#define OG0VA1B_REG_MODE_SELECT		CCI_REG8(0x0100)
#define OG0VA1B_MODE_STANDBY		0x00
#define OG0VA1B_MODE_STREAMING		BIT(0)

#define OG0VA1B_REG_SOFTWARE_RST	CCI_REG8(0x0103)
#define OG0VA1B_SOFTWARE_RST		BIT(0)

/* Exposure controls from sensor */
#define OG0VA1B_REG_EXPOSURE		CCI_REG24(0x3500)
#define OG0VA1B_EXPOSURE_MIN		1
#define OG0VA1B_EXPOSURE_MAX_MARGIN	14
#define OG0VA1B_EXPOSURE_STEP		1
#define OG0VA1B_EXPOSURE_DEFAULT	554

/* Analogue gain controls from sensor */
#define OG0VA1B_REG_ANALOGUE_GAIN	CCI_REG16(0x350a)
#define OG0VA1B_ANALOGUE_GAIN_MIN	1
#define OG0VA1B_ANALOGUE_GAIN_MAX	0x1ff
#define OG0VA1B_ANALOGUE_GAIN_STEP	1
#define OG0VA1B_ANALOGUE_GAIN_DEFAULT	16

/* Vertical timing size */
#define OG0VA1B_REG_VTS			CCI_REG16(0x380e)
#define OG0VA1B_VTS_MAX			0xffff

/* Test pattern */
#define OG0VA1B_REG_PRE_ISP		CCI_REG8(0x5e00)
#define OG0VA1B_TEST_PATTERN_ENABLE	BIT(7)

#define to_og0va1b(_sd)			container_of(_sd, struct og0va1b, sd)

static const s64 og0va1b_link_freq_menu[] = {
	OG0VA1B_LINK_FREQ_500MHZ,  /* index 0 = default, matches hardware PLL */
	OG0VA1B_LINK_FREQ_400MHZ,
};

static int og0va1b_mipi_ctrl = -1;
module_param_named(mipi_ctrl, og0va1b_mipi_ctrl, int, 0644);
MODULE_PARM_DESC(mipi_ctrl, "Override sensor register 0x4800 when streaming (-1 keeps table value)");

static int og0va1b_force_dvdd_gpio = -1;
module_param_named(force_dvdd_gpio, og0va1b_force_dvdd_gpio, int, 0644);
MODULE_PARM_DESC(force_dvdd_gpio, "Debug: force a global GPIO number high while sensor is powered (-1 disables)");

static struct gpio_desc *og0va1b_get_forced_dvdd_gpio(struct device *dev)
{
	struct gpio_device *gdev;
	struct gpio_desc *desc = NULL;

	if (og0va1b_force_dvdd_gpio < 0)
		return NULL;

	gdev = gpio_device_find_by_label("INTC1083:00");
	if (!gdev) {
		dev_warn(dev, "GPIO chip INTC1083:00 not found\n");
		return NULL;
	}

	desc = gpio_device_get_desc(gdev, og0va1b_force_dvdd_gpio);
	gpio_device_put(gdev);

	if (IS_ERR(desc)) {
		dev_warn(dev, "GPIO offset %d not found: %ld\n",
			 og0va1b_force_dvdd_gpio, PTR_ERR(desc));
		return NULL;
	}

	return desc;
}

struct og0va1b_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct og0va1b_mode {
	u32 width;	/* Frame width in pixels */
	u32 height;	/* Frame height in pixels */
	u32 hts;	/* Horizontal timing size */
	u32 vts;	/* Default vertical timing size */
	u32 bpp;	/* Bits per pixel */

	const struct og0va1b_reg_list reg_list;	/* Sensor register setting */
};

static const char * const og0va1b_test_pattern_menu[] = {
	"Disabled",
	"Vertical Colour Bars",
};

static const char * const og0va1b_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define OG0VA1B_NUM_SUPPLIES	ARRAY_SIZE(og0va1b_supply_names)

struct og0va1b {
	struct device *dev;
	struct regmap *regmap;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[OG0VA1B_NUM_SUPPLIES];

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl_handler ctrl_handler;

	/* Saved register value */
	u64 pre_isp;
};

static const struct cci_reg_sequence og0va1b_648x488_60fps_mode[] = {
	{ CCI_REG8(0x0302), 0x31 },
	{ CCI_REG8(0x0304), 0x01 },
	{ CCI_REG8(0x0305), 0xe0 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0326), 0xd8 },
	{ CCI_REG8(0x3006), 0x0e },
	{ CCI_REG8(0x300d), 0x08 },
	{ CCI_REG8(0x3018), 0xf0 },
	{ CCI_REG8(0x301c), 0xf0 },
	{ CCI_REG8(0x3040), 0x0f },
	{ CCI_REG8(0x3022), 0x01 },
	{ CCI_REG8(0x3107), 0x40 },
	{ CCI_REG8(0x3216), 0x01 },
	{ CCI_REG8(0x3217), 0x00 },
	{ CCI_REG8(0x3218), 0xc0 },
	{ CCI_REG8(0x3219), 0x55 },
	{ CCI_REG8(0x3500), 0x00 },
	{ CCI_REG8(0x3501), 0x01 },
	{ CCI_REG8(0x3502), 0xfe },
	{ CCI_REG8(0x3506), 0x01 },
	{ CCI_REG8(0x3507), 0x50 },
	{ CCI_REG8(0x3508), 0x01 },
	{ CCI_REG8(0x3509), 0x00 },
	{ CCI_REG8(0x350a), 0x01 },
	{ CCI_REG8(0x350b), 0x00 },
	{ CCI_REG8(0x350c), 0x00 },
	{ CCI_REG8(0x3541), 0x00 },
	{ CCI_REG8(0x3542), 0x40 },
	{ CCI_REG8(0x3605), 0x90 },
	{ CCI_REG8(0x3606), 0x41 },
	{ CCI_REG8(0x3612), 0x00 },
	{ CCI_REG8(0x3620), 0x08 },
	{ CCI_REG8(0x3630), 0x17 },
	{ CCI_REG8(0x3631), 0x99 },
	{ CCI_REG8(0x3639), 0x88 },
	{ CCI_REG8(0x3668), 0x08 },
	{ CCI_REG8(0x3674), 0x00 },
	{ CCI_REG8(0x3677), 0x3f },
	{ CCI_REG8(0x368f), 0x06 },
	{ CCI_REG8(0x36a2), 0x19 },
	{ CCI_REG8(0x36a4), 0xf1 },
	{ CCI_REG8(0x36a5), 0x2d },
	{ CCI_REG8(0x3706), 0x30 },
	{ CCI_REG8(0x370d), 0x72 },
	{ CCI_REG8(0x3713), 0x86 },
	{ CCI_REG8(0x3715), 0x03 },
	{ CCI_REG8(0x3716), 0x00 },
	{ CCI_REG8(0x376d), 0x24 },
	{ CCI_REG8(0x3770), 0x3a },
	{ CCI_REG8(0x3778), 0x00 },
	{ CCI_REG8(0x37a8), 0x03 },
	{ CCI_REG8(0x37a9), 0x00 },
	{ CCI_REG8(0x37df), 0x7d },
	{ CCI_REG8(0x3800), 0x00 },
	{ CCI_REG8(0x3801), 0x00 },
	{ CCI_REG8(0x3802), 0x00 },
	{ CCI_REG8(0x3803), 0x00 },
	{ CCI_REG8(0x3804), 0x02 },
	{ CCI_REG8(0x3805), 0x8f },
	{ CCI_REG8(0x3806), 0x01 },
	{ CCI_REG8(0x3807), 0xef },
	{ CCI_REG8(0x3808), 0x02 },
	{ CCI_REG8(0x3809), 0x88 },
	{ CCI_REG8(0x380a), 0x01 },
	{ CCI_REG8(0x380b), 0xe8 },
	{ CCI_REG8(0x380c), 0x01 },
	{ CCI_REG8(0x380d), 0x78 },
	{ CCI_REG8(0x380e), 0x02 },
	{ CCI_REG8(0x380f), 0x0c },
	{ CCI_REG8(0x3810), 0x00 },
	{ CCI_REG8(0x3811), 0x04 },
	{ CCI_REG8(0x3812), 0x00 },
	{ CCI_REG8(0x3813), 0x04 },
	{ CCI_REG8(0x3814), 0x11 },
	{ CCI_REG8(0x3815), 0x11 },
	{ CCI_REG8(0x3816), 0x00 },
	{ CCI_REG8(0x3817), 0x01 },
	{ CCI_REG8(0x3818), 0x00 },
	{ CCI_REG8(0x3819), 0x05 },
	{ CCI_REG8(0x3820), 0x40 },
	{ CCI_REG8(0x3821), 0x04 },
	{ CCI_REG8(0x3823), 0x00 },
	{ CCI_REG8(0x3826), 0x00 },
	{ CCI_REG8(0x3827), 0x00 },
	{ CCI_REG8(0x382b), 0x52 },
	{ CCI_REG8(0x384a), 0xa2 },
	{ CCI_REG8(0x3858), 0x00 },
	{ CCI_REG8(0x3859), 0x00 },
	{ CCI_REG8(0x3860), 0x00 },
	{ CCI_REG8(0x3861), 0x00 },
	{ CCI_REG8(0x3866), 0x0c },
	{ CCI_REG8(0x3867), 0x07 },
	{ CCI_REG8(0x3884), 0x00 },
	{ CCI_REG8(0x3885), 0x08 },
	{ CCI_REG8(0x3888), 0x50 },
	{ CCI_REG8(0x3893), 0x6c },
	{ CCI_REG8(0x3898), 0x00 },
	{ CCI_REG8(0x389a), 0x04 },
	{ CCI_REG8(0x389b), 0x01 },
	{ CCI_REG8(0x389c), 0x0b },
	{ CCI_REG8(0x389d), 0xdc },
	{ CCI_REG8(0x389f), 0x08 },
	{ CCI_REG8(0x38a0), 0x00 },
	{ CCI_REG8(0x38a1), 0x00 },
	{ CCI_REG8(0x38b1), 0x04 },
	{ CCI_REG8(0x38b2), 0x00 },
	{ CCI_REG8(0x38b3), 0x08 },
	{ CCI_REG8(0x38c1), 0x46 },
	{ CCI_REG8(0x38c9), 0x02 },
	{ CCI_REG8(0x38d4), 0x06 },
	{ CCI_REG8(0x38d5), 0x5a },
	{ CCI_REG8(0x38d6), 0x08 },
	{ CCI_REG8(0x38d7), 0x3a },
	{ CCI_REG8(0x391e), 0x00 },
	{ CCI_REG8(0x391f), 0x00 },
	{ CCI_REG8(0x3920), 0xa5 },
	{ CCI_REG8(0x3921), 0x00 },
	{ CCI_REG8(0x3922), 0x00 },
	{ CCI_REG8(0x3923), 0x00 },
	{ CCI_REG8(0x3924), 0x05 },
	{ CCI_REG8(0x3925), 0x00 },
	{ CCI_REG8(0x3926), 0x00 },
	{ CCI_REG8(0x3927), 0x00 },
	{ CCI_REG8(0x3928), 0x1a },
	{ CCI_REG8(0x3929), 0x01 },
	{ CCI_REG8(0x392a), 0xb4 },
	{ CCI_REG8(0x392b), 0x00 },
	{ CCI_REG8(0x392c), 0x10 },
	{ CCI_REG8(0x392f), 0x40 },
	{ CCI_REG8(0x3a06), 0x06 },
	{ CCI_REG8(0x3a07), 0x78 },
	{ CCI_REG8(0x3a08), 0x08 },
	{ CCI_REG8(0x3a09), 0x80 },
	{ CCI_REG8(0x3a52), 0x00 },
	{ CCI_REG8(0x3a53), 0x01 },
	{ CCI_REG8(0x3a54), 0x0c },
	{ CCI_REG8(0x3a55), 0x04 },
	{ CCI_REG8(0x3a58), 0x0c },
	{ CCI_REG8(0x3a59), 0x04 },
	{ CCI_REG8(0x4000), 0xcf },
	{ CCI_REG8(0x4003), 0x40 },
	{ CCI_REG8(0x4008), 0x04 },
	{ CCI_REG8(0x4009), 0x13 },
	{ CCI_REG8(0x400a), 0x02 },
	{ CCI_REG8(0x400b), 0x34 },
	{ CCI_REG8(0x4010), 0x71 },
	{ CCI_REG8(0x4042), 0xc3 },
	{ CCI_REG8(0x4306), 0x04 },
	{ CCI_REG8(0x4307), 0x12 },
	{ CCI_REG8(0x4500), 0x70 },
	{ CCI_REG8(0x4509), 0x00 },
	{ CCI_REG8(0x450b), 0x83 },
	{ CCI_REG8(0x4604), 0x68 },
	{ CCI_REG8(0x481b), 0x44 },
	{ CCI_REG8(0x481f), 0x30 },
	{ CCI_REG8(0x4823), 0x44 },
	{ CCI_REG8(0x4825), 0x35 },
	{ CCI_REG8(0x4f00), 0x04 },
	{ CCI_REG8(0x4f10), 0x04 },
	{ CCI_REG8(0x4f21), 0x01 },
	{ CCI_REG8(0x4f22), 0x00 },
	{ CCI_REG8(0x4f23), 0x54 },
	{ CCI_REG8(0x4f24), 0x51 },
	{ CCI_REG8(0x4f25), 0x41 },
	{ CCI_REG8(0x5000), 0x3f },
	{ CCI_REG8(0x5001), 0x80 },
	{ CCI_REG8(0x500a), 0x00 },
	{ CCI_REG8(0x5100), 0x00 },
	{ CCI_REG8(0x5111), 0x20 },
	{ CCI_REG8(0x3020), 0x20 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0323), 0x01 },
	{ CCI_REG8(0x0304), 0x01 },
	{ CCI_REG8(0x0305), 0x90 },
	{ CCI_REG8(0x0324), 0x01 },
	{ CCI_REG8(0x0325), 0x0e },
	{ CCI_REG8(0x380c), 0x03 },
	{ CCI_REG8(0x380d), 0x84 },
	{ CCI_REG8(0x380e), 0x03 },
	{ CCI_REG8(0x380f), 0x76 },
	{ CCI_REG8(0x4837), 0x11 },
	{ CCI_REG8(0x3501), 0x00 },
	{ CCI_REG8(0x3502), 0x01 },
	{ CCI_REG8(0x3920), 0xaa },
	{ CCI_REG8(0x389f), 0x08 },
	{ CCI_REG8(0x38a0), 0x00 },
	{ CCI_REG8(0x38a1), 0x00 },
	{ CCI_REG8(0x3921), 0x00 },
	{ CCI_REG8(0x3922), 0x00 },
	{ CCI_REG8(0x3923), 0x00 },
	{ CCI_REG8(0x3924), 0x00 },
	{ CCI_REG8(0x3925), 0x00 },
	{ CCI_REG8(0x3926), 0x00 },
	{ CCI_REG8(0x3927), 0x03 },
	{ CCI_REG8(0x3928), 0x68 },
	{ CCI_REG8(0x3929), 0x00 },
	{ CCI_REG8(0x392a), 0x07 },
	{ CCI_REG8(0x392f), 0x4b },
	{ CCI_REG8(0x392d), 0x03 },
	{ CCI_REG8(0x392e), 0x84 },
	{ CCI_REG8(0x391e), 0x00 },
};

static const struct cci_reg_sequence og0va1b_stream_start_regs[] = {
	{ CCI_REG8(0x3107), 0x00 },
	{ CCI_REG8(0x3208), 0x04 },
	{ CCI_REG8(0x4244), 0x01 },
	{ CCI_REG8(0x3208), 0x14 },
	{ CCI_REG8(0x3107), 0x00 },
	{ CCI_REG8(0x3217), 0xbb },
	{ CCI_REG8(0x3216), 0x01 },
	{ CCI_REG8(0x3218), 0xa2 },
	{ CCI_REG8(0x3219), 0x55 },
	{ CCI_REG8(0x3858), 0x01 },
	{ CCI_REG8(0x3859), 0x00 },
	{ CCI_REG8(0x4306), 0x05 },
	{ CCI_REG8(0x4307), 0x12 },
	{ CCI_REG8(0x4604), 0x48 },
	{ CCI_REG8(0x500a), 0x80 },
	{ CCI_REG8(0x5111), 0x40 },
};

static const struct og0va1b_mode supported_modes[] = {
	{
		.width = 648,
		.height = 488,
		.hts = 900,
		.vts = 886,
		.bpp = 10,
		.reg_list = {
			.regs = og0va1b_648x488_60fps_mode,
			.num_regs = ARRAY_SIZE(og0va1b_648x488_60fps_mode),
		},
	},
};

static int og0va1b_enable_test_pattern(struct og0va1b *og0va1b, u32 pattern)
{
	u64 val = og0va1b->pre_isp;

	if (pattern)
		val |= OG0VA1B_TEST_PATTERN_ENABLE;
	else
		val &= ~OG0VA1B_TEST_PATTERN_ENABLE;

	return cci_write(og0va1b->regmap, OG0VA1B_REG_PRE_ISP, val, NULL);
}

static int og0va1b_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct og0va1b *og0va1b = container_of(ctrl->handler, struct og0va1b,
					       ctrl_handler);
	const struct og0va1b_mode *mode = &supported_modes[0];
	s64 exposure_max;
	int ret;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		exposure_max = ctrl->val + mode->height -
			OG0VA1B_EXPOSURE_MAX_MARGIN;
		ret = __v4l2_ctrl_modify_range(og0va1b->exposure,
					og0va1b->exposure->minimum,
					exposure_max,
					og0va1b->exposure->step,
					og0va1b->exposure->default_value);
		if (ret)
			return ret;
	}

	/* V4L2 controls are applied, when sensor is powered up for streaming */
	if (!pm_runtime_get_if_active(og0va1b->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(og0va1b->regmap, OG0VA1B_REG_ANALOGUE_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(og0va1b->regmap, OG0VA1B_REG_EXPOSURE,
				ctrl->val << 4, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(og0va1b->regmap, OG0VA1B_REG_VTS,
				ctrl->val + mode->height, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = og0va1b_enable_test_pattern(og0va1b, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(og0va1b->dev);

	return ret;
}

static const struct v4l2_ctrl_ops og0va1b_ctrl_ops = {
	.s_ctrl = og0va1b_set_ctrl,
};

static int og0va1b_init_controls(struct og0va1b *og0va1b)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &og0va1b->ctrl_handler;
	const struct og0va1b_mode *mode = &supported_modes[0];
	s64 exposure_max, pixel_rate, h_blank, v_blank;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 9);

	ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr, &og0va1b_ctrl_ops,
				      V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(og0va1b_link_freq_menu) - 1,
				      0, og0va1b_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = og0va1b_link_freq_menu[0] * 2 / mode->bpp;
	v4l2_ctrl_new_std(ctrl_hdlr, &og0va1b_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  0, pixel_rate, 1, pixel_rate);

	h_blank = mode->hts - mode->width;
	ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &og0va1b_ctrl_ops, V4L2_CID_HBLANK,
				 h_blank, h_blank, 1, h_blank);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v_blank = mode->vts - mode->height;
	og0va1b->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &og0va1b_ctrl_ops,
					    V4L2_CID_VBLANK, v_blank,
					    OG0VA1B_VTS_MAX - mode->height, 1,
					    v_blank);

	v4l2_ctrl_new_std(ctrl_hdlr, &og0va1b_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  OG0VA1B_ANALOGUE_GAIN_MIN, OG0VA1B_ANALOGUE_GAIN_MAX,
			  OG0VA1B_ANALOGUE_GAIN_STEP,
			  OG0VA1B_ANALOGUE_GAIN_DEFAULT);

	exposure_max = mode->vts - OG0VA1B_EXPOSURE_MAX_MARGIN;
	og0va1b->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &og0va1b_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      OG0VA1B_EXPOSURE_MIN,
					      exposure_max,
					      OG0VA1B_EXPOSURE_STEP,
					      OG0VA1B_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &og0va1b_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(og0va1b_test_pattern_menu) - 1,
				     0, 0, og0va1b_test_pattern_menu);

	if (ctrl_hdlr->error)
		return ctrl_hdlr->error;

	ret = v4l2_fwnode_device_parse(og0va1b->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &og0va1b_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	og0va1b->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static void og0va1b_update_pad_format(const struct og0va1b_mode *mode,
				      struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int og0va1b_enable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	const struct og0va1b_reg_list *reg_list = &supported_modes[0].reg_list;
	struct og0va1b *og0va1b = to_og0va1b(sd);
	int ret;

	ret = pm_runtime_resume_and_get(og0va1b->dev);
	if (ret)
		return ret;

	dev_info(og0va1b->dev, "enable_streams called, xvclk=%lu Hz\n",
		 clk_get_rate(og0va1b->xvclk));

	/* Allow MCLK to stabilize before starting any I2C transactions */
	usleep_range(10 * USEC_PER_MSEC, 15 * USEC_PER_MSEC);

	/* Skip a step of explicit entering into the standby mode */
	ret = cci_write(og0va1b->regmap, OG0VA1B_REG_SOFTWARE_RST,
			OG0VA1B_SOFTWARE_RST, NULL);
	if (ret) {
		dev_err(og0va1b->dev, "failed to software reset: %d\n", ret);
		goto error;
	}

	/* Wait for reset to complete before writing registers; without this
	 * the PLL configuration is lost when the reset finishes after writes. */
	usleep_range(50 * USEC_PER_MSEC, 55 * USEC_PER_MSEC);

	ret = cci_multi_reg_write(og0va1b->regmap, reg_list->regs,
				  reg_list->num_regs, NULL);
	if (ret) {
		dev_err(og0va1b->dev, "failed to set mode: %d\n", ret);
		goto error;
	}

	/* Verify PLL register writes immediately after init table */
	{
		u64 vr = 0;
		cci_read(og0va1b->regmap, CCI_REG8(0x0305), &vr, NULL);
		dev_info(og0va1b->dev, "post-init 0x0305=0x%02llx\n", vr);
		vr = 0;
		cci_read(og0va1b->regmap, CCI_REG8(0x0325), &vr, NULL);
		dev_info(og0va1b->dev, "post-init 0x0325=0x%02llx\n", vr);
		vr = 0;
		cci_read(og0va1b->regmap, CCI_REG8(0x4837), &vr, NULL);
		dev_info(og0va1b->dev, "post-init 0x4837=0x%02llx\n", vr);
	}

	if (og0va1b_mipi_ctrl >= 0) {
		ret = cci_write(og0va1b->regmap, CCI_REG8(0x4800),
				og0va1b_mipi_ctrl & 0xff, NULL);
		if (ret) {
			dev_err(og0va1b->dev, "failed to override 0x4800: %d\n",
				ret);
			goto error;
		}
		dev_info(og0va1b->dev, "0x4800 override=0x%02x\n",
			 og0va1b_mipi_ctrl & 0xff);
	}

	ret = __v4l2_ctrl_handler_setup(og0va1b->sd.ctrl_handler);
	if (ret)
		goto error;

	ret = cci_multi_reg_write(og0va1b->regmap, og0va1b_stream_start_regs,
				  ARRAY_SIZE(og0va1b_stream_start_regs), NULL);
	if (ret) {
		dev_err(og0va1b->dev, "failed to set stream-start regs: %d\n", ret);
		goto error;
	}

	ret = cci_write(og0va1b->regmap, OG0VA1B_REG_MODE_SELECT,
			OG0VA1B_MODE_STREAMING, NULL);
	if (ret) {
		dev_err(og0va1b->dev, "failed to start streaming: %d\n", ret);
		goto error;
	}

	/* Readback to verify sensor state */
	{
		u64 v = 0;
		cci_read(og0va1b->regmap, OG0VA1B_REG_MODE_SELECT, &v, NULL);
		dev_info(og0va1b->dev, "0x0100 readback=0x%02llx\n", v);
		v = 0;
		cci_read(og0va1b->regmap, CCI_REG8(0x4800), &v, NULL);
		dev_info(og0va1b->dev, "0x4800 readback=0x%02llx\n", v);
		v = 0;
		cci_read(og0va1b->regmap, OG0VA1B_REG_CHIP_ID, &v, NULL);
		dev_info(og0va1b->dev, "chip_id readback=0x%06llx\n", v);
		/* PLL and clock status diagnostics */
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x3023), &v, NULL);
		dev_info(og0va1b->dev, "0x3023(PLL lock?)=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x300f), &v, NULL);
		dev_info(og0va1b->dev, "0x300f=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x301c), &v, NULL);
		dev_info(og0va1b->dev, "0x301c=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x30a0), &v, NULL);
		dev_info(og0va1b->dev, "0x30a0=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x30a2), &v, NULL);
		dev_info(og0va1b->dev, "0x30a2(PLL_mult?)=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x30a3), &v, NULL);
		dev_info(og0va1b->dev, "0x30a3(PLL_prediv?)=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x3082), &v, NULL);
		dev_info(og0va1b->dev, "0x3082=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x3083), &v, NULL);
		dev_info(og0va1b->dev, "0x3083=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x3016), &v, NULL);
		dev_info(og0va1b->dev, "0x3016=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x3037), &v, NULL);
		dev_info(og0va1b->dev, "0x3037=0x%02llx\n", v);
		/* MIPI lane state */
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x4801), &v, NULL);
		dev_info(og0va1b->dev, "0x4801=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x4806), &v, NULL);
		dev_info(og0va1b->dev, "0x4806=0x%02llx\n", v);
		/* Extra status: scan 0x3000-0x300e for any non-zero status regs */
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x3000), &v, NULL);
		dev_info(og0va1b->dev, "0x3000=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x3001), &v, NULL);
		dev_info(og0va1b->dev, "0x3001=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x3002), &v, NULL);
		dev_info(og0va1b->dev, "0x3002=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x302a), &v, NULL);
		dev_info(og0va1b->dev, "0x302a=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x302b), &v, NULL);
		dev_info(og0va1b->dev, "0x302b=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x302c), &v, NULL);
		dev_info(og0va1b->dev, "0x302c=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x302d), &v, NULL);
		dev_info(og0va1b->dev, "0x302d=0x%02llx\n", v);
		/* MIPI phy status */
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x4840), &v, NULL);
		dev_info(og0va1b->dev, "0x4840(phy_status?)=0x%02llx\n", v);
		v = 0; cci_read(og0va1b->regmap, CCI_REG8(0x4841), &v, NULL);
		dev_info(og0va1b->dev, "0x4841=0x%02llx\n", v);
	}

	return 0;

error:
	pm_runtime_put_autosuspend(og0va1b->dev);

	return ret;
}

static int og0va1b_disable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state, u32 pad,
				   u64 streams_mask)
{
	struct og0va1b *og0va1b = to_og0va1b(sd);
	int ret;

	ret = cci_write(og0va1b->regmap, OG0VA1B_REG_MODE_SELECT,
			OG0VA1B_MODE_STANDBY, NULL);
	if (ret)
		dev_err(og0va1b->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(og0va1b->dev);

	return ret;
}

static int og0va1b_set_pad_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *format;
	const struct og0va1b_mode *mode;

	format = v4l2_subdev_state_get_format(state, 0);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width,
				      fmt->format.height);

	og0va1b_update_pad_format(mode, &fmt->format);
	*format = fmt->format;

	return 0;
}

static int og0va1b_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;

	return 0;
}

static int og0va1b_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}


static int og0va1b_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = supported_modes[0].width;
		sel->r.height = supported_modes[0].height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int og0va1b_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
				.code = MEDIA_BUS_FMT_SGRBG10_1X10,
			.width = supported_modes[0].width,
			.height = supported_modes[0].height,
		},
	};

	og0va1b_set_pad_format(sd, state, &fmt);

	return 0;
}

static const struct v4l2_subdev_video_ops og0va1b_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops og0va1b_pad_ops = {
	.set_fmt = og0va1b_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = og0va1b_get_selection,
	.enum_mbus_code = og0va1b_enum_mbus_code,
	.enum_frame_size = og0va1b_enum_frame_size,
	.enable_streams = og0va1b_enable_streams,
	.disable_streams = og0va1b_disable_streams,
};

static const struct v4l2_subdev_ops og0va1b_subdev_ops = {
	.video = &og0va1b_video_ops,
	.pad = &og0va1b_pad_ops,
};

static const struct v4l2_subdev_internal_ops og0va1b_internal_ops = {
	.init_state = og0va1b_init_state,
};

static const struct media_entity_operations og0va1b_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int og0va1b_identify_sensor(struct og0va1b *og0va1b)
{
	u64 val;
	int ret;

	ret = cci_read(og0va1b->regmap, OG0VA1B_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(og0va1b->dev, "failed to read chip id: %d\n", ret);
		return ret;
	}

	if (0) /* chip ID check disabled for OG0VA1B probe */ {
		dev_err(og0va1b->dev, "chip id mismatch: %x!=%llx\n",
			OG0VA1B_CHIP_ID, val);
		return -ENODEV;
	}

	ret = cci_read(og0va1b->regmap, OG0VA1B_REG_PRE_ISP,
		       &og0va1b->pre_isp, NULL);
	if (ret)
		dev_err(og0va1b->dev, "failed to read pre_isp: %d\n", ret);

	return ret;
}

static int og0va1b_check_hwcfg(struct og0va1b *og0va1b)
{
	struct fwnode_handle *fwnode = dev_fwnode(og0va1b->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	ret = v4l2_link_freq_to_bitmap(og0va1b->dev,
				       bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       og0va1b_link_freq_menu,
				       ARRAY_SIZE(og0va1b_link_freq_menu),
				       &freq_bitmap);

	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int og0va1b_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct og0va1b *og0va1b = to_og0va1b(sd);
	int ret;

	ret = regulator_bulk_enable(OG0VA1B_NUM_SUPPLIES, og0va1b->supplies);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(og0va1b->reset_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 15 * USEC_PER_MSEC);

	if (og0va1b_force_dvdd_gpio >= 0) {
		struct gpio_desc *dvdd_gpio;

		dvdd_gpio = og0va1b_get_forced_dvdd_gpio(og0va1b->dev);
		if (dvdd_gpio) {
			ret = gpiod_direction_output_raw(dvdd_gpio, 1);
			if (ret)
				dev_warn(og0va1b->dev,
					 "failed to force INTC1083:00 GPIO %d high: %d\n",
					 og0va1b_force_dvdd_gpio, ret);
			else
				dev_info(og0va1b->dev, "forced INTC1083:00 GPIO %d high\n",
					 og0va1b_force_dvdd_gpio);
		}
	}

	ret = clk_prepare_enable(og0va1b->xvclk);
	if (ret)
		goto reset_gpio;

	return 0;

reset_gpio:
	gpiod_set_value_cansleep(og0va1b->reset_gpio, 1);

	if (og0va1b_force_dvdd_gpio >= 0) {
		struct gpio_desc *dvdd_gpio;

		dvdd_gpio = og0va1b_get_forced_dvdd_gpio(og0va1b->dev);
		if (dvdd_gpio)
			gpiod_direction_output_raw(dvdd_gpio, 0);
	}

	regulator_bulk_disable(OG0VA1B_NUM_SUPPLIES, og0va1b->supplies);

	return ret;
}

static int og0va1b_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct og0va1b *og0va1b = to_og0va1b(sd);

	clk_disable_unprepare(og0va1b->xvclk);

	gpiod_set_value_cansleep(og0va1b->reset_gpio, 1);

	regulator_bulk_disable(OG0VA1B_NUM_SUPPLIES, og0va1b->supplies);

	return 0;
}

static int og0va1b_probe(struct i2c_client *client)
{
	struct og0va1b *og0va1b;
	unsigned long freq;
	unsigned int i;
	int ret;

	og0va1b = devm_kzalloc(&client->dev, sizeof(*og0va1b), GFP_KERNEL);
	if (!og0va1b)
		return -ENOMEM;

	og0va1b->dev = &client->dev;

	v4l2_i2c_subdev_init(&og0va1b->sd, client, &og0va1b_subdev_ops);

	og0va1b->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(og0va1b->regmap))
		return dev_err_probe(og0va1b->dev, PTR_ERR(og0va1b->regmap),
				     "failed to init CCI\n");

	og0va1b->xvclk = devm_v4l2_sensor_clk_get(og0va1b->dev, NULL);
	if (IS_ERR(og0va1b->xvclk))
		return dev_err_probe(og0va1b->dev, PTR_ERR(og0va1b->xvclk),
				     "failed to get XVCLK clock\n");

	freq = clk_get_rate(og0va1b->xvclk);
	dev_info(&client->dev, "xvclk frequency at probe: %lu Hz\n", freq);
	if (freq && freq != OG0VA1B_MCLK_FREQ_24MHZ && freq != OG0VA1B_MCLK_FREQ_19_2MHZ)
		return dev_err_probe(og0va1b->dev, -EINVAL,
				     "XVCLK clock frequency %lu is not supported\n",
				     freq);

	ret = og0va1b_check_hwcfg(og0va1b);
	if (ret)
		return dev_err_probe(og0va1b->dev, ret,
				     "failed to check HW configuration\n");

	og0va1b->reset_gpio = devm_gpiod_get_optional(og0va1b->dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(og0va1b->reset_gpio))
		return dev_err_probe(og0va1b->dev, PTR_ERR(og0va1b->reset_gpio),
				     "cannot get reset GPIO\n");

	for (i = 0; i < OG0VA1B_NUM_SUPPLIES; i++)
		og0va1b->supplies[i].supply = og0va1b_supply_names[i];

	ret = devm_regulator_bulk_get(og0va1b->dev, OG0VA1B_NUM_SUPPLIES,
				      og0va1b->supplies);
	if (ret)
		return dev_err_probe(og0va1b->dev, ret,
				     "failed to get supply regulators\n");

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = og0va1b_power_on(og0va1b->dev);
	if (ret)
		return ret;

	ret = og0va1b_identify_sensor(og0va1b);
	if (ret) {
		dev_err_probe(og0va1b->dev, ret, "failed to find sensor\n");
		goto power_off;
	}

	ret = og0va1b_init_controls(og0va1b);
	if (ret) {
		dev_err_probe(og0va1b->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	og0va1b->sd.state_lock = og0va1b->ctrl_handler.lock;
	og0va1b->sd.internal_ops = &og0va1b_internal_ops;
	og0va1b->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	og0va1b->sd.entity.ops = &og0va1b_subdev_entity_ops;
	og0va1b->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	og0va1b->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&og0va1b->sd.entity, 1, &og0va1b->pad);
	if (ret) {
		dev_err_probe(og0va1b->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&og0va1b->sd);
	if (ret < 0) {
		dev_err_probe(og0va1b->dev, ret,
			      "failed to init media entity pads\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(og0va1b->dev);
	pm_runtime_enable(og0va1b->dev);

	ret = v4l2_async_register_subdev_sensor(&og0va1b->sd);
	if (ret < 0) {
		dev_err_probe(og0va1b->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	/* Enable runtime PM and turn off the device */
	pm_runtime_idle(og0va1b->dev);
	pm_runtime_set_autosuspend_delay(og0va1b->dev, 1000);
	pm_runtime_use_autosuspend(og0va1b->dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&og0va1b->sd);
	pm_runtime_disable(og0va1b->dev);
	pm_runtime_set_suspended(og0va1b->dev);

media_entity_cleanup:
	media_entity_cleanup(&og0va1b->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(og0va1b->sd.ctrl_handler);

power_off:
	og0va1b_power_off(og0va1b->dev);

	return ret;
}

static void og0va1b_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct og0va1b *og0va1b = to_og0va1b(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(og0va1b->dev);

	if (!pm_runtime_status_suspended(og0va1b->dev)) {
		og0va1b_power_off(og0va1b->dev);
		pm_runtime_set_suspended(og0va1b->dev);
	}
}

static const struct dev_pm_ops og0va1b_pm_ops = {
	SET_RUNTIME_PM_OPS(og0va1b_power_off, og0va1b_power_on, NULL)
};

static const struct acpi_device_id og0va1b_acpi_match[] = {
	{ "OVTI00AB", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, og0va1b_acpi_match);

static const struct of_device_id og0va1b_of_match[] = {
	{ .compatible = "ovti,og0va1b" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, og0va1b_of_match);

static struct i2c_driver og0va1b_i2c_driver = {
	.driver = {
		.name = "og0va1b",
		.pm = &og0va1b_pm_ops,
		.of_match_table = og0va1b_of_match,
		.acpi_match_table = og0va1b_acpi_match,
	},
	.probe = og0va1b_probe,
	.remove = og0va1b_remove,
};

module_i2c_driver(og0va1b_i2c_driver);

MODULE_AUTHOR("Vladimir Zapolskiy <vladimir.zapolskiy@linaro.org>");
MODULE_DESCRIPTION("OmniVision OG0VA1B sensor driver");
MODULE_LICENSE("GPL");
