// SPDX-License-Identifier: GPL-2.0+
/*
 * Azoteq IQS269A Capacitive Touch Controller
 *
 * Copyright (C) 2020 Jeff LaBundy <jeff@labundy.com>
 *
 * This driver registers up to 3 input devices: one representing capacitive or
 * inductive keys as well as Hall-effect switches, and one for each of the two
 * axial sliders presented by the device.
 */

#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define IQS269_VER_INFO				0x00
#define IQS269_VER_INFO_PROD_NUM		0x4F
#define IQS269_VER_INFO_FW_NUM_2		0x03
#define IQS269_VER_INFO_FW_NUM_3		0x10

#define IQS269_SYS_FLAGS			0x02
#define IQS269_SYS_FLAGS_SHOW_RESET		BIT(15)
#define IQS269_SYS_FLAGS_PWR_MODE_MASK		GENMASK(12, 11)
#define IQS269_SYS_FLAGS_PWR_MODE_SHIFT		11
#define IQS269_SYS_FLAGS_IN_ATI			BIT(10)

#define IQS269_CHx_COUNTS			0x08

#define IQS269_SLIDER_X				0x30

#define IQS269_CAL_DATA_A			0x35
#define IQS269_CAL_DATA_A_HALL_BIN_L_MASK	GENMASK(15, 12)
#define IQS269_CAL_DATA_A_HALL_BIN_L_SHIFT	12
#define IQS269_CAL_DATA_A_HALL_BIN_R_MASK	GENMASK(11, 8)
#define IQS269_CAL_DATA_A_HALL_BIN_R_SHIFT	8

#define IQS269_SYS_SETTINGS			0x80
#define IQS269_SYS_SETTINGS_CLK_DIV		BIT(15)
#define IQS269_SYS_SETTINGS_ULP_AUTO		BIT(14)
#define IQS269_SYS_SETTINGS_DIS_AUTO		BIT(13)
#define IQS269_SYS_SETTINGS_PWR_MODE_MASK	GENMASK(12, 11)
#define IQS269_SYS_SETTINGS_PWR_MODE_SHIFT	11
#define IQS269_SYS_SETTINGS_PWR_MODE_MAX	3
#define IQS269_SYS_SETTINGS_ULP_UPDATE_MASK	GENMASK(10, 8)
#define IQS269_SYS_SETTINGS_ULP_UPDATE_SHIFT	8
#define IQS269_SYS_SETTINGS_ULP_UPDATE_MAX	7
#define IQS269_SYS_SETTINGS_SLIDER_SWIPE	BIT(7)
#define IQS269_SYS_SETTINGS_RESEED_OFFSET	BIT(6)
#define IQS269_SYS_SETTINGS_EVENT_MODE		BIT(5)
#define IQS269_SYS_SETTINGS_EVENT_MODE_LP	BIT(4)
#define IQS269_SYS_SETTINGS_REDO_ATI		BIT(2)
#define IQS269_SYS_SETTINGS_ACK_RESET		BIT(0)

#define IQS269_FILT_STR_LP_LTA_MASK		GENMASK(7, 6)
#define IQS269_FILT_STR_LP_LTA_SHIFT		6
#define IQS269_FILT_STR_LP_CNT_MASK		GENMASK(5, 4)
#define IQS269_FILT_STR_LP_CNT_SHIFT		4
#define IQS269_FILT_STR_NP_LTA_MASK		GENMASK(3, 2)
#define IQS269_FILT_STR_NP_LTA_SHIFT		2
#define IQS269_FILT_STR_NP_CNT_MASK		GENMASK(1, 0)
#define IQS269_FILT_STR_MAX			3

#define IQS269_EVENT_MASK_SYS			BIT(6)
#define IQS269_EVENT_MASK_GESTURE		BIT(3)
#define IQS269_EVENT_MASK_DEEP			BIT(2)
#define IQS269_EVENT_MASK_TOUCH			BIT(1)
#define IQS269_EVENT_MASK_PROX			BIT(0)

#define IQS269_RATE_NP_MS_MAX			255
#define IQS269_RATE_LP_MS_MAX			255
#define IQS269_RATE_ULP_MS_MAX			4080
#define IQS269_TIMEOUT_PWR_MS_MAX		130560
#define IQS269_TIMEOUT_LTA_MS_MAX		130560

#define IQS269_MISC_A_ATI_BAND_DISABLE		BIT(15)
#define IQS269_MISC_A_ATI_LP_ONLY		BIT(14)
#define IQS269_MISC_A_ATI_BAND_TIGHTEN		BIT(13)
#define IQS269_MISC_A_FILT_DISABLE		BIT(12)
#define IQS269_MISC_A_GPIO3_SELECT_MASK		GENMASK(10, 8)
#define IQS269_MISC_A_GPIO3_SELECT_SHIFT	8
#define IQS269_MISC_A_DUAL_DIR			BIT(6)
#define IQS269_MISC_A_TX_FREQ_MASK		GENMASK(5, 4)
#define IQS269_MISC_A_TX_FREQ_SHIFT		4
#define IQS269_MISC_A_TX_FREQ_MAX		3
#define IQS269_MISC_A_GLOBAL_CAP_SIZE		BIT(0)

#define IQS269_MISC_B_RESEED_UI_SEL_MASK	GENMASK(7, 6)
#define IQS269_MISC_B_RESEED_UI_SEL_SHIFT	6
#define IQS269_MISC_B_RESEED_UI_SEL_MAX		3
#define IQS269_MISC_B_TRACKING_UI_ENABLE	BIT(4)
#define IQS269_MISC_B_FILT_STR_SLIDER		GENMASK(1, 0)

#define IQS269_TOUCH_HOLD_SLIDER_SEL		0x89
#define IQS269_TOUCH_HOLD_DEFAULT		0x14
#define IQS269_TOUCH_HOLD_MS_MIN		256
#define IQS269_TOUCH_HOLD_MS_MAX		65280

#define IQS269_TIMEOUT_TAP_MS_MAX		4080
#define IQS269_TIMEOUT_SWIPE_MS_MAX		4080
#define IQS269_THRESH_SWIPE_MAX			255

#define IQS269_CHx_ENG_A_MEAS_CAP_SIZE		BIT(15)
#define IQS269_CHx_ENG_A_RX_GND_INACTIVE	BIT(13)
#define IQS269_CHx_ENG_A_LOCAL_CAP_SIZE		BIT(12)
#define IQS269_CHx_ENG_A_ATI_MODE_MASK		GENMASK(9, 8)
#define IQS269_CHx_ENG_A_ATI_MODE_SHIFT		8
#define IQS269_CHx_ENG_A_ATI_MODE_MAX		3
#define IQS269_CHx_ENG_A_INV_LOGIC		BIT(7)
#define IQS269_CHx_ENG_A_PROJ_BIAS_MASK		GENMASK(6, 5)
#define IQS269_CHx_ENG_A_PROJ_BIAS_SHIFT	5
#define IQS269_CHx_ENG_A_PROJ_BIAS_MAX		3
#define IQS269_CHx_ENG_A_SENSE_MODE_MASK	GENMASK(3, 0)
#define IQS269_CHx_ENG_A_SENSE_MODE_MAX		15

#define IQS269_CHx_ENG_B_LOCAL_CAP_ENABLE	BIT(13)
#define IQS269_CHx_ENG_B_SENSE_FREQ_MASK	GENMASK(10, 9)
#define IQS269_CHx_ENG_B_SENSE_FREQ_SHIFT	9
#define IQS269_CHx_ENG_B_SENSE_FREQ_MAX		3
#define IQS269_CHx_ENG_B_STATIC_ENABLE		BIT(8)
#define IQS269_CHx_ENG_B_ATI_BASE_MASK		GENMASK(7, 6)
#define IQS269_CHx_ENG_B_ATI_BASE_75		0x00
#define IQS269_CHx_ENG_B_ATI_BASE_100		0x40
#define IQS269_CHx_ENG_B_ATI_BASE_150		0x80
#define IQS269_CHx_ENG_B_ATI_BASE_200		0xC0
#define IQS269_CHx_ENG_B_ATI_TARGET_MASK	GENMASK(5, 0)
#define IQS269_CHx_ENG_B_ATI_TARGET_MAX		2016

#define IQS269_CHx_WEIGHT_MAX			255
#define IQS269_CHx_THRESH_MAX			255
#define IQS269_CHx_HYST_DEEP_MASK		GENMASK(7, 4)
#define IQS269_CHx_HYST_DEEP_SHIFT		4
#define IQS269_CHx_HYST_TOUCH_MASK		GENMASK(3, 0)
#define IQS269_CHx_HYST_MAX			15

#define IQS269_CHx_HALL_INACTIVE		6
#define IQS269_CHx_HALL_ACTIVE			7

#define IQS269_HALL_PAD_R			BIT(0)
#define IQS269_HALL_PAD_L			BIT(1)
#define IQS269_HALL_PAD_INV			BIT(6)

#define IQS269_HALL_UI				0xF5
#define IQS269_HALL_UI_ENABLE			BIT(15)

#define IQS269_MAX_REG				0xFF

#define IQS269_OTP_OPTION_DEFAULT		0x00
#define IQS269_OTP_OPTION_TWS			0xD0
#define IQS269_OTP_OPTION_HOLD			BIT(7)

#define IQS269_NUM_CH				8
#define IQS269_NUM_SL				2

#define iqs269_irq_wait()			usleep_range(200, 250)

enum iqs269_local_cap_size {
	IQS269_LOCAL_CAP_SIZE_0,
	IQS269_LOCAL_CAP_SIZE_GLOBAL_ONLY,
	IQS269_LOCAL_CAP_SIZE_GLOBAL_0pF5,
};

enum iqs269_st_offs {
	IQS269_ST_OFFS_PROX,
	IQS269_ST_OFFS_DIR,
	IQS269_ST_OFFS_TOUCH,
	IQS269_ST_OFFS_DEEP,
};

enum iqs269_th_offs {
	IQS269_TH_OFFS_PROX,
	IQS269_TH_OFFS_TOUCH,
	IQS269_TH_OFFS_DEEP,
};

enum iqs269_event_id {
	IQS269_EVENT_PROX_DN,
	IQS269_EVENT_PROX_UP,
	IQS269_EVENT_TOUCH_DN,
	IQS269_EVENT_TOUCH_UP,
	IQS269_EVENT_DEEP_DN,
	IQS269_EVENT_DEEP_UP,
};

enum iqs269_slider_id {
	IQS269_SLIDER_NONE,
	IQS269_SLIDER_KEY,
	IQS269_SLIDER_RAW,
};

enum iqs269_gesture_id {
	IQS269_GESTURE_TAP,
	IQS269_GESTURE_HOLD,
	IQS269_GESTURE_FLICK_POS,
	IQS269_GESTURE_FLICK_NEG,
	IQS269_NUM_GESTURES,
};

struct iqs269_switch_desc {
	unsigned int code;
	bool enabled;
};

struct iqs269_event_desc {
	const char *name;
	enum iqs269_st_offs st_offs;
	enum iqs269_th_offs th_offs;
	bool dir_up;
	u8 mask;
};

static const struct iqs269_event_desc iqs269_events[] = {
	[IQS269_EVENT_PROX_DN] = {
		.name = "event-prox",
		.st_offs = IQS269_ST_OFFS_PROX,
		.th_offs = IQS269_TH_OFFS_PROX,
		.mask = IQS269_EVENT_MASK_PROX,
	},
	[IQS269_EVENT_PROX_UP] = {
		.name = "event-prox-alt",
		.st_offs = IQS269_ST_OFFS_PROX,
		.th_offs = IQS269_TH_OFFS_PROX,
		.dir_up = true,
		.mask = IQS269_EVENT_MASK_PROX,
	},
	[IQS269_EVENT_TOUCH_DN] = {
		.name = "event-touch",
		.st_offs = IQS269_ST_OFFS_TOUCH,
		.th_offs = IQS269_TH_OFFS_TOUCH,
		.mask = IQS269_EVENT_MASK_TOUCH,
	},
	[IQS269_EVENT_TOUCH_UP] = {
		.name = "event-touch-alt",
		.st_offs = IQS269_ST_OFFS_TOUCH,
		.th_offs = IQS269_TH_OFFS_TOUCH,
		.dir_up = true,
		.mask = IQS269_EVENT_MASK_TOUCH,
	},
	[IQS269_EVENT_DEEP_DN] = {
		.name = "event-deep",
		.st_offs = IQS269_ST_OFFS_DEEP,
		.th_offs = IQS269_TH_OFFS_DEEP,
		.mask = IQS269_EVENT_MASK_DEEP,
	},
	[IQS269_EVENT_DEEP_UP] = {
		.name = "event-deep-alt",
		.st_offs = IQS269_ST_OFFS_DEEP,
		.th_offs = IQS269_TH_OFFS_DEEP,
		.dir_up = true,
		.mask = IQS269_EVENT_MASK_DEEP,
	},
};

struct iqs269_ver_info {
	u8 prod_num;
	u8 sw_num;
	u8 hw_num;
	u8 fw_num;
} __packed;

struct iqs269_ch_reg {
	u8 rx_enable;
	u8 tx_enable;
	__be16 engine_a;
	__be16 engine_b;
	__be16 ati_comp;
	u8 thresh[3];
	u8 hyst;
	u8 assoc_select;
	u8 assoc_weight;
} __packed;

struct iqs269_sys_reg {
	__be16 general;
	u8 active;
	u8 filter;
	u8 reseed;
	u8 event_mask;
	u8 rate_np;
	u8 rate_lp;
	u8 rate_ulp;
	u8 timeout_pwr;
	u8 timeout_rdy;
	u8 timeout_lta;
	__be16 misc_a;
	__be16 misc_b;
	u8 blocking;
	u8 padding;
	u8 slider_select[IQS269_NUM_SL];
	u8 timeout_tap;
	u8 timeout_swipe;
	u8 thresh_swipe;
	u8 redo_ati;
	struct iqs269_ch_reg ch_reg[IQS269_NUM_CH];
} __packed;

struct iqs269_flags {
	__be16 system;
	u8 gesture;
	u8 padding;
	u8 states[4];
} __packed;

struct iqs269_private {
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex lock;
	struct iqs269_switch_desc switches[ARRAY_SIZE(iqs269_events)];
	struct iqs269_ver_info ver_info;
	struct iqs269_sys_reg sys_reg;
	struct completion ati_done;
	struct input_dev *keypad;
	struct input_dev *slider[IQS269_NUM_SL];
	unsigned int keycode[ARRAY_SIZE(iqs269_events) * IQS269_NUM_CH];
	unsigned int sl_code[IQS269_NUM_SL][IQS269_NUM_GESTURES];
	unsigned int otp_option;
	unsigned int ch_num;
	bool hall_enable;
	bool ati_current;
};

static enum iqs269_slider_id iqs269_slider_type(struct iqs269_private *iqs269,
						int slider_num)
{
	int i;

	/*
	 * Slider 1 is unavailable if the touch-and-hold option is enabled via
	 * OTP. In that case, the channel selection register is repurposed for
	 * the touch-and-hold timer ceiling.
	 */
	if (slider_num && (iqs269->otp_option & IQS269_OTP_OPTION_HOLD))
		return IQS269_SLIDER_NONE;

	if (!iqs269->sys_reg.slider_select[slider_num])
		return IQS269_SLIDER_NONE;

	for (i = 0; i < IQS269_NUM_GESTURES; i++)
		if (iqs269->sl_code[slider_num][i] != KEY_RESERVED)
			return IQS269_SLIDER_KEY;

	return IQS269_SLIDER_RAW;
}

static int iqs269_ati_mode_set(struct iqs269_private *iqs269,
			       unsigned int ch_num, unsigned int mode)
{
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;
	u16 engine_a;

	if (ch_num >= IQS269_NUM_CH)
		return -EINVAL;

	if (mode > IQS269_CHx_ENG_A_ATI_MODE_MAX)
		return -EINVAL;

	guard(mutex)(&iqs269->lock);

	engine_a = be16_to_cpu(ch_reg[ch_num].engine_a);

	engine_a &= ~IQS269_CHx_ENG_A_ATI_MODE_MASK;
	engine_a |= (mode << IQS269_CHx_ENG_A_ATI_MODE_SHIFT);

	ch_reg[ch_num].engine_a = cpu_to_be16(engine_a);
	iqs269->ati_current = false;

	return 0;
}

static int iqs269_ati_mode_get(struct iqs269_private *iqs269,
			       unsigned int ch_num, unsigned int *mode)
{
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;
	u16 engine_a;

	if (ch_num >= IQS269_NUM_CH)
		return -EINVAL;

	guard(mutex)(&iqs269->lock);

	engine_a = be16_to_cpu(ch_reg[ch_num].engine_a);

	engine_a &= IQS269_CHx_ENG_A_ATI_MODE_MASK;
	*mode = (engine_a >> IQS269_CHx_ENG_A_ATI_MODE_SHIFT);

	return 0;
}

static int iqs269_ati_base_set(struct iqs269_private *iqs269,
			       unsigned int ch_num, unsigned int base)
{
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;
	u16 engine_b;

	if (ch_num >= IQS269_NUM_CH)
		return -EINVAL;

	switch (base) {
	case 75:
		base = IQS269_CHx_ENG_B_ATI_BASE_75;
		break;

	case 100:
		base = IQS269_CHx_ENG_B_ATI_BASE_100;
		break;

	case 150:
		base = IQS269_CHx_ENG_B_ATI_BASE_150;
		break;

	case 200:
		base = IQS269_CHx_ENG_B_ATI_BASE_200;
		break;

	default:
		return -EINVAL;
	}

	guard(mutex)(&iqs269->lock);

	engine_b = be16_to_cpu(ch_reg[ch_num].engine_b);

	engine_b &= ~IQS269_CHx_ENG_B_ATI_BASE_MASK;
	engine_b |= base;

	ch_reg[ch_num].engine_b = cpu_to_be16(engine_b);
	iqs269->ati_current = false;

	return 0;
}

static int iqs269_ati_base_get(struct iqs269_private *iqs269,
			       unsigned int ch_num, unsigned int *base)
{
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;
	u16 engine_b;

	if (ch_num >= IQS269_NUM_CH)
		return -EINVAL;

	guard(mutex)(&iqs269->lock);

	engine_b = be16_to_cpu(ch_reg[ch_num].engine_b);

	switch (engine_b & IQS269_CHx_ENG_B_ATI_BASE_MASK) {
	case IQS269_CHx_ENG_B_ATI_BASE_75:
		*base = 75;
		return 0;

	case IQS269_CHx_ENG_B_ATI_BASE_100:
		*base = 100;
		return 0;

	case IQS269_CHx_ENG_B_ATI_BASE_150:
		*base = 150;
		return 0;

	case IQS269_CHx_ENG_B_ATI_BASE_200:
		*base = 200;
		return 0;

	default:
		return -EINVAL;
	}
}

static int iqs269_ati_target_set(struct iqs269_private *iqs269,
				 unsigned int ch_num, unsigned int target)
{
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;
	u16 engine_b;

	if (ch_num >= IQS269_NUM_CH)
		return -EINVAL;

	if (target > IQS269_CHx_ENG_B_ATI_TARGET_MAX)
		return -EINVAL;

	guard(mutex)(&iqs269->lock);

	engine_b = be16_to_cpu(ch_reg[ch_num].engine_b);

	engine_b &= ~IQS269_CHx_ENG_B_ATI_TARGET_MASK;
	engine_b |= target / 32;

	ch_reg[ch_num].engine_b = cpu_to_be16(engine_b);
	iqs269->ati_current = false;

	return 0;
}

static int iqs269_ati_target_get(struct iqs269_private *iqs269,
				 unsigned int ch_num, unsigned int *target)
{
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;
	u16 engine_b;

	if (ch_num >= IQS269_NUM_CH)
		return -EINVAL;

	guard(mutex)(&iqs269->lock);

	engine_b = be16_to_cpu(ch_reg[ch_num].engine_b);
	*target = (engine_b & IQS269_CHx_ENG_B_ATI_TARGET_MASK) * 32;

	return 0;
}

static int iqs269_parse_mask(const struct fwnode_handle *fwnode,
			     const char *propname, u8 *mask)
{
	unsigned int val[IQS269_NUM_CH];
	int count, error, i;

	count = fwnode_property_count_u32(fwnode, propname);
	if (count < 0)
		return 0;

	if (count > IQS269_NUM_CH)
		return -EINVAL;

	error = fwnode_property_read_u32_array(fwnode, propname, val, count);
	if (error)
		return error;

	*mask = 0;

	for (i = 0; i < count; i++) {
		if (val[i] >= IQS269_NUM_CH)
			return -EINVAL;

		*mask |= BIT(val[i]);
	}

	return 0;
}

static int iqs269_parse_chan(struct iqs269_private *iqs269,
			     const struct fwnode_handle *ch_node)
{
	struct i2c_client *client = iqs269->client;
	struct iqs269_ch_reg *ch_reg;
	u16 engine_a, engine_b;
	unsigned int reg, val;
	int error, i;

	error = fwnode_property_read_u32(ch_node, "reg", &reg);
	if (error) {
		dev_err(&client->dev, "Failed to read channel number: %d\n",
			error);
		return error;
	} else if (reg >= IQS269_NUM_CH) {
		dev_err(&client->dev, "Invalid channel number: %u\n", reg);
		return -EINVAL;
	}

	iqs269->sys_reg.active |= BIT(reg);
	if (!fwnode_property_present(ch_node, "azoteq,reseed-disable"))
		iqs269->sys_reg.reseed |= BIT(reg);

	if (fwnode_property_present(ch_node, "azoteq,blocking-enable"))
		iqs269->sys_reg.blocking |= BIT(reg);

	if (fwnode_property_present(ch_node, "azoteq,slider0-select"))
		iqs269->sys_reg.slider_select[0] |= BIT(reg);

	if (fwnode_property_present(ch_node, "azoteq,slider1-select") &&
	    !(iqs269->otp_option & IQS269_OTP_OPTION_HOLD))
		iqs269->sys_reg.slider_select[1] |= BIT(reg);

	ch_reg = &iqs269->sys_reg.ch_reg[reg];

	error = iqs269_parse_mask(ch_node, "azoteq,rx-enable",
				  &ch_reg->rx_enable);
	if (error) {
		dev_err(&client->dev, "Invalid channel %u RX enable mask: %d\n",
			reg, error);
		return error;
	}

	error = iqs269_parse_mask(ch_node, "azoteq,tx-enable",
				  &ch_reg->tx_enable);
	if (error) {
		dev_err(&client->dev, "Invalid channel %u TX enable mask: %d\n",
			reg, error);
		return error;
	}

	engine_a = be16_to_cpu(ch_reg->engine_a);
	engine_b = be16_to_cpu(ch_reg->engine_b);

	engine_a |= IQS269_CHx_ENG_A_MEAS_CAP_SIZE;
	if (fwnode_property_present(ch_node, "azoteq,meas-cap-decrease"))
		engine_a &= ~IQS269_CHx_ENG_A_MEAS_CAP_SIZE;

	engine_a |= IQS269_CHx_ENG_A_RX_GND_INACTIVE;
	if (fwnode_property_present(ch_node, "azoteq,rx-float-inactive"))
		engine_a &= ~IQS269_CHx_ENG_A_RX_GND_INACTIVE;

	engine_a &= ~IQS269_CHx_ENG_A_LOCAL_CAP_SIZE;
	engine_b &= ~IQS269_CHx_ENG_B_LOCAL_CAP_ENABLE;
	if (!fwnode_property_read_u32(ch_node, "azoteq,local-cap-size", &val)) {
		switch (val) {
		case IQS269_LOCAL_CAP_SIZE_0:
			break;

		case IQS269_LOCAL_CAP_SIZE_GLOBAL_0pF5:
			engine_a |= IQS269_CHx_ENG_A_LOCAL_CAP_SIZE;
			fallthrough;

		case IQS269_LOCAL_CAP_SIZE_GLOBAL_ONLY:
			engine_b |= IQS269_CHx_ENG_B_LOCAL_CAP_ENABLE;
			break;

		default:
			dev_err(&client->dev,
				"Invalid channel %u local cap. size: %u\n", reg,
				val);
			return -EINVAL;
		}
	}

	engine_a &= ~IQS269_CHx_ENG_A_INV_LOGIC;
	if (fwnode_property_present(ch_node, "azoteq,invert-enable"))
		engine_a |= IQS269_CHx_ENG_A_INV_LOGIC;

	if (!fwnode_property_read_u32(ch_node, "azoteq,proj-bias", &val)) {
		if (val > IQS269_CHx_ENG_A_PROJ_BIAS_MAX) {
			dev_err(&client->dev,
				"Invalid channel %u bias current: %u\n", reg,
				val);
			return -EINVAL;
		}

		engine_a &= ~IQS269_CHx_ENG_A_PROJ_BIAS_MASK;
		engine_a |= (val << IQS269_CHx_ENG_A_PROJ_BIAS_SHIFT);
	}

	if (!fwnode_property_read_u32(ch_node, "azoteq,sense-mode", &val)) {
		if (val > IQS269_CHx_ENG_A_SENSE_MODE_MAX) {
			dev_err(&client->dev,
				"Invalid channel %u sensing mode: %u\n", reg,
				val);
			return -EINVAL;
		}

		engine_a &= ~IQS269_CHx_ENG_A_SENSE_MODE_MASK;
		engine_a |= val;
	}

	if (!fwnode_property_read_u32(ch_node, "azoteq,sense-freq", &val)) {
		if (val > IQS269_CHx_ENG_B_SENSE_FREQ_MAX) {
			dev_err(&client->dev,
				"Invalid channel %u sensing frequency: %u\n",
				reg, val);
			return -EINVAL;
		}

		engine_b &= ~IQS269_CHx_ENG_B_SENSE_FREQ_MASK;
		engine_b |= (val << IQS269_CHx_ENG_B_SENSE_FREQ_SHIFT);
	}

	engine_b &= ~IQS269_CHx_ENG_B_STATIC_ENABLE;
	if (fwnode_property_present(ch_node, "azoteq,static-enable"))
		engine_b |= IQS269_CHx_ENG_B_STATIC_ENABLE;

	ch_reg->engine_a = cpu_to_be16(engine_a);
	ch_reg->engine_b = cpu_to_be16(engine_b);

	if (!fwnode_property_read_u32(ch_node, "azoteq,ati-mode", &val)) {
		error = iqs269_ati_mode_set(iqs269, reg, val);
		if (error) {
			dev_err(&client->dev,
				"Invalid channel %u ATI mode: %u\n", reg, val);
			return error;
		}
	}

	if (!fwnode_property_read_u32(ch_node, "azoteq,ati-base", &val)) {
		error = iqs269_ati_base_set(iqs269, reg, val);
		if (error) {
			dev_err(&client->dev,
				"Invalid channel %u ATI base: %u\n", reg, val);
			return error;
		}
	}

	if (!fwnode_property_read_u32(ch_node, "azoteq,ati-target", &val)) {
		error = iqs269_ati_target_set(iqs269, reg, val);
		if (error) {
			dev_err(&client->dev,
				"Invalid channel %u ATI target: %u\n", reg,
				val);
			return error;
		}
	}

	error = iqs269_parse_mask(ch_node, "azoteq,assoc-select",
				  &ch_reg->assoc_select);
	if (error) {
		dev_err(&client->dev, "Invalid channel %u association: %d\n",
			reg, error);
		return error;
	}

	if (!fwnode_property_read_u32(ch_node, "azoteq,assoc-weight", &val)) {
		if (val > IQS269_CHx_WEIGHT_MAX) {
			dev_err(&client->dev,
				"Invalid channel %u associated weight: %u\n",
				reg, val);
			return -EINVAL;
		}

		ch_reg->assoc_weight = val;
	}

	for (i = 0; i < ARRAY_SIZE(iqs269_events); i++) {
		struct fwnode_handle *ev_node __free(fwnode_handle) =
			fwnode_get_named_child_node(ch_node,
						    iqs269_events[i].name);
		if (!ev_node)
			continue;

		if (!fwnode_property_read_u32(ev_node, "azoteq,thresh", &val)) {
			if (val > IQS269_CHx_THRESH_MAX) {
				dev_err(&client->dev,
					"Invalid channel %u threshold: %u\n",
					reg, val);
				return -EINVAL;
			}

			ch_reg->thresh[iqs269_events[i].th_offs] = val;
		}

		if (!fwnode_property_read_u32(ev_node, "azoteq,hyst", &val)) {
			u8 *hyst = &ch_reg->hyst;

			if (val > IQS269_CHx_HYST_MAX) {
				dev_err(&client->dev,
					"Invalid channel %u hysteresis: %u\n",
					reg, val);
				return -EINVAL;
			}

			if (i == IQS269_EVENT_DEEP_DN ||
			    i == IQS269_EVENT_DEEP_UP) {
				*hyst &= ~IQS269_CHx_HYST_DEEP_MASK;
				*hyst |= (val << IQS269_CHx_HYST_DEEP_SHIFT);
			} else if (i == IQS269_EVENT_TOUCH_DN ||
				   i == IQS269_EVENT_TOUCH_UP) {
				*hyst &= ~IQS269_CHx_HYST_TOUCH_MASK;
				*hyst |= val;
			}
		}

		error = fwnode_property_read_u32(ev_node, "linux,code", &val);
		if (error == -EINVAL) {
			continue;
		} else if (error) {
			dev_err(&client->dev,
				"Failed to read channel %u code: %d\n", reg,
				error);
			return error;
		}

		switch (reg) {
		case IQS269_CHx_HALL_ACTIVE:
			if (iqs269->hall_enable) {
				iqs269->switches[i].code = val;
				iqs269->switches[i].enabled = true;
			}
			fallthrough;

		case IQS269_CHx_HALL_INACTIVE:
			if (iqs269->hall_enable)
				break;
			fallthrough;

		default:
			iqs269->keycode[i * IQS269_NUM_CH + reg] = val;
		}

		iqs269->sys_reg.event_mask &= ~iqs269_events[i].mask;
	}

	return 0;
}

static int iqs269_parse_prop(struct iqs269_private *iqs269)
{
	struct iqs269_sys_reg *sys_reg = &iqs269->sys_reg;
	struct i2c_client *client = iqs269->client;
	u16 general, misc_a, misc_b;
	unsigned int val;
	int error;

	iqs269->hall_enable = device_property_present(&client->dev,
						      "azoteq,hall-enable");

	error = regmap_raw_read(iqs269->regmap, IQS269_SYS_SETTINGS, sys_reg,
				sizeof(*sys_reg));
	if (error)
		return error;

	if (!device_property_read_u32(&client->dev, "azoteq,filt-str-lp-lta",
				      &val)) {
		if (val > IQS269_FILT_STR_MAX) {
			dev_err(&client->dev, "Invalid filter strength: %u\n",
				val);
			return -EINVAL;
		}

		sys_reg->filter &= ~IQS269_FILT_STR_LP_LTA_MASK;
		sys_reg->filter |= (val << IQS269_FILT_STR_LP_LTA_SHIFT);
	}

	if (!device_property_read_u32(&client->dev, "azoteq,filt-str-lp-cnt",
				      &val)) {
		if (val > IQS269_FILT_STR_MAX) {
			dev_err(&client->dev, "Invalid filter strength: %u\n",
				val);
			return -EINVAL;
		}

		sys_reg->filter &= ~IQS269_FILT_STR_LP_CNT_MASK;
		sys_reg->filter |= (val << IQS269_FILT_STR_LP_CNT_SHIFT);
	}

	if (!device_property_read_u32(&client->dev, "azoteq,filt-str-np-lta",
				      &val)) {
		if (val > IQS269_FILT_STR_MAX) {
			dev_err(&client->dev, "Invalid filter strength: %u\n",
				val);
			return -EINVAL;
		}

		sys_reg->filter &= ~IQS269_FILT_STR_NP_LTA_MASK;
		sys_reg->filter |= (val << IQS269_FILT_STR_NP_LTA_SHIFT);
	}

	if (!device_property_read_u32(&client->dev, "azoteq,filt-str-np-cnt",
				      &val)) {
		if (val > IQS269_FILT_STR_MAX) {
			dev_err(&client->dev, "Invalid filter strength: %u\n",
				val);
			return -EINVAL;
		}

		sys_reg->filter &= ~IQS269_FILT_STR_NP_CNT_MASK;
		sys_reg->filter |= val;
	}

	if (!device_property_read_u32(&client->dev, "azoteq,rate-np-ms",
				      &val)) {
		if (val > IQS269_RATE_NP_MS_MAX) {
			dev_err(&client->dev, "Invalid report rate: %u\n", val);
			return -EINVAL;
		}

		sys_reg->rate_np = val;
	}

	if (!device_property_read_u32(&client->dev, "azoteq,rate-lp-ms",
				      &val)) {
		if (val > IQS269_RATE_LP_MS_MAX) {
			dev_err(&client->dev, "Invalid report rate: %u\n", val);
			return -EINVAL;
		}

		sys_reg->rate_lp = val;
	}

	if (!device_property_read_u32(&client->dev, "azoteq,rate-ulp-ms",
				      &val)) {
		if (val > IQS269_RATE_ULP_MS_MAX) {
			dev_err(&client->dev, "Invalid report rate: %u\n", val);
			return -EINVAL;
		}

		sys_reg->rate_ulp = val / 16;
	}

	if (!device_property_read_u32(&client->dev, "azoteq,timeout-pwr-ms",
				      &val)) {
		if (val > IQS269_TIMEOUT_PWR_MS_MAX) {
			dev_err(&client->dev, "Invalid timeout: %u\n", val);
			return -EINVAL;
		}

		sys_reg->timeout_pwr = val / 512;
	}

	if (!device_property_read_u32(&client->dev, "azoteq,timeout-lta-ms",
				      &val)) {
		if (val > IQS269_TIMEOUT_LTA_MS_MAX) {
			dev_err(&client->dev, "Invalid timeout: %u\n", val);
			return -EINVAL;
		}

		sys_reg->timeout_lta = val / 512;
	}

	misc_a = be16_to_cpu(sys_reg->misc_a);
	misc_b = be16_to_cpu(sys_reg->misc_b);

	misc_a &= ~IQS269_MISC_A_ATI_BAND_DISABLE;
	if (device_property_present(&client->dev, "azoteq,ati-band-disable"))
		misc_a |= IQS269_MISC_A_ATI_BAND_DISABLE;

	misc_a &= ~IQS269_MISC_A_ATI_LP_ONLY;
	if (device_property_present(&client->dev, "azoteq,ati-lp-only"))
		misc_a |= IQS269_MISC_A_ATI_LP_ONLY;

	misc_a &= ~IQS269_MISC_A_ATI_BAND_TIGHTEN;
	if (device_property_present(&client->dev, "azoteq,ati-band-tighten"))
		misc_a |= IQS269_MISC_A_ATI_BAND_TIGHTEN;

	misc_a &= ~IQS269_MISC_A_FILT_DISABLE;
	if (device_property_present(&client->dev, "azoteq,filt-disable"))
		misc_a |= IQS269_MISC_A_FILT_DISABLE;

	if (!device_property_read_u32(&client->dev, "azoteq,gpio3-select",
				      &val)) {
		if (val >= IQS269_NUM_CH) {
			dev_err(&client->dev, "Invalid GPIO3 selection: %u\n",
				val);
			return -EINVAL;
		}

		misc_a &= ~IQS269_MISC_A_GPIO3_SELECT_MASK;
		misc_a |= (val << IQS269_MISC_A_GPIO3_SELECT_SHIFT);
	}

	misc_a &= ~IQS269_MISC_A_DUAL_DIR;
	if (device_property_present(&client->dev, "azoteq,dual-direction"))
		misc_a |= IQS269_MISC_A_DUAL_DIR;

	if (!device_property_read_u32(&client->dev, "azoteq,tx-freq", &val)) {
		if (val > IQS269_MISC_A_TX_FREQ_MAX) {
			dev_err(&client->dev,
				"Invalid excitation frequency: %u\n", val);
			return -EINVAL;
		}

		misc_a &= ~IQS269_MISC_A_TX_FREQ_MASK;
		misc_a |= (val << IQS269_MISC_A_TX_FREQ_SHIFT);
	}

	misc_a &= ~IQS269_MISC_A_GLOBAL_CAP_SIZE;
	if (device_property_present(&client->dev, "azoteq,global-cap-increase"))
		misc_a |= IQS269_MISC_A_GLOBAL_CAP_SIZE;

	if (!device_property_read_u32(&client->dev, "azoteq,reseed-select",
				      &val)) {
		if (val > IQS269_MISC_B_RESEED_UI_SEL_MAX) {
			dev_err(&client->dev, "Invalid reseed selection: %u\n",
				val);
			return -EINVAL;
		}

		misc_b &= ~IQS269_MISC_B_RESEED_UI_SEL_MASK;
		misc_b |= (val << IQS269_MISC_B_RESEED_UI_SEL_SHIFT);
	}

	misc_b &= ~IQS269_MISC_B_TRACKING_UI_ENABLE;
	if (device_property_present(&client->dev, "azoteq,tracking-enable"))
		misc_b |= IQS269_MISC_B_TRACKING_UI_ENABLE;

	if (!device_property_read_u32(&client->dev, "azoteq,filt-str-slider",
				      &val)) {
		if (val > IQS269_FILT_STR_MAX) {
			dev_err(&client->dev, "Invalid filter strength: %u\n",
				val);
			return -EINVAL;
		}

		misc_b &= ~IQS269_MISC_B_FILT_STR_SLIDER;
		misc_b |= val;
	}

	sys_reg->misc_a = cpu_to_be16(misc_a);
	sys_reg->misc_b = cpu_to_be16(misc_b);

	sys_reg->active = 0;
	sys_reg->reseed = 0;

	sys_reg->blocking = 0;

	sys_reg->slider_select[0] = 0;

	/*
	 * If configured via OTP to do so, the device asserts a pulse on the
	 * GPIO4 pin for approximately 60 ms once a selected channel is held
	 * in a state of touch for a configurable length of time.
	 *
	 * In that case, the register used for slider 1 channel selection is
	 * repurposed for the touch-and-hold timer ceiling.
	 */
	if (iqs269->otp_option & IQS269_OTP_OPTION_HOLD) {
		if (!device_property_read_u32(&client->dev,
					      "azoteq,touch-hold-ms", &val)) {
			if (val < IQS269_TOUCH_HOLD_MS_MIN ||
			    val > IQS269_TOUCH_HOLD_MS_MAX) {
				dev_err(&client->dev,
					"Invalid touch-and-hold ceiling: %u\n",
					val);
				return -EINVAL;
			}

			sys_reg->slider_select[1] = val / 256;
		} else if (iqs269->ver_info.fw_num < IQS269_VER_INFO_FW_NUM_3) {
			/*
			 * The default touch-and-hold timer ceiling initially
			 * read from early revisions of silicon is invalid if
			 * the device experienced a soft reset between power-
			 * on and the read operation.
			 *
			 * To protect against this case, explicitly cache the
			 * default value so that it is restored each time the
			 * device is re-initialized.
			 */
			sys_reg->slider_select[1] = IQS269_TOUCH_HOLD_DEFAULT;
		}
	} else {
		sys_reg->slider_select[1] = 0;
	}

	sys_reg->event_mask = ~((u8)IQS269_EVENT_MASK_SYS);

	device_for_each_child_node_scoped(&client->dev, ch_node) {
		error = iqs269_parse_chan(iqs269, ch_node);
		if (error)
			return error;
	}

	/*
	 * Volunteer all active channels to participate in ATI when REDO-ATI is
	 * manually triggered.
	 */
	sys_reg->redo_ati = sys_reg->active;

	general = be16_to_cpu(sys_reg->general);

	if (device_property_present(&client->dev, "azoteq,clk-div"))
		general |= IQS269_SYS_SETTINGS_CLK_DIV;

	/*
	 * Configure the device to automatically switch between normal and low-
	 * power modes as a function of sensing activity. Ultra-low-power mode,
	 * if enabled, is reserved for suspend.
	 */
	general &= ~IQS269_SYS_SETTINGS_ULP_AUTO;
	general &= ~IQS269_SYS_SETTINGS_DIS_AUTO;
	general &= ~IQS269_SYS_SETTINGS_PWR_MODE_MASK;

	if (!device_property_read_u32(&client->dev, "azoteq,suspend-mode",
				      &val)) {
		if (val > IQS269_SYS_SETTINGS_PWR_MODE_MAX) {
			dev_err(&client->dev, "Invalid suspend mode: %u\n",
				val);
			return -EINVAL;
		}

		general |= (val << IQS269_SYS_SETTINGS_PWR_MODE_SHIFT);
	}

	if (!device_property_read_u32(&client->dev, "azoteq,ulp-update",
				      &val)) {
		if (val > IQS269_SYS_SETTINGS_ULP_UPDATE_MAX) {
			dev_err(&client->dev, "Invalid update rate: %u\n", val);
			return -EINVAL;
		}

		general &= ~IQS269_SYS_SETTINGS_ULP_UPDATE_MASK;
		general |= (val << IQS269_SYS_SETTINGS_ULP_UPDATE_SHIFT);
	}

	if (device_property_present(&client->dev, "linux,keycodes")) {
		int scale = 1;
		int count = device_property_count_u32(&client->dev,
						      "linux,keycodes");
		if (count > IQS269_NUM_GESTURES * IQS269_NUM_SL) {
			dev_err(&client->dev, "Too many keycodes present\n");
			return -EINVAL;
		} else if (count < 0) {
			dev_err(&client->dev, "Failed to count keycodes: %d\n",
				count);
			return count;
		}

		error = device_property_read_u32_array(&client->dev,
						       "linux,keycodes",
						       *iqs269->sl_code, count);
		if (error) {
			dev_err(&client->dev, "Failed to read keycodes: %d\n",
				error);
			return error;
		}

		if (device_property_present(&client->dev,
					    "azoteq,gesture-swipe"))
			general |= IQS269_SYS_SETTINGS_SLIDER_SWIPE;

		/*
		 * Early revisions of silicon use a more granular step size for
		 * tap and swipe gesture timeouts; scale them appropriately.
		 */
		if (iqs269->ver_info.fw_num < IQS269_VER_INFO_FW_NUM_3)
			scale = 4;

		if (!device_property_read_u32(&client->dev,
					      "azoteq,timeout-tap-ms", &val)) {
			if (val > IQS269_TIMEOUT_TAP_MS_MAX / scale) {
				dev_err(&client->dev, "Invalid timeout: %u\n",
					val);
				return -EINVAL;
			}

			sys_reg->timeout_tap = val / (16 / scale);
		}

		if (!device_property_read_u32(&client->dev,
					      "azoteq,timeout-swipe-ms",
					      &val)) {
			if (val > IQS269_TIMEOUT_SWIPE_MS_MAX / scale) {
				dev_err(&client->dev, "Invalid timeout: %u\n",
					val);
				return -EINVAL;
			}

			sys_reg->timeout_swipe = val / (16 / scale);
		}

		if (!device_property_read_u32(&client->dev,
					      "azoteq,thresh-swipe", &val)) {
			if (val > IQS269_THRESH_SWIPE_MAX) {
				dev_err(&client->dev, "Invalid threshold: %u\n",
					val);
				return -EINVAL;
			}

			sys_reg->thresh_swipe = val;
		}

		sys_reg->event_mask &= ~IQS269_EVENT_MASK_GESTURE;
	}

	general &= ~IQS269_SYS_SETTINGS_RESEED_OFFSET;
	if (device_property_present(&client->dev, "azoteq,reseed-offset"))
		general |= IQS269_SYS_SETTINGS_RESEED_OFFSET;

	general |= IQS269_SYS_SETTINGS_EVENT_MODE;

	/*
	 * As per the datasheet, enable streaming during normal-power mode if
	 * raw coordinates will be read from either slider. In that case, the
	 * device returns to event mode during low-power mode.
	 */
	if (iqs269_slider_type(iqs269, 0) == IQS269_SLIDER_RAW ||
	    iqs269_slider_type(iqs269, 1) == IQS269_SLIDER_RAW)
		general |= IQS269_SYS_SETTINGS_EVENT_MODE_LP;

	general |= IQS269_SYS_SETTINGS_REDO_ATI;
	general |= IQS269_SYS_SETTINGS_ACK_RESET;

	sys_reg->general = cpu_to_be16(general);

	return 0;
}

static const struct reg_sequence iqs269_tws_init[] = {
	{ IQS269_TOUCH_HOLD_SLIDER_SEL, IQS269_TOUCH_HOLD_DEFAULT },
	{ 0xF0, 0x580F },
	{ 0xF0, 0x59EF },
};

static int iqs269_dev_init(struct iqs269_private *iqs269)
{
	int error;

	guard(mutex)(&iqs269->lock);

	/*
	 * Early revisions of silicon require the following workaround in order
	 * to restore any OTP-enabled functionality after a soft reset.
	 */
	if (iqs269->otp_option == IQS269_OTP_OPTION_TWS &&
	    iqs269->ver_info.fw_num < IQS269_VER_INFO_FW_NUM_3) {
		error = regmap_multi_reg_write(iqs269->regmap, iqs269_tws_init,
					       ARRAY_SIZE(iqs269_tws_init));
		if (error)
			return error;
	}

	error = regmap_update_bits(iqs269->regmap, IQS269_HALL_UI,
				   IQS269_HALL_UI_ENABLE,
				   iqs269->hall_enable ? ~0 : 0);
	if (error)
		return error;

	error = regmap_raw_write(iqs269->regmap, IQS269_SYS_SETTINGS,
				 &iqs269->sys_reg, sizeof(iqs269->sys_reg));
	if (error)
		return error;

	/*
	 * The following delay gives the device time to deassert its RDY output
	 * so as to prevent an interrupt from being serviced prematurely.
	 */
	usleep_range(2000, 2100);

	iqs269->ati_current = true;

	return 0;
}

static int iqs269_input_init(struct iqs269_private *iqs269)
{
	struct i2c_client *client = iqs269->client;
	unsigned int sw_code, keycode;
	int error, i, j;

	iqs269->keypad = devm_input_allocate_device(&client->dev);
	if (!iqs269->keypad)
		return -ENOMEM;

	iqs269->keypad->keycodemax = ARRAY_SIZE(iqs269->keycode);
	iqs269->keypad->keycode = iqs269->keycode;
	iqs269->keypad->keycodesize = sizeof(*iqs269->keycode);

	iqs269->keypad->name = "iqs269a_keypad";
	iqs269->keypad->id.bustype = BUS_I2C;

	for (i = 0; i < ARRAY_SIZE(iqs269_events); i++) {
		sw_code = iqs269->switches[i].code;

		for (j = 0; j < IQS269_NUM_CH; j++) {
			keycode = iqs269->keycode[i * IQS269_NUM_CH + j];

			/*
			 * Hall-effect sensing repurposes a pair of dedicated
			 * channels, only one of which reports events.
			 */
			switch (j) {
			case IQS269_CHx_HALL_ACTIVE:
				if (iqs269->hall_enable &&
				    iqs269->switches[i].enabled)
					input_set_capability(iqs269->keypad,
							     EV_SW, sw_code);
				fallthrough;

			case IQS269_CHx_HALL_INACTIVE:
				if (iqs269->hall_enable)
					continue;
				fallthrough;

			default:
				if (keycode != KEY_RESERVED)
					input_set_capability(iqs269->keypad,
							     EV_KEY, keycode);
			}
		}
	}

	for (i = 0; i < IQS269_NUM_SL; i++) {
		if (iqs269_slider_type(iqs269, i) == IQS269_SLIDER_NONE)
			continue;

		iqs269->slider[i] = devm_input_allocate_device(&client->dev);
		if (!iqs269->slider[i])
			return -ENOMEM;

		iqs269->slider[i]->keycodemax = ARRAY_SIZE(iqs269->sl_code[i]);
		iqs269->slider[i]->keycode = iqs269->sl_code[i];
		iqs269->slider[i]->keycodesize = sizeof(**iqs269->sl_code);

		iqs269->slider[i]->name = i ? "iqs269a_slider_1"
					    : "iqs269a_slider_0";
		iqs269->slider[i]->id.bustype = BUS_I2C;

		for (j = 0; j < IQS269_NUM_GESTURES; j++)
			if (iqs269->sl_code[i][j] != KEY_RESERVED)
				input_set_capability(iqs269->slider[i], EV_KEY,
						     iqs269->sl_code[i][j]);

		/*
		 * Present the slider as a narrow trackpad if one or more chan-
		 * nels have been selected to participate, but no gestures have
		 * been mapped to a keycode.
		 */
		if (iqs269_slider_type(iqs269, i) == IQS269_SLIDER_RAW) {
			input_set_capability(iqs269->slider[i],
					     EV_KEY, BTN_TOUCH);
			input_set_abs_params(iqs269->slider[i],
					     ABS_X, 0, 255, 0, 0);
		}

		error = input_register_device(iqs269->slider[i]);
		if (error) {
			dev_err(&client->dev,
				"Failed to register slider %d: %d\n", i, error);
			return error;
		}
	}

	return 0;
}

static int iqs269_report(struct iqs269_private *iqs269)
{
	struct i2c_client *client = iqs269->client;
	struct iqs269_flags flags;
	unsigned int sw_code, keycode;
	int error, i, j;
	u8 slider_x[IQS269_NUM_SL];
	u8 dir_mask, state;

	error = regmap_raw_read(iqs269->regmap, IQS269_SYS_FLAGS, &flags,
				sizeof(flags));
	if (error) {
		dev_err(&client->dev, "Failed to read device status: %d\n",
			error);
		return error;
	}

	/*
	 * The device resets itself if its own watchdog bites, which can happen
	 * in the event of an I2C communication error. In this case, the device
	 * asserts a SHOW_RESET interrupt and all registers must be restored.
	 */
	if (be16_to_cpu(flags.system) & IQS269_SYS_FLAGS_SHOW_RESET) {
		dev_err(&client->dev, "Unexpected device reset\n");

		error = iqs269_dev_init(iqs269);
		if (error)
			dev_err(&client->dev,
				"Failed to re-initialize device: %d\n", error);

		return error;
	}

	if (be16_to_cpu(flags.system) & IQS269_SYS_FLAGS_IN_ATI)
		return 0;

	if (iqs269_slider_type(iqs269, 0) == IQS269_SLIDER_RAW ||
	    iqs269_slider_type(iqs269, 1) == IQS269_SLIDER_RAW) {
		error = regmap_raw_read(iqs269->regmap, IQS269_SLIDER_X,
					slider_x, sizeof(slider_x));
		if (error) {
			dev_err(&client->dev,
				"Failed to read slider position: %d\n", error);
			return error;
		}
	}

	for (i = 0; i < IQS269_NUM_SL; i++) {
		flags.gesture >>= (i * IQS269_NUM_GESTURES);

		switch (iqs269_slider_type(iqs269, i)) {
		case IQS269_SLIDER_NONE:
			continue;

		case IQS269_SLIDER_KEY:
			for (j = 0; j < IQS269_NUM_GESTURES; j++)
				input_report_key(iqs269->slider[i],
						 iqs269->sl_code[i][j],
						 flags.gesture & BIT(j));

			if (!(flags.gesture & (BIT(IQS269_GESTURE_FLICK_NEG) |
					       BIT(IQS269_GESTURE_FLICK_POS) |
					       BIT(IQS269_GESTURE_TAP))))
				break;

			input_sync(iqs269->slider[i]);

			/*
			 * Momentary gestures are followed by a complementary
			 * release cycle so as to emulate a full keystroke.
			 */
			for (j = 0; j < IQS269_NUM_GESTURES; j++)
				if (j != IQS269_GESTURE_HOLD)
					input_report_key(iqs269->slider[i],
							 iqs269->sl_code[i][j],
							 0);
			break;

		case IQS269_SLIDER_RAW:
			/*
			 * The slider is considered to be in a state of touch
			 * if any selected channels are in a state of touch.
			 */
			state = flags.states[IQS269_ST_OFFS_TOUCH];
			state &= iqs269->sys_reg.slider_select[i];

			input_report_key(iqs269->slider[i], BTN_TOUCH, state);

			if (state)
				input_report_abs(iqs269->slider[i],
						 ABS_X, slider_x[i]);
			break;
		}

		input_sync(iqs269->slider[i]);
	}

	for (i = 0; i < ARRAY_SIZE(iqs269_events); i++) {
		dir_mask = flags.states[IQS269_ST_OFFS_DIR];
		if (!iqs269_events[i].dir_up)
			dir_mask = ~dir_mask;

		state = flags.states[iqs269_events[i].st_offs] & dir_mask;

		sw_code = iqs269->switches[i].code;

		for (j = 0; j < IQS269_NUM_CH; j++) {
			keycode = iqs269->keycode[i * IQS269_NUM_CH + j];

			switch (j) {
			case IQS269_CHx_HALL_ACTIVE:
				if (iqs269->hall_enable &&
				    iqs269->switches[i].enabled)
					input_report_switch(iqs269->keypad,
							    sw_code,
							    state & BIT(j));
				fallthrough;

			case IQS269_CHx_HALL_INACTIVE:
				if (iqs269->hall_enable)
					continue;
				fallthrough;

			default:
				input_report_key(iqs269->keypad, keycode,
						 state & BIT(j));
			}
		}
	}

	input_sync(iqs269->keypad);

	/*
	 * The following completion signals that ATI has finished, any initial
	 * switch states have been reported and the keypad can be registered.
	 */
	complete_all(&iqs269->ati_done);

	return 0;
}

static irqreturn_t iqs269_irq(int irq, void *context)
{
	struct iqs269_private *iqs269 = context;

	if (iqs269_report(iqs269))
		return IRQ_NONE;

	/*
	 * The device does not deassert its interrupt (RDY) pin until shortly
	 * after receiving an I2C stop condition; the following delay ensures
	 * the interrupt handler does not return before this time.
	 */
	iqs269_irq_wait();

	return IRQ_HANDLED;
}

static ssize_t counts_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	struct i2c_client *client = iqs269->client;
	__le16 counts;
	int error;

	if (!iqs269->ati_current || iqs269->hall_enable)
		return -EPERM;

	if (!completion_done(&iqs269->ati_done))
		return -EBUSY;

	/*
	 * Unsolicited I2C communication prompts the device to assert its RDY
	 * pin, so disable the interrupt line until the operation is finished
	 * and RDY has been deasserted.
	 */
	disable_irq(client->irq);

	error = regmap_raw_read(iqs269->regmap,
				IQS269_CHx_COUNTS + iqs269->ch_num * 2,
				&counts, sizeof(counts));

	iqs269_irq_wait();
	enable_irq(client->irq);

	if (error)
		return error;

	return sysfs_emit(buf, "%u\n", le16_to_cpu(counts));
}

static ssize_t hall_bin_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;
	struct i2c_client *client = iqs269->client;
	unsigned int val;
	int error;

	disable_irq(client->irq);

	error = regmap_read(iqs269->regmap, IQS269_CAL_DATA_A, &val);

	iqs269_irq_wait();
	enable_irq(client->irq);

	if (error)
		return error;

	switch (ch_reg[IQS269_CHx_HALL_ACTIVE].rx_enable &
		ch_reg[IQS269_CHx_HALL_INACTIVE].rx_enable) {
	case IQS269_HALL_PAD_R:
		val &= IQS269_CAL_DATA_A_HALL_BIN_R_MASK;
		val >>= IQS269_CAL_DATA_A_HALL_BIN_R_SHIFT;
		break;

	case IQS269_HALL_PAD_L:
		val &= IQS269_CAL_DATA_A_HALL_BIN_L_MASK;
		val >>= IQS269_CAL_DATA_A_HALL_BIN_L_SHIFT;
		break;

	default:
		return -EINVAL;
	}

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t hall_enable_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", iqs269->hall_enable);
}

static ssize_t hall_enable_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	unsigned int val;
	int error;

	error = kstrtouint(buf, 10, &val);
	if (error)
		return error;

	guard(mutex)(&iqs269->lock);

	iqs269->hall_enable = val;
	iqs269->ati_current = false;

	return count;
}

static ssize_t ch_number_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", iqs269->ch_num);
}

static ssize_t ch_number_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	unsigned int val;
	int error;

	error = kstrtouint(buf, 10, &val);
	if (error)
		return error;

	if (val >= IQS269_NUM_CH)
		return -EINVAL;

	iqs269->ch_num = val;

	return count;
}

static ssize_t rx_enable_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;

	return sysfs_emit(buf, "%u\n", ch_reg[iqs269->ch_num].rx_enable);
}

static ssize_t rx_enable_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	struct iqs269_ch_reg *ch_reg = iqs269->sys_reg.ch_reg;
	unsigned int val;
	int error;

	error = kstrtouint(buf, 10, &val);
	if (error)
		return error;

	if (val > 0xFF)
		return -EINVAL;

	guard(mutex)(&iqs269->lock);

	ch_reg[iqs269->ch_num].rx_enable = val;
	iqs269->ati_current = false;

	return count;
}

static ssize_t ati_mode_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	unsigned int val;
	int error;

	error = iqs269_ati_mode_get(iqs269, iqs269->ch_num, &val);
	if (error)
		return error;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t ati_mode_store(struct device *dev,
			      struct device_attribute *attr, const char *buf,
			      size_t count)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	unsigned int val;
	int error;

	error = kstrtouint(buf, 10, &val);
	if (error)
		return error;

	error = iqs269_ati_mode_set(iqs269, iqs269->ch_num, val);
	if (error)
		return error;

	return count;
}

static ssize_t ati_base_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	unsigned int val;
	int error;

	error = iqs269_ati_base_get(iqs269, iqs269->ch_num, &val);
	if (error)
		return error;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t ati_base_store(struct device *dev,
			      struct device_attribute *attr, const char *buf,
			      size_t count)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	unsigned int val;
	int error;

	error = kstrtouint(buf, 10, &val);
	if (error)
		return error;

	error = iqs269_ati_base_set(iqs269, iqs269->ch_num, val);
	if (error)
		return error;

	return count;
}

static ssize_t ati_target_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	unsigned int val;
	int error;

	error = iqs269_ati_target_get(iqs269, iqs269->ch_num, &val);
	if (error)
		return error;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t ati_target_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	unsigned int val;
	int error;

	error = kstrtouint(buf, 10, &val);
	if (error)
		return error;

	error = iqs269_ati_target_set(iqs269, iqs269->ch_num, val);
	if (error)
		return error;

	return count;
}

static ssize_t ati_trigger_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n",
			  iqs269->ati_current &&
			  completion_done(&iqs269->ati_done));
}

static ssize_t ati_trigger_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	struct i2c_client *client = iqs269->client;
	unsigned int val;
	int error;

	error = kstrtouint(buf, 10, &val);
	if (error)
		return error;

	if (!val)
		return count;

	disable_irq(client->irq);
	reinit_completion(&iqs269->ati_done);

	error = iqs269_dev_init(iqs269);

	iqs269_irq_wait();
	enable_irq(client->irq);

	if (error)
		return error;

	if (!wait_for_completion_timeout(&iqs269->ati_done,
					 msecs_to_jiffies(2000)))
		return -ETIMEDOUT;

	return count;
}

static DEVICE_ATTR_RO(counts);
static DEVICE_ATTR_RO(hall_bin);
static DEVICE_ATTR_RW(hall_enable);
static DEVICE_ATTR_RW(ch_number);
static DEVICE_ATTR_RW(rx_enable);
static DEVICE_ATTR_RW(ati_mode);
static DEVICE_ATTR_RW(ati_base);
static DEVICE_ATTR_RW(ati_target);
static DEVICE_ATTR_RW(ati_trigger);

static struct attribute *iqs269_attrs[] = {
	&dev_attr_counts.attr,
	&dev_attr_hall_bin.attr,
	&dev_attr_hall_enable.attr,
	&dev_attr_ch_number.attr,
	&dev_attr_rx_enable.attr,
	&dev_attr_ati_mode.attr,
	&dev_attr_ati_base.attr,
	&dev_attr_ati_target.attr,
	&dev_attr_ati_trigger.attr,
	NULL,
};
ATTRIBUTE_GROUPS(iqs269);

static const struct regmap_config iqs269_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = IQS269_MAX_REG,
};

static int iqs269_probe(struct i2c_client *client)
{
	struct iqs269_private *iqs269;
	int error;

	iqs269 = devm_kzalloc(&client->dev, sizeof(*iqs269), GFP_KERNEL);
	if (!iqs269)
		return -ENOMEM;

	i2c_set_clientdata(client, iqs269);
	iqs269->client = client;

	iqs269->regmap = devm_regmap_init_i2c(client, &iqs269_regmap_config);
	if (IS_ERR(iqs269->regmap)) {
		error = PTR_ERR(iqs269->regmap);
		dev_err(&client->dev, "Failed to initialize register map: %d\n",
			error);
		return error;
	}

	mutex_init(&iqs269->lock);
	init_completion(&iqs269->ati_done);

	iqs269->otp_option = (uintptr_t)device_get_match_data(&client->dev);

	error = regmap_raw_read(iqs269->regmap, IQS269_VER_INFO,
				&iqs269->ver_info, sizeof(iqs269->ver_info));
	if (error)
		return error;

	if (iqs269->ver_info.prod_num != IQS269_VER_INFO_PROD_NUM) {
		dev_err(&client->dev, "Unrecognized product number: 0x%02X\n",
			iqs269->ver_info.prod_num);
		return -EINVAL;
	}

	error = iqs269_parse_prop(iqs269);
	if (error)
		return error;

	error = iqs269_dev_init(iqs269);
	if (error) {
		dev_err(&client->dev, "Failed to initialize device: %d\n",
			error);
		return error;
	}

	error = iqs269_input_init(iqs269);
	if (error)
		return error;

	error = devm_request_threaded_irq(&client->dev, client->irq,
					  NULL, iqs269_irq, IRQF_ONESHOT,
					  client->name, iqs269);
	if (error) {
		dev_err(&client->dev, "Failed to request IRQ: %d\n", error);
		return error;
	}

	if (!wait_for_completion_timeout(&iqs269->ati_done,
					 msecs_to_jiffies(2000))) {
		dev_err(&client->dev, "Failed to complete ATI\n");
		return -ETIMEDOUT;
	}

	/*
	 * The keypad may include one or more switches and is not registered
	 * until ATI is complete and the initial switch states are read.
	 */
	error = input_register_device(iqs269->keypad);
	if (error) {
		dev_err(&client->dev, "Failed to register keypad: %d\n", error);
		return error;
	}

	return error;
}

static u16 iqs269_general_get(struct iqs269_private *iqs269)
{
	u16 general = be16_to_cpu(iqs269->sys_reg.general);

	general &= ~IQS269_SYS_SETTINGS_REDO_ATI;
	general &= ~IQS269_SYS_SETTINGS_ACK_RESET;

	return general | IQS269_SYS_SETTINGS_DIS_AUTO;
}

static int iqs269_suspend(struct device *dev)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	struct i2c_client *client = iqs269->client;
	int error;
	u16 general = iqs269_general_get(iqs269);

	if (!(general & IQS269_SYS_SETTINGS_PWR_MODE_MASK))
		return 0;

	disable_irq(client->irq);

	error = regmap_write(iqs269->regmap, IQS269_SYS_SETTINGS, general);

	iqs269_irq_wait();
	enable_irq(client->irq);

	return error;
}

static int iqs269_resume(struct device *dev)
{
	struct iqs269_private *iqs269 = dev_get_drvdata(dev);
	struct i2c_client *client = iqs269->client;
	int error;
	u16 general = iqs269_general_get(iqs269);

	if (!(general & IQS269_SYS_SETTINGS_PWR_MODE_MASK))
		return 0;

	disable_irq(client->irq);

	error = regmap_write(iqs269->regmap, IQS269_SYS_SETTINGS,
			     general & ~IQS269_SYS_SETTINGS_PWR_MODE_MASK);
	if (!error)
		error = regmap_write(iqs269->regmap, IQS269_SYS_SETTINGS,
				     general & ~IQS269_SYS_SETTINGS_DIS_AUTO);

	iqs269_irq_wait();
	enable_irq(client->irq);

	return error;
}

static DEFINE_SIMPLE_DEV_PM_OPS(iqs269_pm, iqs269_suspend, iqs269_resume);

static const struct of_device_id iqs269_of_match[] = {
	{
		.compatible = "azoteq,iqs269a",
		.data = (void *)IQS269_OTP_OPTION_DEFAULT,
	},
	{
		.compatible = "azoteq,iqs269a-00",
		.data = (void *)IQS269_OTP_OPTION_DEFAULT,
	},
	{
		.compatible = "azoteq,iqs269a-d0",
		.data = (void *)IQS269_OTP_OPTION_TWS,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, iqs269_of_match);

static struct i2c_driver iqs269_i2c_driver = {
	.driver = {
		.name = "iqs269a",
		.dev_groups = iqs269_groups,
		.of_match_table = iqs269_of_match,
		.pm = pm_sleep_ptr(&iqs269_pm),
	},
	.probe = iqs269_probe,
};
module_i2c_driver(iqs269_i2c_driver);

MODULE_AUTHOR("Jeff LaBundy <jeff@labundy.com>");
MODULE_DESCRIPTION("Azoteq IQS269A Capacitive Touch Controller");
MODULE_LICENSE("GPL");
