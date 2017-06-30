/* Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/firmware.h>
#include <linux/completion.h>
#include <linux/mfd/msm-cdc-pinctrl.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include "wcd-mbhc-v2.h"
#include "wcdcal-hwdep.h"
/* HTC_AUD_START */
#include <linux/mfd/wcd934x/registers.h>
/* HTC_AUD_END */

#define WCD_MBHC_JACK_MASK (SND_JACK_HEADSET | SND_JACK_OC_HPHL | \
			   SND_JACK_OC_HPHR | SND_JACK_LINEOUT | \
			   SND_JACK_MECHANICAL | SND_JACK_MICROPHONE2 | \
			   SND_JACK_UNSUPPORTED)

#define WCD_MBHC_JACK_BUTTON_MASK (SND_JACK_BTN_0 | SND_JACK_BTN_1 | \
				  SND_JACK_BTN_2 | SND_JACK_BTN_3 | \
				  SND_JACK_BTN_4 | SND_JACK_BTN_5 )
#define OCP_ATTEMPT 20
/* HTC_AUD_START - reduce cable detection time */
#if 0
#define HS_DETECT_PLUG_TIME_MS (3 * 1000)
#else
#define HS_DETECT_PLUG_TIME_MS (1 * 1000)
#endif
/* HTC_AUD_END */
#define SPECIAL_HS_DETECT_TIME_MS (2 * 1000)
#define MBHC_BUTTON_PRESS_THRESHOLD_MIN 250
#define GND_MIC_SWAP_THRESHOLD 4
#define WCD_FAKE_REMOVAL_MIN_PERIOD_MS 100
#define HS_VREF_MIN_VAL 1400
#define FW_READ_ATTEMPTS 15
#define FW_READ_TIMEOUT 4000000
#define FAKE_REM_RETRY_ATTEMPTS 3
#define MAX_IMPED 60000

#define WCD_MBHC_BTN_PRESS_COMPL_TIMEOUT_MS  50
#define ANC_DETECT_RETRY_CNT 7
#define WCD_MBHC_SPL_HS_CNT  2

/* HTC_AUD_START - AS HS */
#define HTC_AS_HEADSET_DEBOUNCE_TIME 50000
#define HTC_AS_HEADSET_DETECT_RETRY 30
#define MIC_BIAS_2V85 2850
/* HTC_AUD_END */

static int det_extn_cable_en;
module_param(det_extn_cable_en, int,
		S_IRUGO | S_IWUSR | S_IWGRP);
MODULE_PARM_DESC(det_extn_cable_en, "enable/disable extn cable detect");

enum wcd_mbhc_cs_mb_en_flag {
	WCD_MBHC_EN_CS = 0,
	WCD_MBHC_EN_MB,
	WCD_MBHC_EN_PULLUP,
	WCD_MBHC_EN_NONE,
};

/* HTC_AUD_START - for simulate */
struct wcd_mbhc* __MBHC = NULL;

static int __CTOI(char s)
{
	int ret = 0;
	if (s >= 'a' && s <= 'f') {
		ret = 10 + s - 'a';
	} else if (s >= 'A' && s <= 'F') {
		ret = 10 + s - 'A';
	} else if (s >= '0' && s <= '9') {
		ret = s - '0';
	}
	return ret;
}
static int __ATOI(char *s)
{
	int ret = 0;
	if (strlen(s) == 3) {
		ret += __CTOI(s[0]) * 256;
		ret += __CTOI(s[1]) * 16;
		ret += __CTOI(s[2]);
	}
	pr_info("[AUD][HS] ATOI ret = %d\n", ret);
	return ret;
}
static void dump_register(u16 reg)
{
	int i = 0;

	if (__MBHC == NULL) {
		pr_err("[AUD][HS] __MBHC is NULL\n");
		return;
	}

	if (reg == 0 || reg > 0x6FF) {
		pr_err("[AUD][HS] wrong register\n");
		return;
	}

	if (__MBHC->debug_reg_count >= 50) {
		pr_err("[AUD][HS] debug count more than 50\n");
		return;
	}

	for (i = 0; i < __MBHC->debug_reg_count; i ++) {
		if (__MBHC->debug_reg[i] == reg) break;
	}

	if (i == __MBHC->debug_reg_count) {
		__MBHC->debug_reg[__MBHC->debug_reg_count] = reg;
		__MBHC->debug_reg_count ++;
	}
	pr_info("[AUD][HS] dump reg %x, current dump count = %d\n",
			reg, __MBHC->debug_reg_count);
/*
	for (i = 0; i < __MBHC->debug_reg_count; i ++)
		pr_info("[AUD][HS] reg %x will be dumpped\n", __MBHC->debug_reg[i]);
*/
}

static void undump_register(u16 reg)
{
	int i = 0;

	if (__MBHC == NULL) {
		pr_err("[AUD][HS] __MBHC is NULL\n");
		return;
	}

	if (reg == 0 || reg > 0x6FF) {
		pr_err("[AUD][HS] wrong register\n");
		return;
	}

	for (i = 0; i < __MBHC->debug_reg_count; i ++)
		if (__MBHC->debug_reg[i] == reg) break;

	if (i < __MBHC->debug_reg_count) {
		for (; i < __MBHC->debug_reg_count; i ++)
			if (i + 1 < __MBHC->debug_reg_count)
				__MBHC->debug_reg[i] = __MBHC->debug_reg[i + 1];
		__MBHC->debug_reg_count --;
	}

	pr_info("[AUD][HS] undump reg %x, current dump count = %d\n",
			reg, __MBHC->debug_reg_count);
/*
	for (i = 0; i < __MBHC->debug_reg_count; i ++)
		pr_info("[AUD][HS] reg %x will be dumpped\n", __MBHC->debug_reg[i]);
*/
}

/* Add attribute on sysfs for debugging */
static ssize_t debug_flag_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct snd_soc_codec *codec;
	int len = 0, i;
	char *s;
	s = buf;
	if (__MBHC == NULL) {
		return (buf - s);
	}

	codec = __MBHC->codec;

	len =  scnprintf(buf, PAGE_SIZE - 1, "HP_DET = %d\n", (__MBHC->current_plug != MBHC_PLUG_TYPE_NONE) ? 0 : 1);
	buf += len;

	for (i = 0; i < __MBHC->debug_reg_count; i ++) {
		len =  scnprintf(buf, PAGE_SIZE - 1, "reg 0x%x value 0x%x\n", __MBHC->debug_reg[i],
					   snd_soc_read(codec, __MBHC->debug_reg[i]));
		buf += len;
	}

	return (buf - s);
}

static ssize_t debug_flag_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	u16 reg;
	char s[4] = {'\0'};
//	pr_info("%s %s %d\n", __func__, buf, count);
	if (strncmp(buf, "enable", count - 1) == 0) {
		pr_info("[AUD][HS] debug enable\n");
	} else if (strncmp(buf, "disable", count - 1) == 0) {
		pr_info("[AUD][HS] debug disable\n");
	} else if (strncmp(buf, "dump 0x", count - 4) == 0) {
		s[0] = buf[7]; s[1] = buf[8]; s[2] = buf[9];
		reg = __ATOI(s);
		dump_register(reg);
	} else if (strncmp(buf, "undump 0x", count - 4) == 0) {
		s[0] = buf[9]; s[1] = buf[10]; s[2] = buf[11];
		reg = __ATOI(s);
		undump_register(reg);
	} else if (strncmp(buf, "no_headset", count - 1) == 0) {
		pr_info("[AUD][HS] set no headset status\n");
	} else {
		pr_err("[AUD][HS] Invalid parameter");
		return count;
	}

	return count;
}

static DEVICE_ACCESSORY_ATTR(debug, 0644, debug_flag_show, debug_flag_store);

/* HTC_AUD_START - AS HS */

static int disable_anc_mic(struct wcd_mbhc *mbhc)
{
	struct snd_soc_codec *codec = mbhc->codec;
	pr_info("%s disable anc mic to avoid pop\n", __func__);
	snd_soc_update_bits(codec, WCD934X_CDC_RX1_RX_PATH_CFG0, 0x10, 0x00);
	snd_soc_update_bits(codec, WCD934X_CDC_RX2_RX_PATH_CFG0, 0x10, 0x00);
	return 0;
}

static ssize_t headset_switch_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct htc_headset_config hs_cfg = __MBHC->mbhc_cfg->htc_headset_cfg;

	pr_info("%s headset switch S3 status [PMI8998 GPIO%d: %d, GPIO%d: %d]\n", __func__,
		hs_cfg.switch_gpio[HEADSET_S3_0],
		gpio_get_value(hs_cfg.switch_gpio[HEADSET_S3_0]),
		hs_cfg.switch_gpio[HEADSET_S3_1],
		gpio_get_value(hs_cfg.switch_gpio[HEADSET_S3_1]));
	pr_info("%s headset switch S4 status [GPIO%d: %d]\n", __func__,
		hs_cfg.switch_gpio[HEADSET_S4],
		gpio_get_value(hs_cfg.switch_gpio[HEADSET_S4]));
	pr_info("%s headset switch S5 status [PM8998 GPIO%d: %d]\n", __func__,
		hs_cfg.switch_gpio[HEADSET_S5],
		gpio_get_value(hs_cfg.switch_gpio[HEADSET_S5]));
	pr_info("%s headset ID1 status [GPIO%d: %d]\n", __func__,
		hs_cfg.id_gpio[TYPEC_ID1],
		gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1]));
	pr_info("%s headset ID2 status [GPIO%d: %d]\n", __func__,
		hs_cfg.id_gpio[TYPEC_ID2],
		gpio_get_value(hs_cfg.id_gpio[TYPEC_ID2]));
	pr_info("%s headset USB Position status [GPIO%d: %d]\n", __func__,
		hs_cfg.id_gpio[TYPEC_POSITION],
		gpio_get_value(hs_cfg.id_gpio[TYPEC_POSITION]));
	pr_info("%s headset external micbias status [PM8998 GPIO%d: %d]\n", __func__,
		hs_cfg.ext_micbias,
		gpio_get_value(hs_cfg.ext_micbias));
/*
	pr_info("%s headset MBHC det pin status [GPIO%d: %d]\n", __func__,
		hs_cfg.mbhc_det_pin,
		gpio_get_value(hs_cfg.mbhc_det_pin));
*/
	pr_info("%s headset USB FSA3030 Sel0 status [GPIO%d: %d]\n", __func__,
		hs_cfg.fsa3030_sel0,
		gpio_get_value(hs_cfg.fsa3030_sel0));
	pr_info("%s headset  USB FSA3030 Sel1 status [GPIO%d: %d]\n", __func__,
		hs_cfg.fsa3030_sel1,
		gpio_get_value(hs_cfg.fsa3030_sel1));
	pr_info("%s headset USB FSA3030 Sel2 status [GPIO%d: %d]\n", __func__,
		hs_cfg.fsa3030_sel2,
		gpio_get_value(hs_cfg.fsa3030_sel2));
	return 0;
}

static ssize_t headset_switch_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct htc_headset_config hs_cfg = __MBHC->mbhc_cfg->htc_headset_cfg;
	pr_info("%s ++\n", __func__);
	if (strncmp(buf, "s3_h", count - 1) == 0) {
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_0], 1);
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_1], 1);
	} else if (strncmp(buf, "s3_l", count - 1) == 0) {
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_0], 0);
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_1], 0);
	} else if (strncmp(buf, "s4_h", count - 1) == 0) {
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S4], 1);
	} else if (strncmp(buf, "s4_l", count - 1) == 0) {
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S4], 0);
	} else if (strncmp(buf, "s5_h", count - 1) == 0) {
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5], 1);
	} else if (strncmp(buf, "s5_l", count - 1) == 0) {
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5], 0);
	} else if (strncmp(buf, "id1_l", count - 1) == 0) {
		gpio_set_value(hs_cfg.id_gpio[TYPEC_ID1], 0);
	} else if (strncmp(buf, "id1_h", count - 1) == 0) {
		gpio_set_value(hs_cfg.id_gpio[TYPEC_ID1], 1);
	} else if (strncmp(buf, "id2_l", count - 1) == 0) {
		gpio_set_value(hs_cfg.id_gpio[TYPEC_ID2], 0);
	} else if (strncmp(buf, "id2_h", count - 1) == 0) {
		gpio_set_value(hs_cfg.id_gpio[TYPEC_ID2], 1);
	} else if (strncmp(buf, "usb_pos_l", count - 1) == 0) {
		gpio_set_value(hs_cfg.id_gpio[TYPEC_POSITION], 0);
	} else if (strncmp(buf, "usb_pos_h", count - 1) == 0) {
		gpio_set_value(hs_cfg.id_gpio[TYPEC_POSITION], 1);
	} else if (strncmp(buf, "ext_micb_l", count - 1) == 0) {
		gpio_set_value(hs_cfg.ext_micbias, 0);
	} else if (strncmp(buf, "ext_micb_h", count - 1) == 0) {
		gpio_set_value(hs_cfg.ext_micbias, 1);
/*
	} else if (strncmp(buf, "hs_det_h", count - 1) == 0) {
		gpio_set_value(hs_cfg.mbhc_det_pin, 1);
	} else if (strncmp(buf, "hs_det_l", count - 1) == 0) {
		gpio_set_value(hs_cfg.mbhc_det_pin, 0);
*/
	} else if (strncmp(buf, "fsa_usb", count - 1) == 0) {
		gpio_set_value(hs_cfg.fsa3030_sel0, 0);
		gpio_set_value(hs_cfg.fsa3030_sel1, 1);
	} else if (strncmp(buf, "fsa_aud", count - 1) == 0) {
		gpio_set_value(hs_cfg.fsa3030_sel0, 1);
		gpio_set_value(hs_cfg.fsa3030_sel1, 0);
	} else {
		pr_err("%s: error setting\n", __func__);
	}
	pr_info("%s --\n", __func__);
	return count;
}

static DEVICE_HEADSET_ATTR(switch, 0644, headset_switch_show,
			   headset_switch_store);
/* HTC_AUD_END */

static ssize_t headset_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int length = 0;
	char *state = NULL;

	switch (__MBHC->current_plug) {
	case MBHC_PLUG_TYPE_NONE:
		state = "headset_unplug";
		break;
	case MBHC_PLUG_TYPE_HEADSET:
		state = "headset_mic";
		break;
	case MBHC_PLUG_TYPE_HEADPHONE:
		state = "headset_no_mic";
		break;
	case MBHC_PLUG_TYPE_HIGH_HPH:
		state = "headset_tv_out";
		break;
	case MBHC_PLUG_TYPE_GND_MIC_SWAP:
		state = "headset_gnd_mic_swap";
		break;
/* HTC_AUD_START - AS HS */
	case MBHC_PLUG_TYPE_AS_HEADSET:
		state = "AS_headset_mic";
		break;
	case MBHC_PLUG_TYPE_35MM_HEADSET:
		state = "35mm_headset";
		break;
	case MBHC_PLUG_TYPE_25MM_HEADSET:
		state = "25mm_headset";
		break;
/* HTC_AUD_END */
	default:
		state = "error_state";
	}

	length = scnprintf(buf, PAGE_SIZE - 1, "%s\n", state);

	return length;
}

static ssize_t headset_state_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	pr_info("[AUD][HS] %s\n", __func__);
	return 0;
}

static DEVICE_HEADSET_ATTR(state, 0644, headset_state_show,
			   headset_state_store);

static ssize_t headset_simulate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE - 1, "Command is not supported\n");
}

static ssize_t headset_simulate_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (strncmp(buf, "headset_unplug", count - 1) == 0) {
		pr_info("Headset simulation: headset_unplug\n");
		__MBHC->current_plug = MBHC_PLUG_TYPE_NONE;
		__MBHC->hph_status &= ~WCD_MBHC_JACK_MASK;
	} else if (strncmp(buf, "headset_no_mic", count - 1) == 0) {
		pr_info("Headset simulation: headset_no_mic\n");
		__MBHC->current_plug = MBHC_PLUG_TYPE_HEADPHONE;
		__MBHC->hph_status &= ~WCD_MBHC_JACK_MASK;
		__MBHC->hph_status |= SND_JACK_HEADPHONE;
	} else if (strncmp(buf, "headset_mic", count - 1) == 0) {
		pr_info("Headset simulation: headset_mic\n");
		__MBHC->current_plug = MBHC_PLUG_TYPE_HEADSET;
		__MBHC->hph_status &= ~WCD_MBHC_JACK_MASK;
		__MBHC->hph_status |= SND_JACK_HEADSET;
/* HTC_AUD_START - AS HS */
	} else if (strncmp(buf, "as_headset_mic", count - 1) == 0) {
		pr_info("Headset simulation: as_headset_mic\n");
		__MBHC->current_plug = MBHC_PLUG_TYPE_AS_HEADSET;
		__MBHC->hph_status &= ~WCD_MBHC_JACK_MASK;
		__MBHC->hph_status |= SND_JACK_HEADSET;
	} else if (strncmp(buf, "25mm_headset", count - 1) == 0) {
		pr_info("Headset simulation: 25mm_headset\n");
		__MBHC->current_plug = MBHC_PLUG_TYPE_25MM_HEADSET;
		__MBHC->hph_status &= ~WCD_MBHC_JACK_MASK;
		__MBHC->hph_status |= SND_JACK_HEADSET;
/* HTC_AUD_END */
	} else {
		pr_info("Invalid parameter\n");
		return count;
	}

	snd_soc_jack_report(&__MBHC->headset_jack, __MBHC->hph_status, WCD_MBHC_JACK_MASK);

	return count;
}

static DEVICE_HEADSET_ATTR(simulate, 0644, headset_simulate_show,
			   headset_simulate_store);

static int register_attributes(struct wcd_mbhc *mbhc)
{
	int ret = 0;
	pr_info("[AUD][HS] %s\n", __func__);
	mbhc->htc_accessory_class = class_create(THIS_MODULE, "htc_accessory");
	if (IS_ERR(mbhc->htc_accessory_class)) {
		ret = PTR_ERR(mbhc->htc_accessory_class);
		mbhc->htc_accessory_class = NULL;
		goto err_create_class;
	}

	/* Register headset attributes */
	mbhc->headset_dev = device_create(mbhc->htc_accessory_class,
					NULL, 0, "%s", "headset");
	if (unlikely(IS_ERR(mbhc->headset_dev))) {
		ret = PTR_ERR(mbhc->headset_dev);
		mbhc->headset_dev = NULL;
		goto err_create_headset_device;
	}

	ret = device_create_file(mbhc->headset_dev, &dev_attr_headset_state);
	if (ret) {
		goto err_create_headset_state_device_file;
	}

	ret = device_create_file(mbhc->headset_dev, &dev_attr_headset_simulate);
	if (ret) {
		goto err_create_headset_simulate_device_file;
	}

/* HTC_AUD_START - AS HS */
	ret = device_create_file(mbhc->headset_dev, &dev_attr_headset_switch);
	if (ret) {
		goto err_create_headset_switch_device_file;
	}
/* HTC_AUD_END */

	/* Register debug attributes */
	mbhc->debug_dev = device_create(mbhc->htc_accessory_class,
				      NULL, 0, "%s", "debug");
	if (unlikely(IS_ERR(mbhc->debug_dev))) {
		ret = PTR_ERR(mbhc->debug_dev);
		mbhc->debug_dev = NULL;
		goto err_create_debug_device;
	}

	ret = device_create_file(mbhc->debug_dev, &dev_attr_debug);
	if (ret)
		goto err_create_debug_device_file;

	return 0;

err_create_debug_device_file:
	device_unregister(mbhc->debug_dev);

err_create_debug_device:
	device_remove_file(mbhc->headset_dev, &dev_attr_headset_switch); /* HTC_AUD - AS HS */

err_create_headset_switch_device_file:
	device_remove_file(mbhc->headset_dev, &dev_attr_headset_simulate);

err_create_headset_simulate_device_file:
	device_remove_file(mbhc->headset_dev, &dev_attr_headset_state);

err_create_headset_state_device_file:
	device_unregister(mbhc->headset_dev);

err_create_headset_device:
	class_destroy(mbhc->htc_accessory_class);

err_create_class:
	pr_err("[AUD][HS] %s error\n", __func__);
	return ret;
}

static void unregister_attributes(struct wcd_mbhc *mbhc)
{
	device_remove_file(mbhc->headset_dev, &dev_attr_headset_switch);/* HTC_AUD - AS HS */
	device_remove_file(mbhc->headset_dev, &dev_attr_headset_simulate);
	device_remove_file(mbhc->headset_dev, &dev_attr_headset_state);
	device_remove_file(mbhc->debug_dev, &dev_attr_debug);
	device_unregister(mbhc->headset_dev);
	device_unregister(mbhc->debug_dev);
	class_destroy(mbhc->htc_accessory_class);
}
/* HTC_AUD_END */

static void wcd_mbhc_jack_report(struct wcd_mbhc *mbhc,
				struct snd_soc_jack *jack, int status, int mask)
{
	snd_soc_jack_report(jack, status, mask);
}

static void __hphocp_off_report(struct wcd_mbhc *mbhc, u32 jack_status,
				int irq)
{
	struct snd_soc_codec *codec = mbhc->codec;

	dev_dbg(codec->dev, "%s: clear ocp status %x\n",
		__func__, jack_status);

	if (mbhc->hph_status & jack_status) {
		mbhc->hph_status &= ~jack_status;
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				     mbhc->hph_status, WCD_MBHC_JACK_MASK);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_OCP_FSM_EN, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_OCP_FSM_EN, 1);
		/*
		 * reset retry counter as PA is turned off signifying
		 * start of new OCP detection session
		 */
		if (mbhc->intr_ids->hph_left_ocp)
			mbhc->hphlocp_cnt = 0;
		else
			mbhc->hphrocp_cnt = 0;
		mbhc->mbhc_cb->irq_control(codec, irq, true);
	}
}

static void hphrocp_off_report(struct wcd_mbhc *mbhc, u32 jack_status)
{
	__hphocp_off_report(mbhc, SND_JACK_OC_HPHR,
			    mbhc->intr_ids->hph_right_ocp);
}

static void hphlocp_off_report(struct wcd_mbhc *mbhc, u32 jack_status)
{
	__hphocp_off_report(mbhc, SND_JACK_OC_HPHL,
			    mbhc->intr_ids->hph_left_ocp);
}

static void wcd_program_hs_vref(struct wcd_mbhc *mbhc)
{
	struct wcd_mbhc_plug_type_cfg *plug_type_cfg;
	struct snd_soc_codec *codec = mbhc->codec;
	u32 reg_val;

	plug_type_cfg = WCD_MBHC_CAL_PLUG_TYPE_PTR(mbhc->mbhc_cfg->calibration);
	reg_val = ((plug_type_cfg->v_hs_max - HS_VREF_MIN_VAL) / 100);

	dev_dbg(codec->dev, "%s: reg_val  = %x\n", __func__, reg_val);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HS_VREF, reg_val);
}

static void wcd_program_btn_threshold(const struct wcd_mbhc *mbhc, bool micbias)
{
	struct wcd_mbhc_btn_detect_cfg *btn_det;
	struct snd_soc_codec *codec = mbhc->codec;
	struct snd_soc_card *card = codec->component.card;
	s16 *btn_low, *btn_high;

	if (mbhc->mbhc_cfg->calibration == NULL) {
		dev_err(card->dev, "%s: calibration data is NULL\n", __func__);
		return;
	}

	btn_det = WCD_MBHC_CAL_BTN_DET_PTR(mbhc->mbhc_cfg->calibration);
	btn_low = btn_det->_v_btn_low;
	btn_high = ((void *)&btn_det->_v_btn_low) +
			(sizeof(btn_det->_v_btn_low[0]) * btn_det->num_btn);

	mbhc->mbhc_cb->set_btn_thr(codec, btn_low, btn_high, btn_det->num_btn,
				   micbias);
}

static void wcd_enable_curr_micbias(const struct wcd_mbhc *mbhc,
				const enum wcd_mbhc_cs_mb_en_flag cs_mb_en)
{

	/*
	 * Some codecs handle micbias/pullup enablement in codec
	 * drivers itself and micbias is not needed for regular
	 * plug type detection. So if micbias_control callback function
	 * is defined, just return.
	 */
	if (mbhc->mbhc_cb->mbhc_micbias_control)
		return;

	pr_debug("%s: enter, cs_mb_en: %d\n", __func__, cs_mb_en);

	switch (cs_mb_en) {
	case WCD_MBHC_EN_CS:
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MICB_CTRL, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 3);
		/* Program Button threshold registers as per CS */
		wcd_program_btn_threshold(mbhc, false);
		break;
	case WCD_MBHC_EN_MB:
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);

		/* Disable PULL_UP_EN & enable MICBIAS */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MICB_CTRL, 2);
		/* Program Button threshold registers as per MICBIAS */
		wcd_program_btn_threshold(mbhc, true);
		break;
	case WCD_MBHC_EN_PULLUP:
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 3);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MICB_CTRL, 1);
		/* Program Button threshold registers as per MICBIAS */
		wcd_program_btn_threshold(mbhc, true);
		break;
	case WCD_MBHC_EN_NONE:
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MICB_CTRL, 0);
		break;
	default:
		pr_debug("%s: Invalid parameter", __func__);
		break;
	}

	pr_debug("%s: exit\n", __func__);
}

static const char *wcd_mbhc_get_event_string(int event)
{
	switch (event) {
	case WCD_EVENT_PRE_MICBIAS_2_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_PRE_MICBIAS_2_OFF);
	case WCD_EVENT_POST_MICBIAS_2_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_POST_MICBIAS_2_OFF);
	case WCD_EVENT_PRE_MICBIAS_2_ON:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_PRE_MICBIAS_2_ON);
	case WCD_EVENT_POST_MICBIAS_2_ON:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_POST_MICBIAS_2_ON);
	case WCD_EVENT_PRE_HPHL_PA_ON:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_PRE_HPHL_PA_ON);
	case WCD_EVENT_POST_HPHL_PA_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_POST_HPHL_PA_OFF);
	case WCD_EVENT_PRE_HPHR_PA_ON:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_PRE_HPHR_PA_ON);
	case WCD_EVENT_POST_HPHR_PA_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_POST_HPHR_PA_OFF);
	case WCD_EVENT_PRE_HPHR_PA_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_PRE_HPHR_PA_OFF);
	case WCD_EVENT_PRE_HPHL_PA_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_PRE_HPHL_PA_OFF);
	case WCD_EVENT_POST_DAPM_MICBIAS_2_ON:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_POST_DAPM_MICBIAS_2_ON);
	case WCD_EVENT_PRE_DAPM_MICBIAS_2_ON:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_PRE_DAPM_MICBIAS_2_ON);
	case WCD_EVENT_POST_DAPM_MICBIAS_2_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_POST_DAPM_MICBIAS_2_OFF);
	case WCD_EVENT_PRE_DAPM_MICBIAS_2_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_PRE_DAPM_MICBIAS_2_OFF);
	case WCD_EVENT_OCP_OFF:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_OCP_OFF);
	case WCD_EVENT_OCP_ON:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_OCP_ON);
	case WCD_EVENT_INVALID:
	default:
		return WCD_MBHC_STRINGIFY(WCD_EVENT_INVALID);
	}
}

static int wcd_event_notify(struct notifier_block *self, unsigned long val,
			    void *data)
{
	struct wcd_mbhc *mbhc = (struct wcd_mbhc *)data;
	enum wcd_notify_event event = (enum wcd_notify_event)val;
	struct snd_soc_codec *codec = mbhc->codec;
	bool micbias2 = false;
	bool micbias1 = false;
	u8 fsm_en = 0;

	pr_debug("%s: event %s (%d)\n", __func__,
		 wcd_mbhc_get_event_string(event), event);
	if (mbhc->mbhc_cb->micbias_enable_status) {
		micbias2 = mbhc->mbhc_cb->micbias_enable_status(mbhc,
								MIC_BIAS_2);
		micbias1 = mbhc->mbhc_cb->micbias_enable_status(mbhc,
								MIC_BIAS_1);
	}
	switch (event) {
	/* MICBIAS usage change */
	case WCD_EVENT_POST_DAPM_MICBIAS_2_ON:
		mbhc->is_hs_recording = true;
		pr_debug("%s: is_capture: %d\n", __func__,
			  mbhc->is_hs_recording);
		break;
	case WCD_EVENT_POST_MICBIAS_2_ON:
		if (!mbhc->micbias_enable)
			goto out_micb_en;
		if (mbhc->mbhc_cb->mbhc_common_micb_ctrl) {
			mbhc->mbhc_cb->mbhc_common_micb_ctrl(codec,
					MBHC_COMMON_MICB_PRECHARGE,
					true);
			mbhc->mbhc_cb->mbhc_common_micb_ctrl(codec,
					MBHC_COMMON_MICB_SET_VAL,
					true);
			/*
			 * Special headset needs MICBIAS as 2.7V so wait for
			 * 50 msec for the MICBIAS to reach 2.7 volts.
			 */
			msleep(50);
		}
		if (mbhc->mbhc_cb->set_auto_zeroing)
			mbhc->mbhc_cb->set_auto_zeroing(codec, true);
		if (mbhc->mbhc_cb->mbhc_common_micb_ctrl)
			mbhc->mbhc_cb->mbhc_common_micb_ctrl(codec,
					MBHC_COMMON_MICB_PRECHARGE,
					false);
out_micb_en:
		/* Disable current source if micbias enabled */
		if (mbhc->mbhc_cb->mbhc_micbias_control) {
			WCD_MBHC_REG_READ(WCD_MBHC_FSM_EN, fsm_en);
			if (fsm_en)
				WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL,
							 0);
		} else {
			mbhc->is_hs_recording = true;
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		}
		/* configure cap settings properly when micbias is enabled */
		if (mbhc->mbhc_cb->set_cap_mode)
			mbhc->mbhc_cb->set_cap_mode(codec, micbias1, true);
		break;
	case WCD_EVENT_PRE_MICBIAS_2_OFF:
		/*
		 * Before MICBIAS_2 is turned off, if FSM is enabled,
		 * make sure current source is enabled so as to detect
		 * button press/release events
		 */
		if (mbhc->mbhc_cb->mbhc_micbias_control &&
		    !mbhc->micbias_enable) {
			WCD_MBHC_REG_READ(WCD_MBHC_FSM_EN, fsm_en);
			if (fsm_en)
				WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL,
							 3);
		}
		break;
	/* MICBIAS usage change */
	case WCD_EVENT_POST_DAPM_MICBIAS_2_OFF:
		mbhc->is_hs_recording = false;
		pr_debug("%s: is_capture: %d\n", __func__,
			  mbhc->is_hs_recording);
		break;
	case WCD_EVENT_POST_MICBIAS_2_OFF:
		if (!mbhc->mbhc_cb->mbhc_micbias_control)
			mbhc->is_hs_recording = false;
		if (mbhc->micbias_enable) {
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
			break;
		}

		if (mbhc->mbhc_cb->set_auto_zeroing)
			mbhc->mbhc_cb->set_auto_zeroing(codec, false);
		if (mbhc->mbhc_cb->set_micbias_value && !mbhc->micbias_enable)
			mbhc->mbhc_cb->set_micbias_value(codec);
		/* Enable PULL UP if PA's are enabled */
		if ((test_bit(WCD_MBHC_EVENT_PA_HPHL, &mbhc->event_state)) ||
				(test_bit(WCD_MBHC_EVENT_PA_HPHR,
					  &mbhc->event_state)))
			/* enable pullup and cs, disable mb */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_PULLUP);
		else
			/* enable current source and disable mb, pullup*/
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);

		/* configure cap settings properly when micbias is disabled */
		if (mbhc->mbhc_cb->set_cap_mode)
			mbhc->mbhc_cb->set_cap_mode(codec, micbias1, false);
		break;
	case WCD_EVENT_PRE_HPHL_PA_OFF:
		mutex_lock(&mbhc->hphl_pa_lock);
		break;
	case WCD_EVENT_POST_HPHL_PA_OFF:
		clear_bit(WCD_MBHC_HPHL_PA_OFF_ACK, &mbhc->hph_pa_dac_state);
		if (mbhc->hph_status & SND_JACK_OC_HPHL)
			hphlocp_off_report(mbhc, SND_JACK_OC_HPHL);
		clear_bit(WCD_MBHC_EVENT_PA_HPHL, &mbhc->event_state);
		/* check if micbias is enabled */
		if (micbias2)
			/* Disable cs, pullup & enable micbias */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		else
			/* Disable micbias, pullup & enable cs */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);
		mutex_unlock(&mbhc->hphl_pa_lock);
		break;
	case WCD_EVENT_PRE_HPHR_PA_OFF:
		mutex_lock(&mbhc->hphr_pa_lock);
		break;
	case WCD_EVENT_POST_HPHR_PA_OFF:
		clear_bit(WCD_MBHC_HPHR_PA_OFF_ACK, &mbhc->hph_pa_dac_state);
		if (mbhc->hph_status & SND_JACK_OC_HPHR)
			hphrocp_off_report(mbhc, SND_JACK_OC_HPHR);
		clear_bit(WCD_MBHC_EVENT_PA_HPHR, &mbhc->event_state);
		/* check if micbias is enabled */
		if (micbias2)
			/* Disable cs, pullup & enable micbias */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		else
			/* Disable micbias, pullup & enable cs */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);
		mutex_unlock(&mbhc->hphr_pa_lock);
		break;
	case WCD_EVENT_PRE_HPHL_PA_ON:
		set_bit(WCD_MBHC_EVENT_PA_HPHL, &mbhc->event_state);
		/* check if micbias is enabled */
		if (micbias2)
			/* Disable cs, pullup & enable micbias */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		else
			/* Disable micbias, enable pullup & cs */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_PULLUP);
		break;
	case WCD_EVENT_PRE_HPHR_PA_ON:
		set_bit(WCD_MBHC_EVENT_PA_HPHR, &mbhc->event_state);
		/* check if micbias is enabled */
		if (micbias2)
			/* Disable cs, pullup & enable micbias */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
		else
			/* Disable micbias, enable pullup & cs */
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_PULLUP);
		break;
	case WCD_EVENT_OCP_OFF:
		mbhc->mbhc_cb->irq_control(mbhc->codec,
					   mbhc->intr_ids->hph_left_ocp,
					   false);
		break;
	case WCD_EVENT_OCP_ON:
		mbhc->mbhc_cb->irq_control(mbhc->codec,
					   mbhc->intr_ids->hph_left_ocp,
					   true);
		break;
	default:
		break;
	}
	return 0;
}

static int wcd_cancel_btn_work(struct wcd_mbhc *mbhc)
{
	int r;

	r = cancel_delayed_work_sync(&mbhc->mbhc_btn_dwork);
	/*
	 * if scheduled mbhc.mbhc_btn_dwork is canceled from here,
	 * we have to unlock from here instead btn_work
	 */
	if (r)
		mbhc->mbhc_cb->lock_sleep(mbhc, false);
	return r;
}

static bool wcd_swch_level_remove(struct wcd_mbhc *mbhc)
{
	u16 result2 = 0;

	WCD_MBHC_REG_READ(WCD_MBHC_SWCH_LEVEL_REMOVE, result2);
	return (result2) ? true : false;
}

/* should be called under interrupt context that hold suspend */
static void wcd_schedule_hs_detect_plug(struct wcd_mbhc *mbhc,
					    struct work_struct *work)
{
	pr_debug("%s: scheduling correct_swch_plug\n", __func__);
	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);
	mbhc->hs_detect_work_stop = false;
	mbhc->mbhc_cb->lock_sleep(mbhc, true);
	schedule_work(work);
}

/* called under codec_resource_lock acquisition */
static void wcd_cancel_hs_detect_plug(struct wcd_mbhc *mbhc,
					 struct work_struct *work)
{
	pr_debug("%s: Canceling correct_plug_swch\n", __func__);
	mbhc->hs_detect_work_stop = true;
	WCD_MBHC_RSC_UNLOCK(mbhc);
	if (cancel_work_sync(work)) {
		pr_debug("%s: correct_plug_swch is canceled\n",
			 __func__);
		mbhc->mbhc_cb->lock_sleep(mbhc, false);
	}
	WCD_MBHC_RSC_LOCK(mbhc);
}

static void wcd_mbhc_clr_and_turnon_hph_padac(struct wcd_mbhc *mbhc)
{
	bool pa_turned_on = false;
	u8 wg_time = 0;

	WCD_MBHC_REG_READ(WCD_MBHC_HPH_CNP_WG_TIME, wg_time);
	wg_time += 1;

	mutex_lock(&mbhc->hphr_pa_lock);
	if (test_and_clear_bit(WCD_MBHC_HPHR_PA_OFF_ACK,
			       &mbhc->hph_pa_dac_state)) {
		pr_debug("%s: HPHR clear flag and enable PA\n", __func__);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HPHR_PA_EN, 1);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HPHR_OCP_DET_EN, 1);
		pa_turned_on = true;
	}
	mutex_unlock(&mbhc->hphr_pa_lock);
	mutex_lock(&mbhc->hphl_pa_lock);
	if (test_and_clear_bit(WCD_MBHC_HPHL_PA_OFF_ACK,
			       &mbhc->hph_pa_dac_state)) {
		pr_debug("%s: HPHL clear flag and enable PA\n", __func__);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HPHL_PA_EN, 1);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HPHL_OCP_DET_EN, 1);
		pa_turned_on = true;
	}
	mutex_unlock(&mbhc->hphl_pa_lock);

	if (pa_turned_on) {
		pr_debug("%s: PA was turned on by MBHC and not by DAPM\n",
			 __func__);
		usleep_range(wg_time * 1000, wg_time * 1000 + 50);
	}
}

static bool wcd_mbhc_is_hph_pa_on(struct wcd_mbhc *mbhc)
{
	bool hph_pa_on = false;

	WCD_MBHC_REG_READ(WCD_MBHC_HPH_PA_EN, hph_pa_on);

	return (hph_pa_on) ? true : false;
}

static void wcd_mbhc_set_and_turnoff_hph_padac(struct wcd_mbhc *mbhc)
{
	u8 wg_time = 0;

	WCD_MBHC_REG_READ(WCD_MBHC_HPH_CNP_WG_TIME, wg_time);
	wg_time += 1;

	/* If headphone PA is on, check if userspace receives
	* removal event to sync-up PA's state */
	if (wcd_mbhc_is_hph_pa_on(mbhc)) {
		pr_debug("%s PA is on, setting PA_OFF_ACK\n", __func__);
		set_bit(WCD_MBHC_HPHL_PA_OFF_ACK, &mbhc->hph_pa_dac_state);
		set_bit(WCD_MBHC_HPHR_PA_OFF_ACK, &mbhc->hph_pa_dac_state);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HPHL_OCP_DET_EN, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HPHR_OCP_DET_EN, 0);
	} else {
		pr_debug("%s PA is off\n", __func__);
	}
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HPH_PA_EN, 0);
	usleep_range(wg_time * 1000, wg_time * 1000 + 50);
}

int wcd_mbhc_get_impedance(struct wcd_mbhc *mbhc, uint32_t *zl,
			uint32_t *zr)
{
	*zl = mbhc->zl;
	*zr = mbhc->zr;

	if (*zl && *zr)
		return 0;
	else
		return -EINVAL;
}

static void wcd_mbhc_hs_elec_irq(struct wcd_mbhc *mbhc, int irq_type,
				 bool enable)
{
	int irq;

	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);

	if (irq_type == WCD_MBHC_ELEC_HS_INS)
		irq = mbhc->intr_ids->mbhc_hs_ins_intr;
	else if (irq_type == WCD_MBHC_ELEC_HS_REM)
		irq = mbhc->intr_ids->mbhc_hs_rem_intr;
	else {
		pr_debug("%s: irq_type: %d, enable: %d\n",
			__func__, irq_type, enable);
		return;
	}

	pr_debug("%s: irq: %d, enable: %d, intr_status:%lu\n",
		 __func__, irq, enable, mbhc->intr_status);
	if ((test_bit(irq_type, &mbhc->intr_status)) != enable) {
		mbhc->mbhc_cb->irq_control(mbhc->codec, irq, enable);
		if (enable)
			set_bit(irq_type, &mbhc->intr_status);
		else
			clear_bit(irq_type, &mbhc->intr_status);
	}
}

static void wcd_mbhc_report_plug(struct wcd_mbhc *mbhc, int insertion,
				enum snd_jack_types jack_type)
{
	struct snd_soc_codec *codec = mbhc->codec;
	bool is_pa_on = false;

	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);

	pr_debug("%s: enter insertion %d hph_status %x\n",
		 __func__, insertion, mbhc->hph_status);
	if (!insertion) {
		/* Report removal */
		mbhc->hph_status &= ~jack_type;
		/*
		 * cancel possibly scheduled btn work and
		 * report release if we reported button press
		 */
		if (wcd_cancel_btn_work(mbhc)) {
			pr_debug("%s: button press is canceled\n", __func__);
		} else if (mbhc->buttons_pressed) {
			pr_debug("%s: release of button press%d\n",
				 __func__, jack_type);
			wcd_mbhc_jack_report(mbhc, &mbhc->button_jack, 0,
					    mbhc->buttons_pressed);
			mbhc->buttons_pressed &=
				~WCD_MBHC_JACK_BUTTON_MASK;
		}

		if (mbhc->micbias_enable) {
			if (mbhc->mbhc_cb->mbhc_micbias_control)
				mbhc->mbhc_cb->mbhc_micbias_control(
						codec, MIC_BIAS_2,
						MICB_DISABLE);
			if (mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic)
				mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(
						codec,
						MIC_BIAS_2, false);
			if (mbhc->mbhc_cb->set_micbias_value) {
				mbhc->mbhc_cb->set_micbias_value(codec);
				WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MICB_CTRL, 0);
			}
			mbhc->micbias_enable = false;
		}

		mbhc->hph_type = WCD_MBHC_HPH_NONE;
		mbhc->zl = mbhc->zr = 0;
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Reporting removal %d(%x)\n", __func__,
			 jack_type, mbhc->hph_status);
#else
		pr_info("%s: Reporting removal %d(%x)\n", __func__,
			 jack_type, mbhc->hph_status);
#endif
/* HTC_AUD_END */
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				mbhc->hph_status, WCD_MBHC_JACK_MASK);
		wcd_mbhc_set_and_turnoff_hph_padac(mbhc);
		hphrocp_off_report(mbhc, SND_JACK_OC_HPHR);
		hphlocp_off_report(mbhc, SND_JACK_OC_HPHL);
		mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
	} else {
		/*
		 * Report removal of current jack type.
		 * Headphone to headset shouldn't report headphone
		 * removal.
		 */
		if (mbhc->mbhc_cfg->detect_extn_cable &&
		    (mbhc->current_plug == MBHC_PLUG_TYPE_HIGH_HPH ||
		    jack_type == SND_JACK_LINEOUT) &&
		    (mbhc->hph_status && mbhc->hph_status != jack_type)) {

			if (mbhc->micbias_enable) {
				if (mbhc->mbhc_cb->mbhc_micbias_control)
					mbhc->mbhc_cb->mbhc_micbias_control(
						codec, MIC_BIAS_2,
						MICB_DISABLE);
				if (mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic)
					mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(
						codec,
						MIC_BIAS_2, false);
				if (mbhc->mbhc_cb->set_micbias_value) {
					mbhc->mbhc_cb->set_micbias_value(
							codec);
					WCD_MBHC_REG_UPDATE_BITS(
							WCD_MBHC_MICB_CTRL, 0);
				}
				mbhc->micbias_enable = false;
			}
			mbhc->hph_type = WCD_MBHC_HPH_NONE;
			mbhc->zl = mbhc->zr = 0;
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: Reporting removal (%x)\n",
				 __func__, mbhc->hph_status);
#else
			pr_info("%s: Reporting removal (%x)\n",
				 __func__, mbhc->hph_status);
#endif
/* HTC_AUD_END */
			wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
					    0, WCD_MBHC_JACK_MASK);

			if (mbhc->hph_status == SND_JACK_LINEOUT) {

				pr_debug("%s: Enable micbias\n", __func__);
				/* Disable current source and enable micbias */
				wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
/* HTC_AUD_START - debug purpose */
#if 0
				pr_debug("%s: set up elec removal detection\n",
					  __func__);
#else
				pr_info("%s: set up elec removal detection\n",
					  __func__);
#endif
/* HTC_AUD_END */
				WCD_MBHC_REG_UPDATE_BITS(
						WCD_MBHC_ELECT_DETECTION_TYPE,
						0);
				usleep_range(200, 210);
				wcd_mbhc_hs_elec_irq(mbhc,
						     WCD_MBHC_ELEC_HS_REM,
						     true);
			}
			mbhc->hph_status &= ~(SND_JACK_HEADSET |
						SND_JACK_LINEOUT |
						SND_JACK_ANC_HEADPHONE |
						SND_JACK_UNSUPPORTED);
		}

		if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET &&
			jack_type == SND_JACK_HEADPHONE)
			mbhc->hph_status &= ~SND_JACK_HEADSET;

		/* Report insertion */
		if (jack_type == SND_JACK_HEADPHONE)
			mbhc->current_plug = MBHC_PLUG_TYPE_HEADPHONE;
		else if (jack_type == SND_JACK_UNSUPPORTED)
			mbhc->current_plug = MBHC_PLUG_TYPE_GND_MIC_SWAP;
		else if (jack_type == SND_JACK_HEADSET) {
			mbhc->current_plug = MBHC_PLUG_TYPE_HEADSET;
			mbhc->jiffies_atreport = jiffies;
		} else if (jack_type == SND_JACK_LINEOUT) {
			mbhc->current_plug = MBHC_PLUG_TYPE_HIGH_HPH;
		} else if (jack_type == SND_JACK_ANC_HEADPHONE)
			mbhc->current_plug = MBHC_PLUG_TYPE_ANC_HEADPHONE;

		if (mbhc->mbhc_cb->hph_pa_on_status)
			is_pa_on = mbhc->mbhc_cb->hph_pa_on_status(codec);

		if (mbhc->impedance_detect &&
			mbhc->mbhc_cb->compute_impedance &&
			(mbhc->mbhc_cfg->linein_th != 0) &&
			(!is_pa_on)) {
				mbhc->mbhc_cb->compute_impedance(mbhc,
						&mbhc->zl, &mbhc->zr);
			if ((mbhc->zl > mbhc->mbhc_cfg->linein_th &&
				mbhc->zl < MAX_IMPED) &&
				(mbhc->zr > mbhc->mbhc_cfg->linein_th &&
				 mbhc->zr < MAX_IMPED) &&
				(jack_type == SND_JACK_HEADPHONE)) {
				jack_type = SND_JACK_LINEOUT;
				mbhc->current_plug = MBHC_PLUG_TYPE_HIGH_HPH;
				if (mbhc->hph_status) {
					mbhc->hph_status &= ~(SND_JACK_HEADSET |
							SND_JACK_LINEOUT |
							SND_JACK_UNSUPPORTED);
					wcd_mbhc_jack_report(mbhc,
							&mbhc->headset_jack,
							mbhc->hph_status,
							WCD_MBHC_JACK_MASK);
				}
/* HTC_AUD_START - debug purpose */
#if 0
				pr_debug("%s: Marking jack type as SND_JACK_LINEOUT\n",
				__func__);
#else
				pr_info("%s: Marking jack type as SND_JACK_LINEOUT\n",
				__func__);
#endif
/* HTC_AUD_END */
			}
			pr_info("%s: mbhc->zl %d, mbhc->zr %d\n", __func__, mbhc->zl, mbhc->zr); /* HTC_AUD - debug purpose */
		}

		mbhc->hph_status |= jack_type;

/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Reporting insertion %d(%x)\n", __func__,
			 jack_type, mbhc->hph_status);
#else
		pr_info("%s: Reporting insertion %d(%x)\n", __func__,
			 jack_type, mbhc->hph_status);
#endif
/* HTC_AUD_END */
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				    (mbhc->hph_status | SND_JACK_MECHANICAL),
				    WCD_MBHC_JACK_MASK);
		wcd_mbhc_clr_and_turnon_hph_padac(mbhc);
	}
	pr_debug("%s: leave hph_status %x\n", __func__, mbhc->hph_status);
}

static bool wcd_mbhc_detect_anc_plug_type(struct wcd_mbhc *mbhc)
{
	bool anc_mic_found = false;
	u16 val, hs_comp_res, btn_status = 0;
	unsigned long retry = 0;
	int valid_plug_cnt = 0, invalid_plug_cnt = 0;
	int btn_status_cnt = 0;
	bool is_check_btn_press = false;


	if (mbhc->mbhc_cfg->anc_micbias < MIC_BIAS_1 ||
	    mbhc->mbhc_cfg->anc_micbias > MIC_BIAS_4)
		return false;

	if (!mbhc->mbhc_cb->mbhc_micbias_control)
		return false;

	WCD_MBHC_REG_READ(WCD_MBHC_FSM_EN, val);

	if (val)
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);

	mbhc->mbhc_cb->mbhc_micbias_control(mbhc->codec,
					    mbhc->mbhc_cfg->anc_micbias,
					    MICB_ENABLE);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MUX_CTL, 0x2);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ANC_DET_EN, 1);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);
	/*
	 * wait for button debounce time 20ms. If 4-pole plug is inserted
	 * into 5-pole jack, then there will be a button press interrupt
	 * during anc plug detection. In that case though Hs_comp_res is 0,
	 * it should not be declared as ANC plug type
	 */
	usleep_range(20000, 20100);

	/*
	 * After enabling FSM, to handle slow insertion scenarios,
	 * check hs_comp_result for few times to see if the IN3 voltage
	 * is below the Vref
	 */
	do {
		if (wcd_swch_level_remove(mbhc)) {
			pr_debug("%s: Switch level is low\n", __func__);
			goto exit;
		}
		pr_debug("%s: Retry attempt %lu\n", __func__, retry + 1);
		WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_res);

		if (!hs_comp_res) {
			valid_plug_cnt++;
			is_check_btn_press = true;
		} else
			invalid_plug_cnt++;
		/* Wait 1ms before taking another reading */
		usleep_range(1000, 1100);

		WCD_MBHC_REG_READ(WCD_MBHC_FSM_STATUS, btn_status);
		if (btn_status)
			btn_status_cnt++;

		retry++;
	} while (retry < ANC_DETECT_RETRY_CNT);

	pr_debug("%s: valid: %d, invalid: %d, btn_status_cnt: %d\n",
		 __func__, valid_plug_cnt, invalid_plug_cnt, btn_status_cnt);

	/* decision logic */
	if ((valid_plug_cnt > invalid_plug_cnt) && is_check_btn_press &&
	    (btn_status_cnt == 0))
		anc_mic_found = true;
exit:
	if (!val)
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);

	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ANC_DET_EN, 0);

	mbhc->mbhc_cb->mbhc_micbias_control(mbhc->codec,
					    mbhc->mbhc_cfg->anc_micbias,
					    MICB_DISABLE);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MUX_CTL, 0x0);
	pr_debug("%s: anc mic %sfound\n", __func__,
		 anc_mic_found ? "" : "not ");
	return anc_mic_found;
}

static void wcd_mbhc_find_plug_and_report(struct wcd_mbhc *mbhc,
					 enum wcd_mbhc_plug_type plug_type)
{
	bool anc_mic_found = false;
	enum snd_jack_types jack_type;

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: enter current_plug(%d) new_plug(%d)\n",
		 __func__, mbhc->current_plug, plug_type);
#else
	pr_info("%s: enter current_plug(%d) new_plug(%d)\n",
		 __func__, mbhc->current_plug, plug_type);
#endif
/* HTC_AUD_END */

	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);

	if (mbhc->current_plug == plug_type) {
		pr_debug("%s: cable already reported, exit\n", __func__);
		goto exit;
	}

	if (plug_type == MBHC_PLUG_TYPE_HEADPHONE) {
		/*
		 * Nothing was reported previously
		 * report a headphone or unsupported
		 */
		wcd_mbhc_report_plug(mbhc, 1, SND_JACK_HEADPHONE);
	} else if (plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP) {
			if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADPHONE)
				wcd_mbhc_report_plug(mbhc, 0,
						SND_JACK_HEADPHONE);
			if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET)
				wcd_mbhc_report_plug(mbhc, 0, SND_JACK_HEADSET);
		wcd_mbhc_report_plug(mbhc, 1, SND_JACK_UNSUPPORTED);
	} else if (plug_type == MBHC_PLUG_TYPE_HEADSET) {
		if (mbhc->mbhc_cfg->enable_anc_mic_detect)
			anc_mic_found = wcd_mbhc_detect_anc_plug_type(mbhc);

		jack_type = SND_JACK_HEADSET;
		if (anc_mic_found)
			jack_type = SND_JACK_ANC_HEADPHONE;

		/*
		 * If Headphone was reported previously, this will
		 * only report the mic line
		 */
		wcd_mbhc_report_plug(mbhc, 1, jack_type);
	} else if (plug_type == MBHC_PLUG_TYPE_HIGH_HPH) {
		if (mbhc->mbhc_cfg->detect_extn_cable) {
			/* High impedance device found. Report as LINEOUT */
			wcd_mbhc_report_plug(mbhc, 1, SND_JACK_LINEOUT);
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: setup mic trigger for further detection\n",
				 __func__);
#else
			pr_info("%s: setup mic trigger for further detection\n",
				 __func__);
#endif
/* HTC_AUD_END */

			/* Disable HW FSM and current source */
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 0);
			/* Setup for insertion detection */
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_DETECTION_TYPE,
						 1);
			/*
			 * Enable HPHL trigger and MIC Schmitt triggers
			 * and request for elec insertion interrupts
			 */
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC,
						 3);
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_INS,
					     true);
		} else {
			wcd_mbhc_report_plug(mbhc, 1, SND_JACK_LINEOUT);
		}
	} else {
		WARN(1, "Unexpected current plug_type %d, plug_type %d\n",
		     mbhc->current_plug, plug_type);
	}
exit:
	pr_debug("%s: leave\n", __func__);
}

/* To determine if cross connection occured */
static int wcd_check_cross_conn(struct wcd_mbhc *mbhc)
{
	u16 swap_res = 0;
	enum wcd_mbhc_plug_type plug_type = MBHC_PLUG_TYPE_NONE;
	s16 reg1 = 0;
	bool hphl_sch_res = 0, hphr_sch_res = 0;

	if (wcd_swch_level_remove(mbhc)) {
		pr_debug("%s: Switch level is low\n", __func__);
		return -EINVAL;
	}

	/* If PA is enabled, dont check for cross-connection */
	if (mbhc->mbhc_cb->hph_pa_on_status)
		if (mbhc->mbhc_cb->hph_pa_on_status(mbhc->codec))
			return false;

	WCD_MBHC_REG_READ(WCD_MBHC_ELECT_SCHMT_ISRC, reg1);
	/*
	 * Check if there is any cross connection,
	 * Micbias and schmitt trigger (HPHL-HPHR)
	 * needs to be enabled. For some codecs like wcd9335,
	 * pull-up will already be enabled when this function
	 * is called for cross-connection identification. No
	 * need to enable micbias in that case.
	 */
	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC, 2);

	WCD_MBHC_REG_READ(WCD_MBHC_ELECT_RESULT, swap_res);
	pr_debug("%s: swap_res%x\n", __func__, swap_res);

	/*
	 * Read reg hphl and hphr schmitt result with cross connection
	 * bit. These bits will both be "0" in case of cross connection
	 * otherwise, they stay at 1
	 */
	WCD_MBHC_REG_READ(WCD_MBHC_HPHL_SCHMT_RESULT, hphl_sch_res);
	WCD_MBHC_REG_READ(WCD_MBHC_HPHR_SCHMT_RESULT, hphr_sch_res);
	if (!(hphl_sch_res || hphr_sch_res)) {
		plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
		pr_debug("%s: Cross connection identified\n", __func__);
	} else {
		pr_debug("%s: No Cross connection found\n", __func__);
	}

	/* Disable schmitt trigger and restore micbias */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC, reg1);
/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: leave, plug type: %d\n", __func__,  plug_type);
#else
	pr_info("%s: leave, plug type: %d\n", __func__,  plug_type);
#endif
/* HTC_AUD_END */

	return (plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP) ? true : false;
}

/* HTC_AUD_START - disable it to reduce cable detection time */
#if 0
static bool wcd_is_special_headset(struct wcd_mbhc *mbhc)
{
	struct snd_soc_codec *codec = mbhc->codec;
/* HTC_AUD_START - fix klockwork */
#if 0
	int delay = 0, rc;
#else
	int delay = 0, rc = 0;
#endif
/* HTC_AUD_END */
	bool ret = false;
	u16 hs_comp_res;
	bool is_spl_hs = false;

	/*
	 * Increase micbias to 2.7V to detect headsets with
	 * threshold on microphone
	 */
	if (mbhc->mbhc_cb->mbhc_micbias_control &&
	    !mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic) {
		pr_debug("%s: callback fn micb_ctrl_thr_mic not defined\n",
			 __func__);
		return false;
	} else if (mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic) {
		rc = mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(codec,
							MIC_BIAS_2, true);
		if (rc) {
			pr_err("%s: Micbias control for thr mic failed, rc: %d\n",
				__func__, rc);
			return false;
		}
	}

	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: special headset, start register writes\n", __func__);
#else
	pr_info("%s: special headset, start register writes\n", __func__);
#endif
/* HTC_AUD_END */

	WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_res);
	while (!is_spl_hs)  {
		if (mbhc->hs_detect_work_stop) {
			pr_debug("%s: stop requested: %d\n", __func__,
					mbhc->hs_detect_work_stop);
			break;
		}
		delay = delay + 50;
		if (mbhc->mbhc_cb->mbhc_common_micb_ctrl) {
			mbhc->mbhc_cb->mbhc_common_micb_ctrl(codec,
					MBHC_COMMON_MICB_PRECHARGE,
					true);
			mbhc->mbhc_cb->mbhc_common_micb_ctrl(codec,
					MBHC_COMMON_MICB_SET_VAL,
					true);
		}
		/* Wait for 50msec for MICBIAS to settle down */
		msleep(50);
		if (mbhc->mbhc_cb->set_auto_zeroing)
			mbhc->mbhc_cb->set_auto_zeroing(codec, true);
		/* Wait for 50msec for FSM to update result values */
		msleep(50);
		WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_res);
		if (!(hs_comp_res)) {
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: Special headset detected in %d msecs\n",
					__func__, (delay * 2));
#else
			pr_info("%s: Special headset detected in %d msecs\n",
					__func__, (delay * 2));
#endif
/* HTC_AUD_END */
			is_spl_hs = true;
		}
		if (delay == SPECIAL_HS_DETECT_TIME_MS) {
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: Spl headset didnt get detect in 4 sec\n",
					__func__);
#else
			pr_info("%s: Spl headset didnt get detect in 4 sec\n",
					__func__);
#endif
/* HTC_AUD_END */
			break;
		}
	}
	if (is_spl_hs) {
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Headset with threshold found\n",  __func__);
#else
		pr_info("%s: Headset with threshold found\n",  __func__);
#endif
/* HTC_AUD_END */
		mbhc->micbias_enable = true;
		ret = true;
	}
	if (mbhc->mbhc_cb->mbhc_common_micb_ctrl)
		mbhc->mbhc_cb->mbhc_common_micb_ctrl(codec,
				MBHC_COMMON_MICB_PRECHARGE,
				false);
	if (mbhc->mbhc_cb->set_micbias_value && !mbhc->micbias_enable)
		mbhc->mbhc_cb->set_micbias_value(codec);
	if (mbhc->mbhc_cb->set_auto_zeroing)
		mbhc->mbhc_cb->set_auto_zeroing(codec, false);

	if (mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic &&
	    !mbhc->micbias_enable)
		mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(codec, MIC_BIAS_2,
						      false);

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: leave, micb_enable: %d\n", __func__,
		  mbhc->micbias_enable);
#else
	pr_info("%s: leave, micb_enable: %d\n", __func__,
		  mbhc->micbias_enable);
#endif
/* HTC_AUD_END */
	return ret;
}
#endif
/* HTC_AUD_END */

static void wcd_mbhc_update_fsm_source(struct wcd_mbhc *mbhc,
				       enum wcd_mbhc_plug_type plug_type)
{
	bool micbias2;

	micbias2 = mbhc->mbhc_cb->micbias_enable_status(mbhc,
							MIC_BIAS_2);
	switch (plug_type) {
	case MBHC_PLUG_TYPE_HEADPHONE:
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 3);
		break;
	case MBHC_PLUG_TYPE_HEADSET:
	case MBHC_PLUG_TYPE_ANC_HEADPHONE:
	case MBHC_PLUG_TYPE_AS_HEADSET: /* HTC_AUD - AS HS */
		if (!mbhc->is_hs_recording && !micbias2)
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 3);
		break;
	default:
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 0);
		break;

	};
}

static void wcd_enable_mbhc_supply(struct wcd_mbhc *mbhc,
			enum wcd_mbhc_plug_type plug_type)
{

	struct snd_soc_codec *codec = mbhc->codec;

	/*
	 * Do not disable micbias if recording is going on or
	 * headset is inserted on the other side of the extn
	 * cable. If headset has been detected current source
	 * needs to be kept enabled for button detection to work.
	 * If the accessory type is invalid or unsupported, we
	 * dont need to enable either of them.
	 */
	if (det_extn_cable_en && mbhc->is_extn_cable &&
		mbhc->mbhc_cb && mbhc->mbhc_cb->extn_use_mb &&
		mbhc->mbhc_cb->extn_use_mb(codec)) {
		if (plug_type == MBHC_PLUG_TYPE_HEADPHONE ||
		    plug_type == MBHC_PLUG_TYPE_HEADSET)
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);
	} else {
		if (plug_type == MBHC_PLUG_TYPE_HEADSET) {
			if (mbhc->is_hs_recording || mbhc->micbias_enable)
				wcd_enable_curr_micbias(mbhc,
							WCD_MBHC_EN_MB);
			else if ((test_bit(WCD_MBHC_EVENT_PA_HPHL,
				&mbhc->event_state)) ||
				(test_bit(WCD_MBHC_EVENT_PA_HPHR,
				&mbhc->event_state)))
					wcd_enable_curr_micbias(mbhc,
							WCD_MBHC_EN_PULLUP);
			else
				wcd_enable_curr_micbias(mbhc,
							WCD_MBHC_EN_CS);
		} else if (plug_type == MBHC_PLUG_TYPE_HEADPHONE) {
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_CS);
		} else {
			wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_NONE);
		}
	}
}

static bool wcd_mbhc_check_for_spl_headset(struct wcd_mbhc *mbhc,
					   int *spl_hs_cnt)
{
	u16 hs_comp_res_1_8v = 0, hs_comp_res_2_7v = 0;
	bool spl_hs = false;

	pr_info("%s: enter\n", __func__); //HTC_AUD

	if (!mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic)
		goto exit;

	/* Read back hs_comp_res @ 1.8v Micbias */
	WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_res_1_8v);
	if (!hs_comp_res_1_8v) {
		spl_hs = false;
		goto exit;
	}

	/* Bump up MB2 to 2.7v */
	mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(mbhc->codec,
				mbhc->mbhc_cfg->mbhc_micbias, true);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);
	usleep_range(10000, 10100);

	/* Read back HS_COMP_RESULT */
	WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_res_2_7v);
	if (!hs_comp_res_2_7v && hs_comp_res_1_8v)
		spl_hs = true;

	if (spl_hs && spl_hs_cnt)
		*spl_hs_cnt += 1;

	/* MB2 back to 1.8v */
	if (*spl_hs_cnt != WCD_MBHC_SPL_HS_CNT) {
		mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(mbhc->codec,
				mbhc->mbhc_cfg->mbhc_micbias, false);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);
		usleep_range(10000, 10100);
	}

	if (spl_hs)
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Detected special HS (%d)\n", __func__, spl_hs);
#else
		pr_info("%s: Detected special HS (%d)\n", __func__, spl_hs);
	else
		pr_info("%s: Not Detected special HS (%d)\n", __func__, spl_hs);
#endif
/* HTC_AUD_END */

exit:
	return spl_hs;
}

static void wcd_correct_swch_plug(struct work_struct *work)
{
	struct wcd_mbhc *mbhc;
	struct snd_soc_codec *codec;
	enum wcd_mbhc_plug_type plug_type = MBHC_PLUG_TYPE_INVALID;
	unsigned long timeout;
	u16 hs_comp_res = 0, hphl_sch = 0, mic_sch = 0, btn_result = 0;
	bool wrk_complete = false;
	int pt_gnd_mic_swap_cnt = 0;
	int no_gnd_mic_swap_cnt = 0;
	bool is_pa_on = false, spl_hs = false;
	bool micbias2 = false;
	bool micbias1 = false;
	int ret = 0;
	int rc, spl_hs_count = 0;
	int cross_conn;
	int try = 0;

	pr_debug("%s: enter\n", __func__);

	mbhc = container_of(work, struct wcd_mbhc, correct_plug_swch);
	codec = mbhc->codec;

	/*
	 * Enable micbias/pullup for detection in correct work.
	 * This work will get scheduled from detect_plug_type which
	 * will already request for pullup/micbias. If the pullup/micbias
	 * is handled with ref-counts by individual codec drivers, there is
	 * no need to enabale micbias/pullup here
	 */

	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);


	/* Enable HW FSM */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);
	/*
	 * Check for any button press interrupts before starting 3-sec
	 * loop.
	 */
	rc = wait_for_completion_timeout(&mbhc->btn_press_compl,
			msecs_to_jiffies(WCD_MBHC_BTN_PRESS_COMPL_TIMEOUT_MS));

	WCD_MBHC_REG_READ(WCD_MBHC_BTN_RESULT, btn_result);
	WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_res);

	if (!rc) {
		pr_debug("%s No btn press interrupt\n", __func__);
		if (!btn_result && !hs_comp_res)
			plug_type = MBHC_PLUG_TYPE_HEADSET;
		else if (!btn_result && hs_comp_res)
			plug_type = MBHC_PLUG_TYPE_HIGH_HPH;
		else
			plug_type = MBHC_PLUG_TYPE_INVALID;
	} else {
		if (!btn_result && !hs_comp_res)
			plug_type = MBHC_PLUG_TYPE_HEADPHONE;
		else
			plug_type = MBHC_PLUG_TYPE_INVALID;
	}

/* HTC_AUD_START - disable swap detection */
#if 0
	do {
		cross_conn = wcd_check_cross_conn(mbhc);
		try++;
	} while (try < GND_MIC_SWAP_THRESHOLD);
	/*
	 * check for cross coneection 4 times.
	 * conisder the result of the fourth iteration.
	 */
	if (cross_conn > 0) {
		pr_debug("%s: cross con found, start polling\n",
			 __func__);
		plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
		pr_debug("%s: Plug found, plug type is %d\n",
			 __func__, plug_type);
		goto correct_plug_type;
	}
#else
	if (mbhc->swap_detect) {
		do {
			cross_conn = wcd_check_cross_conn(mbhc);
			try++;
		} while (try < GND_MIC_SWAP_THRESHOLD);
		/*
		 * check for cross coneection 4 times.
		 * conisder the result of the fourth iteration.
		 */
		if (cross_conn > 0) {
			pr_debug("%s: cross con found, start polling\n",
				 __func__);
			plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
			pr_debug("%s: Plug found, plug type is %d\n",
				 __func__, plug_type);
			goto correct_plug_type;
		}
	} else {
		pr_info("%s: Valid plug found, plug type is %d\n",
		 __func__, plug_type);
	}
#endif
/* HTC_AUD_END */

	if ((plug_type == MBHC_PLUG_TYPE_HEADSET ||
	     plug_type == MBHC_PLUG_TYPE_HEADPHONE) &&
	    (!wcd_swch_level_remove(mbhc))) {
		WCD_MBHC_RSC_LOCK(mbhc);
		wcd_mbhc_find_plug_and_report(mbhc, plug_type);
		WCD_MBHC_RSC_UNLOCK(mbhc);
	}

correct_plug_type:

	timeout = jiffies + msecs_to_jiffies(HS_DETECT_PLUG_TIME_MS);
	while (!time_after(jiffies, timeout)) {
		if (mbhc->hs_detect_work_stop) {
			pr_debug("%s: stop requested: %d\n", __func__,
					mbhc->hs_detect_work_stop);
			wcd_enable_curr_micbias(mbhc,
						WCD_MBHC_EN_NONE);
			if (mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic &&
				mbhc->micbias_enable) {
				mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(
					mbhc->codec, MIC_BIAS_2, false);
				if (mbhc->mbhc_cb->set_micbias_value)
					mbhc->mbhc_cb->set_micbias_value(
							mbhc->codec);
				mbhc->micbias_enable = false;
			}
			goto exit;
		}
		if (mbhc->btn_press_intr) {
			wcd_cancel_btn_work(mbhc);
			mbhc->btn_press_intr = false;
		}
		/* Toggle FSM */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);

		/* allow sometime and re-check stop requested again */
		msleep(20);
		if (mbhc->hs_detect_work_stop) {
			pr_debug("%s: stop requested: %d\n", __func__,
					mbhc->hs_detect_work_stop);
			wcd_enable_curr_micbias(mbhc,
						WCD_MBHC_EN_NONE);
			if (mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic &&
				mbhc->micbias_enable) {
				mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(
					mbhc->codec, MIC_BIAS_2, false);
				if (mbhc->mbhc_cb->set_micbias_value)
					mbhc->mbhc_cb->set_micbias_value(
							mbhc->codec);
				mbhc->micbias_enable = false;
			}
			goto exit;
		}
		WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_res);

/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: hs_comp_res: %x\n", __func__, hs_comp_res);
#else
		pr_info("%s: hs_comp_res: %x\n", __func__, hs_comp_res);
#endif
/* HTC_AUD_END */
		if (mbhc->mbhc_cb->hph_pa_on_status)
			is_pa_on = mbhc->mbhc_cb->hph_pa_on_status(codec);

		/*
		 * instead of hogging system by contineous polling, wait for
		 * sometime and re-check stop request again.
		 */
		msleep(180);
		if (hs_comp_res && (spl_hs_count < WCD_MBHC_SPL_HS_CNT)) {
			spl_hs = wcd_mbhc_check_for_spl_headset(mbhc,
								&spl_hs_count);

			if (spl_hs_count == WCD_MBHC_SPL_HS_CNT) {
				hs_comp_res = 0;
				spl_hs = true;
				mbhc->micbias_enable = true;
			}
		}

/* HTC_AUD_START - disable swap detection */
#if 0
		if ((!hs_comp_res) && (!is_pa_on)) {
			/* Check for cross connection*/
			ret = wcd_check_cross_conn(mbhc);
			if (ret < 0) {
				continue;
			} else if (ret > 0) {
				pt_gnd_mic_swap_cnt++;
				no_gnd_mic_swap_cnt = 0;
				if (pt_gnd_mic_swap_cnt <
						GND_MIC_SWAP_THRESHOLD) {
					continue;
				} else if (pt_gnd_mic_swap_cnt >
						GND_MIC_SWAP_THRESHOLD) {
					/*
					 * This is due to GND/MIC switch didn't
					 * work,  Report unsupported plug.
					 */
					pr_debug("%s: switch didnt work\n",
						  __func__);
					plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
					goto report;
				} else {
					plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
				}
			} else {
				no_gnd_mic_swap_cnt++;
				pt_gnd_mic_swap_cnt = 0;
				plug_type = MBHC_PLUG_TYPE_HEADSET;
				if ((no_gnd_mic_swap_cnt <
				    GND_MIC_SWAP_THRESHOLD) &&
				    (spl_hs_count != WCD_MBHC_SPL_HS_CNT)) {
					continue;
				} else {
					no_gnd_mic_swap_cnt = 0;
				}
			}
			if ((pt_gnd_mic_swap_cnt == GND_MIC_SWAP_THRESHOLD) &&
				(plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP)) {
				/*
				 * if switch is toggled, check again,
				 * otherwise report unsupported plug
				 */
				if (mbhc->mbhc_cfg->swap_gnd_mic &&
					mbhc->mbhc_cfg->swap_gnd_mic(codec)) {
					pr_debug("%s: US_EU gpio present,flip switch\n"
						, __func__);
					continue;
				}
			}
		}
#else
		if (mbhc->swap_detect) {
			if ((!hs_comp_res) && (!is_pa_on)) {
				/* Check for cross connection*/
				ret = wcd_check_cross_conn(mbhc);
				if (ret < 0) {
					continue;
				} else if (ret > 0) {
					pt_gnd_mic_swap_cnt++;
					no_gnd_mic_swap_cnt = 0;
					if (pt_gnd_mic_swap_cnt <
							GND_MIC_SWAP_THRESHOLD) {
						continue;
					} else if (pt_gnd_mic_swap_cnt >
							GND_MIC_SWAP_THRESHOLD) {
						/*
						 * This is due to GND/MIC switch didn't
						 * work,  Report unsupported plug.
						 */
						pr_debug("%s: switch didnt work\n",
							  __func__);
						plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
						goto report;
					} else {
						plug_type = MBHC_PLUG_TYPE_GND_MIC_SWAP;
					}
				} else {
					no_gnd_mic_swap_cnt++;
					pt_gnd_mic_swap_cnt = 0;
					plug_type = MBHC_PLUG_TYPE_HEADSET;
					if ((no_gnd_mic_swap_cnt <
					    GND_MIC_SWAP_THRESHOLD) &&
					    (spl_hs_count != WCD_MBHC_SPL_HS_CNT)) {
						continue;
					} else {
						no_gnd_mic_swap_cnt = 0;
					}
				}
				if ((pt_gnd_mic_swap_cnt == GND_MIC_SWAP_THRESHOLD) &&
					(plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP)) {
					/*
					 * if switch is toggled, check again,
					 * otherwise report unsupported plug
					 */
					if (mbhc->mbhc_cfg->swap_gnd_mic &&
						mbhc->mbhc_cfg->swap_gnd_mic(codec)) {
						pr_debug("%s: US_EU gpio present,flip switch\n"
							, __func__);
						continue;
					}
				}
			}
		} else {
			plug_type = MBHC_PLUG_TYPE_HEADSET;
		}
#endif
/* HTC_AUD_END */

		WCD_MBHC_REG_READ(WCD_MBHC_HPHL_SCHMT_RESULT, hphl_sch);
		WCD_MBHC_REG_READ(WCD_MBHC_MIC_SCHMT_RESULT, mic_sch);
		if (hs_comp_res && !(hphl_sch || mic_sch)) {
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: cable is extension cable\n", __func__);
#else
			pr_info("%s: cable is extension cable\n", __func__);
#endif
/* HTC_AUD_END */
			plug_type = MBHC_PLUG_TYPE_HIGH_HPH;
			wrk_complete = true;
/* HTC_AUD_START */
/*
[issue scenario]
1) detect special hs on wcd_mbhc_check_for_spl_headset() and set micbias_enable = true
2) detect plug_type as MBHC_PLUG_TYPE_HIGH_HPH after reading register
3) detect Not special hs on wcd_is_special_headset()
It will diable micbias on wcd_mbhc_report_plug() and wcd_correct_swch_plug().
Make Tx failed and Rx has noise. Only recovery can recovery.
Set micbias_enable to faled if type is MBHC_PLUG_TYPE_HIGH_HPH to fix issue.
*/
			mbhc->micbias_enable = false;
/* HTC_AUD_END */
		} else {
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: cable might be headset: %d\n", __func__,
					plug_type);
#else
			pr_info("%s: cable might be headset: %d\n", __func__,
					plug_type);
#endif
/* HTC_AUD_END */
			if (!(plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP)) {
				plug_type = MBHC_PLUG_TYPE_HEADSET;
				/*
				 * Report headset only if not already reported
				 * and if there is not button press without
				 * release
				 */
				if (((mbhc->current_plug !=
				      MBHC_PLUG_TYPE_HEADSET) &&
				     (mbhc->current_plug !=
				      MBHC_PLUG_TYPE_ANC_HEADPHONE)) &&
				    !wcd_swch_level_remove(mbhc) &&
				    !mbhc->btn_press_intr) {
/* HTC_AUD_START - debug purpose */
#if 0
					pr_debug("%s: cable is %sheadset\n",
						__func__,
						((spl_hs_count ==
							WCD_MBHC_SPL_HS_CNT) ?
							"special ":""));
#else
					pr_info("%s: cable is %sheadset\n",
						__func__,
						((spl_hs_count ==
							WCD_MBHC_SPL_HS_CNT) ?
							"special ":""));
#endif
/* HTC_AUD_END */
					goto report;
				}
			}
			wrk_complete = false;
		}
	}
	if (!wrk_complete && mbhc->btn_press_intr) {
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Can be slow insertion of headphone\n", __func__);
#else
		pr_info("%s: Can be slow insertion of headphone\n", __func__);
#endif
/* HTC_AUD_END */
		wcd_cancel_btn_work(mbhc);
		plug_type = MBHC_PLUG_TYPE_HEADPHONE;
	}
	/*
	 * If plug_tye is headset, we might have already reported either in
	 * detect_plug-type or in above while loop, no need to report again
	 */
	if (!wrk_complete && ((plug_type == MBHC_PLUG_TYPE_HEADSET) ||
	    (plug_type == MBHC_PLUG_TYPE_ANC_HEADPHONE))) {
		pr_debug("%s: plug_type:0x%x already reported\n",
			 __func__, mbhc->current_plug);
		goto enable_supply;
	}

/* HTC_AUD_START - disable it to reduce cable detection time */
#if 0
	if (plug_type == MBHC_PLUG_TYPE_HIGH_HPH &&
		(!det_extn_cable_en)) {
		if (wcd_is_special_headset(mbhc)) {
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: Special headset found %d\n",
					__func__, plug_type);
#else
			pr_info("%s: Special headset found %d\n",
					__func__, plug_type);
#endif
/* HTC_AUD_END */
			plug_type = MBHC_PLUG_TYPE_HEADSET;
			goto report;
		}
	}
#endif
/* HTC_AUD_END */

report:
	if (wcd_swch_level_remove(mbhc)) {
		pr_debug("%s: Switch level is low\n", __func__);
		goto exit;
	}
	if (plug_type == MBHC_PLUG_TYPE_GND_MIC_SWAP && mbhc->btn_press_intr) {
		pr_debug("%s: insertion of headphone with swap\n", __func__);
		wcd_cancel_btn_work(mbhc);
		plug_type = MBHC_PLUG_TYPE_HEADPHONE;
	}
/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: Valid plug found, plug type %d wrk_cmpt %d btn_intr %d\n",
			__func__, plug_type, wrk_complete,
			mbhc->btn_press_intr);
#else
	pr_info("%s: Valid plug found, plug type %d wrk_cmpt %d btn_intr %d\n",
			__func__, plug_type, wrk_complete,
			mbhc->btn_press_intr);
#endif
/* HTC_AUD_END */
	WCD_MBHC_RSC_LOCK(mbhc);
	wcd_mbhc_find_plug_and_report(mbhc, plug_type);
	WCD_MBHC_RSC_UNLOCK(mbhc);
enable_supply:
	if (mbhc->mbhc_cb->mbhc_micbias_control)
		wcd_mbhc_update_fsm_source(mbhc, plug_type);
	else
		wcd_enable_mbhc_supply(mbhc, plug_type);
exit:
	if (mbhc->mbhc_cb->mbhc_micbias_control &&
	    !mbhc->micbias_enable)
		mbhc->mbhc_cb->mbhc_micbias_control(codec, MIC_BIAS_2,
						    MICB_DISABLE);
	if (mbhc->mbhc_cb->micbias_enable_status) {
		micbias1 = mbhc->mbhc_cb->micbias_enable_status(mbhc,
								MIC_BIAS_1);
		micbias2 = mbhc->mbhc_cb->micbias_enable_status(mbhc,
								MIC_BIAS_2);
	}

	if (mbhc->mbhc_cfg->detect_extn_cable &&
	    ((plug_type == MBHC_PLUG_TYPE_HEADPHONE) ||
	     (plug_type == MBHC_PLUG_TYPE_HEADSET)) &&
	    !mbhc->hs_detect_work_stop) {
		WCD_MBHC_RSC_LOCK(mbhc);
		wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_REM, true);
		WCD_MBHC_RSC_UNLOCK(mbhc);
	}
	if (mbhc->mbhc_cb->set_cap_mode)
		mbhc->mbhc_cb->set_cap_mode(codec, micbias1, micbias2);

	if (mbhc->mbhc_cb->hph_pull_down_ctrl)
		mbhc->mbhc_cb->hph_pull_down_ctrl(codec, true);

	mbhc->mbhc_cb->lock_sleep(mbhc, false);
	pr_debug("%s: leave\n", __func__);
}

/* called under codec_resource_lock acquisition */
static void wcd_mbhc_detect_plug_type(struct wcd_mbhc *mbhc)
{
	struct snd_soc_codec *codec = mbhc->codec;
	bool micbias1 = false;

	pr_debug("%s: enter\n", __func__);
	WCD_MBHC_RSC_ASSERT_LOCKED(mbhc);

	if (mbhc->mbhc_cb->hph_pull_down_ctrl)
		mbhc->mbhc_cb->hph_pull_down_ctrl(codec, false);

	if (mbhc->mbhc_cb->micbias_enable_status)
		micbias1 = mbhc->mbhc_cb->micbias_enable_status(mbhc,
								MIC_BIAS_1);

	if (mbhc->mbhc_cb->set_cap_mode)
		mbhc->mbhc_cb->set_cap_mode(codec, micbias1, true);

	if (mbhc->mbhc_cb->mbhc_micbias_control)
		mbhc->mbhc_cb->mbhc_micbias_control(codec, MIC_BIAS_2,
						    MICB_ENABLE);
	else
		wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_MB);

	/* Re-initialize button press completion object */
	reinit_completion(&mbhc->btn_press_compl);
	wcd_schedule_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);
	pr_debug("%s: leave\n", __func__);
}

static void wcd_mbhc_swch_irq_handler(struct wcd_mbhc *mbhc)
{
	bool detection_type = 0;
	bool micbias1 = false;
	struct snd_soc_codec *codec = mbhc->codec;

	dev_dbg(codec->dev, "%s: enter\n", __func__);

	WCD_MBHC_RSC_LOCK(mbhc);

	mbhc->in_swch_irq_handler = true;

	/* cancel pending button press */
	if (wcd_cancel_btn_work(mbhc))
		pr_debug("%s: button press is canceled\n", __func__);

	WCD_MBHC_REG_READ(WCD_MBHC_MECH_DETECTION_TYPE, detection_type);

	/* Set the detection type appropriately */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MECH_DETECTION_TYPE,
				 !detection_type);

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: mbhc->current_plug: %d detection_type: %d\n", __func__,
			mbhc->current_plug, detection_type);
#else
	pr_info("%s: mbhc->current_plug: %d detection_type: %d\n", __func__,
			mbhc->current_plug, detection_type);
#endif
/* HTC_AUD_END */
	wcd_cancel_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);

	if (mbhc->mbhc_cb->micbias_enable_status)
		micbias1 = mbhc->mbhc_cb->micbias_enable_status(mbhc,
						MIC_BIAS_1);

	if ((mbhc->current_plug == MBHC_PLUG_TYPE_NONE) &&
	    detection_type) {
		/* Make sure MASTER_BIAS_CTL is enabled */
		mbhc->mbhc_cb->mbhc_bias(codec, true);

		if (mbhc->mbhc_cb->mbhc_common_micb_ctrl)
			mbhc->mbhc_cb->mbhc_common_micb_ctrl(codec,
					MBHC_COMMON_MICB_TAIL_CURR, true);

		if (!mbhc->mbhc_cfg->hs_ext_micbias &&
		     mbhc->mbhc_cb->micb_internal)
			/*
			 * Enable Tx2 RBias if the headset
			 * is using internal micbias
			 */
			mbhc->mbhc_cb->micb_internal(codec, 1, true);

		/* Remove micbias pulldown */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_PULLDOWN_CTRL, 0);
		/* Apply trim if needed on the device */
		if (mbhc->mbhc_cb->trim_btn_reg)
			mbhc->mbhc_cb->trim_btn_reg(codec);
		/* Enable external voltage source to micbias if present */
		if (mbhc->mbhc_cb->enable_mb_source)
			mbhc->mbhc_cb->enable_mb_source(mbhc, true);
		mbhc->btn_press_intr = false;
		mbhc->is_btn_press = false;
		wcd_mbhc_detect_plug_type(mbhc);
	} else if ((mbhc->current_plug != MBHC_PLUG_TYPE_NONE)
			&& !detection_type) {
		/* Disable external voltage source to micbias if present */
		if (mbhc->mbhc_cb->enable_mb_source)
			mbhc->mbhc_cb->enable_mb_source(mbhc, false);
		/* Disable HW FSM */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 0);
		if (mbhc->mbhc_cb->mbhc_common_micb_ctrl)
			mbhc->mbhc_cb->mbhc_common_micb_ctrl(codec,
					MBHC_COMMON_MICB_TAIL_CURR, false);

		if (mbhc->mbhc_cb->set_cap_mode)
			mbhc->mbhc_cb->set_cap_mode(codec, micbias1, false);

		mbhc->btn_press_intr = false;
		mbhc->is_btn_press = false;
		if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADPHONE) {
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_REM,
					     false);
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_INS,
					     false);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_DETECTION_TYPE,
						 1);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC, 0);
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_HEADPHONE);
		} else if (mbhc->current_plug == MBHC_PLUG_TYPE_GND_MIC_SWAP) {
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_UNSUPPORTED);
		} else if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET) {
			/* make sure to turn off Rbias */
			if (mbhc->mbhc_cb->micb_internal)
				mbhc->mbhc_cb->micb_internal(codec, 1, false);

			/* Pulldown micbias */
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_PULLDOWN_CTRL, 1);
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_REM,
					     false);
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_INS,
					     false);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_DETECTION_TYPE,
						 1);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC, 0);
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_HEADSET);
		} else if (mbhc->current_plug == MBHC_PLUG_TYPE_HIGH_HPH) {
			mbhc->is_extn_cable = false;
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_REM,
					     false);
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_INS,
					     false);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_DETECTION_TYPE,
						 1);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC, 0);
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_LINEOUT);
		} else if (mbhc->current_plug == MBHC_PLUG_TYPE_ANC_HEADPHONE) {
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_REM, false);
			wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_INS, false);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_DETECTION_TYPE,
						 0);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC, 0);
			wcd_mbhc_report_plug(mbhc, 0, SND_JACK_ANC_HEADPHONE);
		}
	} else if (!detection_type) {
		/* Disable external voltage source to micbias if present */
		if (mbhc->mbhc_cb->enable_mb_source)
			mbhc->mbhc_cb->enable_mb_source(mbhc, false);
		/* Disable HW FSM */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_ISRC_CTL, 0);
	}

	mbhc->in_swch_irq_handler = false;
	WCD_MBHC_RSC_UNLOCK(mbhc);
	pr_debug("%s: leave\n", __func__);
}

/* HTC_AUD_START - AS HS */
static void htc_button_detection(struct wcd_mbhc *mbhc, bool enable)
{
	pr_info("%s: enable %d\n", __func__, enable);
	if(enable) {
		/* Enable HW FSM */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 1);
	} else {
		/* Disable HW FSM */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);
	}

	if (mbhc->mbhc_cb->mbhc_micbias_control) {
		wcd_mbhc_update_fsm_source(mbhc, mbhc->current_plug);
	} else {
		wcd_enable_mbhc_supply(mbhc, mbhc->current_plug);
	}
}

static int htc_headset_irq_handler(struct wcd_mbhc *mbhc){
	struct htc_headset_config hs_cfg = mbhc->mbhc_cfg->htc_headset_cfg;
	int retry = 0, insert = 0, ret = 0;

	WCD_MBHC_RSC_LOCK(mbhc);

	/* cancel pending button press */
	if (wcd_cancel_btn_work(mbhc))
		pr_info("%s: button press is canceled\n", __func__);

	pr_info("%s: enter, current_plug=%d\n", __func__, mbhc->current_plug);
	insert = !wcd_swch_level_remove(mbhc);
	if (mbhc->current_plug == MBHC_PLUG_TYPE_NONE && insert) {
		while(retry < HTC_AS_HEADSET_DETECT_RETRY) {
			if (!gpio_get_value(hs_cfg.id_gpio[TYPEC_POSITION])) {
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_0], 1);
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_1], 1);
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S4], 0);
				usleep_range(HTC_AS_HEADSET_DEBOUNCE_TIME, HTC_AS_HEADSET_DEBOUNCE_TIME);
				pr_info("%s: position flip\n", __func__);
			}

			if (gpio_get_value(hs_cfg.id_gpio[TYPEC_POSITION])) {
				pr_info("%s headset switch status S3_0=%d S3_1=%d S4=%d S5=%d\n", __func__,
					gpio_get_value(hs_cfg.switch_gpio[HEADSET_S3_0]),
					gpio_get_value(hs_cfg.switch_gpio[HEADSET_S3_1]),
					gpio_get_value(hs_cfg.switch_gpio[HEADSET_S4]),
					gpio_get_value(hs_cfg.switch_gpio[HEADSET_S5]));
				break;
			}

			gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_0], 0);
			gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_1], 0);
			gpio_set_value(hs_cfg.switch_gpio[HEADSET_S4], 1);
			usleep_range(HTC_AS_HEADSET_DEBOUNCE_TIME, HTC_AS_HEADSET_DEBOUNCE_TIME);
			pr_info("%s: retry=%d ID1=%d ID2=%d POSITION=%d\n",
				__func__, retry++,
				gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1]),
				gpio_get_value(hs_cfg.id_gpio[TYPEC_ID2]),
				gpio_get_value(hs_cfg.id_gpio[TYPEC_POSITION]));
		}

		if (gpio_get_value(hs_cfg.id_gpio[TYPEC_POSITION])) {
			mbhc->hph_status |= SND_JACK_HEADSET;
			wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
					(mbhc->hph_status | SND_JACK_MECHANICAL),
					WCD_MBHC_JACK_MASK);
			pr_info("%s: Reporting insertion (%x)\n",
				__func__, mbhc->hph_status);
			mbhc->current_plug = MBHC_PLUG_TYPE_AS_HEADSET;
			htc_button_detection(mbhc, true);
		} else {
			ret = -1;
			pr_err("%s retry count = %d",__func__, retry);
		}
	} else if (mbhc->current_plug == MBHC_PLUG_TYPE_AS_HEADSET && !insert) {
		mbhc->hph_status &= ~SND_JACK_HEADSET;
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				mbhc->hph_status, WCD_MBHC_JACK_MASK);
		pr_info("%s: Reporting removal (%x)\n",
				 __func__, mbhc->hph_status);
		mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
		htc_button_detection(mbhc, false);
	} else {
		pr_err("%s: unknown mbhc->current_plug : %d insert %d hph_status %x\n",
			__func__, mbhc->current_plug, insert, mbhc->hph_status);
	}

	if (!insert) {
		gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5],0);
	}

	/* Set the detection type appropriately */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MECH_DETECTION_TYPE,
				 !insert);

	WCD_MBHC_RSC_UNLOCK(mbhc);

	return ret;
}

static void htc_adapter_irq_handler(struct wcd_mbhc *mbhc){
	struct htc_headset_config hs_cfg = mbhc->mbhc_cfg->htc_headset_cfg;
	int adc = 0, insert = 0;

	pr_info("%s: enter, current_plug=%d\n", __func__, mbhc->current_plug);

	WCD_MBHC_RSC_LOCK(mbhc);

	/* cancel pending button press */
	if (wcd_cancel_btn_work(mbhc))
		pr_info("%s: button press is canceled\n", __func__);

	insert = !wcd_swch_level_remove(mbhc);
	if (mbhc->current_plug == MBHC_PLUG_TYPE_NONE && insert) {
		if (!gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1])) {
			gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_0], 1);
			gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_1], 1);
			gpio_set_value(hs_cfg.switch_gpio[HEADSET_S4], 0);
			usleep_range(HTC_AS_HEADSET_DEBOUNCE_TIME, HTC_AS_HEADSET_DEBOUNCE_TIME);
			pr_info("%s: position flip\n", __func__);
		}
		if (gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1])) {
			hs_cfg.get_adc_value(&adc, hs_cfg.adc_channel);
#if 0 /* HTC_AUD - disable 3.5mm adapter */
			if (adc > hs_cfg.adc_25mm_min) {
				mbhc->current_plug = MBHC_PLUG_TYPE_25MM_HEADSET;
				mbhc->hph_status |= SND_JACK_HEADSET;
				wcd9xxx_jack_report(mbhc, &mbhc->headset_jack, mbhc->hph_status,
						    WCD9XXX_JACK_MASK);
				pr_info("%s: Reporting insertion (%x)\n",
					__func__, mbhc->hph_status);
			} else if (adc > hs_cfg.adc_35mm_min && adc < hs_cfg.adc_35mm_max) {
				mbhc->current_plug = MBHC_PLUG_TYPE_35MM_HEADSET;
			}
			else {
				pr_err("%s: adc is not in range (%d)\n", __func__, adc);
			}
#else
			mbhc->current_plug = MBHC_PLUG_TYPE_25MM_HEADSET;
			mbhc->hph_status |= SND_JACK_HEADSET;
			wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
					mbhc->hph_status, WCD_MBHC_JACK_MASK);
			pr_info("%s: Reporting insertion (%x)\n",
						    __func__, mbhc->hph_status);
#endif
		} else {
			pr_err("%s: should not be here: ID1=%d ID2=%d\n", __func__,
				gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1]),
				gpio_get_value(hs_cfg.id_gpio[TYPEC_ID2]));
		}
#if 0 /* HTC_AUD - disable 3.5mm adapter */
	} else if ((mbhc->current_plug == MBHC_PLUG_TYPE_25MM_HEADSET || mbhc->current_plug == MBHC_PLUG_TYPE_35MM_HEADSET) && !insert) {
#else
	} else if (mbhc->current_plug == MBHC_PLUG_TYPE_25MM_HEADSET && !insert) {
#endif
		mbhc->hph_status &= ~SND_JACK_HEADSET;
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				mbhc->hph_status, WCD_MBHC_JACK_MASK);
		pr_info("%s: Reporting removal (%x)\n",
				 __func__, mbhc->hph_status);
		mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
	} else {
		pr_err("%s: unknown mbhc->current_plug : %d insert %d hph_status %x\n",__func__, mbhc->current_plug, insert, mbhc->hph_status);
	}

	/* Set the detection type appropriately */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MECH_DETECTION_TYPE,
				 !insert);
	WCD_MBHC_RSC_UNLOCK(mbhc);
}

static void enable_mic_bias(struct wcd_mbhc *mbhc, bool en)
{
	int rc = -1;

	pr_info("%s: en %d\n", __func__, en);

	if (en) {
		if (mbhc->mbhc_cb->mbhc_micbias_control) {
			mbhc->mbhc_cb->mbhc_micbias_control(mbhc->codec, MIC_BIAS_2, MICB_ENABLE);
			if (mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic) {
				rc = mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(mbhc->codec,
								MIC_BIAS_2, true);
				if (rc) {
					pr_err("%s: Micbias control for thr mic failed, rc: %d\n",
						__func__, rc);
				}
			} else {
				pr_err("%s: no mbhc_micb_ctrl_thr_mic\n", __func__);
			}
		} else {
			pr_err("%s: no mbhc_micbias_control\n", __func__);
		}
	} else {
		if (mbhc->mbhc_cb->mbhc_micbias_control) {
			mbhc->mbhc_cb->mbhc_micbias_control(mbhc->codec, MIC_BIAS_2, MICB_DISABLE);
			if (mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic) {
				rc = mbhc->mbhc_cb->mbhc_micb_ctrl_thr_mic(mbhc->codec,
								MIC_BIAS_2, false);
				if (rc) {
					pr_err("%s: Micbias control for thr mic failed, rc: %d\n",
						__func__, rc);
				}
			} else {
				pr_err("%s: no mbhc_micb_ctrl_thr_mic\n", __func__);
			}
		} else {
			pr_err("%s: no mbhc_micbias_control\n", __func__);
		}
	}
}

void set_fsa3030_accessory(struct wcd_mbhc *mbhc, bool en)
{
	struct htc_headset_config hs_cfg;

	if(!mbhc) {
		pr_err("%s: mbhc is NULL\n", __func__);
		return;
	}

	pr_info("%s: rara mode %d\n", __func__, en);

	hs_cfg = mbhc->mbhc_cfg->htc_headset_cfg;

	if (en) {
		/* accessory mode */
		gpio_set_value(hs_cfg.fsa3030_sel0, 1);
		gpio_set_value(hs_cfg.fsa3030_sel1, 0);
	} else{
		/* USB mode */
		gpio_set_value(hs_cfg.fsa3030_sel0, 0);
		gpio_set_value(hs_cfg.fsa3030_sel1, 1);
	}
}
/* HTC_AUD_END */

static irqreturn_t wcd_mbhc_mech_plug_detect_irq(int irq, void *data)
{
	int r = IRQ_HANDLED;
	struct wcd_mbhc *mbhc = data;
/* HTC_AUD_START - AS HS */
	int insert = -1, ret = 0;
	struct htc_headset_config hs_cfg = mbhc->mbhc_cfg->htc_headset_cfg;
/* HTC_AUD_END */

//HTC_AUD_START - debug purpose
#if 0
	pr_debug("%s: enter\n", __func__);
#else
	pr_info("%s: enter\n", __func__);
#endif
	if (unlikely((mbhc->mbhc_cb->lock_sleep(mbhc, true)) == false)) {
		pr_warn("%s: failed to hold suspend\n", __func__);
		r = IRQ_NONE;
	} else {
		/* Call handler */
/* HTC_AUD_START - AS HS */
#if 0
		wcd_mbhc_swch_irq_handler(mbhc);
#else
		if (hs_cfg.htc_headset_init) {
			WCD_MBHC_RSC_LOCK(mbhc);
			if (wcd_cancel_btn_work(mbhc)) {
				pr_err("%s: button press is canceled\n", __func__);
			}
			WCD_MBHC_RSC_UNLOCK(mbhc);

			insert = !wcd_swch_level_remove(mbhc);
			if (insert) {
				/* set switch to accessory mode */
				set_fsa3030_accessory(mbhc, true);

				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5], 1);
				gpio_set_value(hs_cfg.ext_micbias, 1);
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_0], 0);
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_1], 0);
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S4], 1);
				enable_mic_bias(mbhc, true);
				usleep_range(HTC_AS_HEADSET_DEBOUNCE_TIME, HTC_AS_HEADSET_DEBOUNCE_TIME);
				pr_info("%s: ID1=%d ID2=%d POSITION=%d\n", __func__,
					gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1]),
					gpio_get_value(hs_cfg.id_gpio[TYPEC_ID2]),
					gpio_get_value(hs_cfg.id_gpio[TYPEC_POSITION]));

				if (gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1]) & gpio_get_value(hs_cfg.id_gpio[TYPEC_ID2]) || mbhc->current_plug == MBHC_PLUG_TYPE_AS_HEADSET) {
					ret = htc_headset_irq_handler(mbhc);
					enable_mic_bias(mbhc, false);
#if 0 /* HTC_AUD - disable 3.5mm adapter */
				} else if (gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1]) ^ gpio_get_value(hs_cfg.id_gpio[TYPEC_ID2]) || mbhc->current_plug == MBHC_PLUG_TYPE_25MM_HEADSET || mbhc->current_plug == MBHC_PLUG_TYPE_35MM_HEADSET){
#else
				} else if (gpio_get_value(hs_cfg.id_gpio[TYPEC_ID1]) ^ gpio_get_value(hs_cfg.id_gpio[TYPEC_ID2]) || mbhc->current_plug == MBHC_PLUG_TYPE_25MM_HEADSET) {
#endif
					htc_adapter_irq_handler(mbhc);
					gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5],0);
					enable_mic_bias(mbhc, false);
				} else {
					pr_info("%s: mbhc->current_plug : %d insert %d hph_status %x\n",__func__, mbhc->current_plug, insert, mbhc->hph_status);
					enable_mic_bias(mbhc, false);
					gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5],0);
				}
				gpio_set_value(hs_cfg.ext_micbias,0);

				if (ret < 0) {
					pr_err("AS headset can't be identified during 3 seconds "); // Plug in AS headset when pressing button
					mbhc->mbhc_cb->lock_sleep(mbhc, false);
					return IRQ_HANDLED;
				}
			} else {
				/* set switch to usb mode */
				set_fsa3030_accessory(mbhc, false);

				pr_info("%s: Remove mbhc->current_plug : %d insert %d hph_status %x\n",__func__, mbhc->current_plug, insert, mbhc->hph_status);
				/* set switch gpio to default value */
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5], 0);
				gpio_set_value(hs_cfg.ext_micbias, 0);
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_0], 0);
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_1], 1);
				gpio_set_value(hs_cfg.switch_gpio[HEADSET_S4], 0);

				if (mbhc->current_plug == MBHC_PLUG_TYPE_25MM_HEADSET) {
					WCD_MBHC_RSC_LOCK(mbhc);
					mbhc->hph_status &= ~SND_JACK_HEADSET;
					wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
						mbhc->hph_status, WCD_MBHC_JACK_MASK);
					pr_info("%s: Reporting removal (%x)\n",
						__func__, mbhc->hph_status);
					mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
					/* Set the detection type appropriately */
					WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MECH_DETECTION_TYPE,
										!insert);
					WCD_MBHC_RSC_UNLOCK(mbhc);
					mbhc->mbhc_cb->lock_sleep(mbhc, false);
					return IRQ_HANDLED;
				} else if (mbhc->current_plug == MBHC_PLUG_TYPE_AS_HEADSET) {
					WCD_MBHC_RSC_LOCK(mbhc);
					mbhc->hph_status &= ~SND_JACK_HEADSET;
					wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
						mbhc->hph_status, WCD_MBHC_JACK_MASK);
					pr_info("%s: Reporting removal (%x)\n",
							 __func__, mbhc->hph_status);
					mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
					htc_button_detection(mbhc, false);
					gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5],0);
					/* Set the detection type appropriately */
					WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MECH_DETECTION_TYPE,
										!insert);
					WCD_MBHC_RSC_UNLOCK(mbhc);
					mbhc->mbhc_cb->lock_sleep(mbhc, false);
/* HTC_AUD_START */
					disable_anc_mic(mbhc);
/* HTC_AUD_END */
					return IRQ_HANDLED;
				}
			}

/* HTC_AUD_START - disable 3.5mm adapter */
			if (0) {
				if (mbhc->current_plug != MBHC_PLUG_TYPE_25MM_HEADSET && mbhc->current_plug != MBHC_PLUG_TYPE_AS_HEADSET) {
					pr_info("%s: call mbhc swch%d\n", __func__, r);
					wcd_mbhc_swch_irq_handler(mbhc);
				}
			}
/* HTC_AUD_END */

/* HTC_AUD_START - report unsupport if it's non HTC analog adapter */
			if (mbhc->current_plug != MBHC_PLUG_TYPE_25MM_HEADSET && mbhc->current_plug != MBHC_PLUG_TYPE_AS_HEADSET) {
				if (insert) {
					switch_set_state(&mbhc->unsupported_type, 1);
					pr_info("%s: Reporting insertion unsupported device \n", __func__);
				} else {
					switch_set_state(&mbhc->unsupported_type, 0);
					pr_info("%s: Reporting removal unsupported device \n", __func__);

					/* set switch gpio to default value */
					gpio_set_value(hs_cfg.switch_gpio[HEADSET_S5], 0);
					gpio_set_value(hs_cfg.ext_micbias, 0);
					gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_0], 0);
					gpio_set_value(hs_cfg.switch_gpio[HEADSET_S3_1], 1);
					gpio_set_value(hs_cfg.switch_gpio[HEADSET_S4], 0);
				}
				WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MECH_DETECTION_TYPE, !insert);
			}
/* HTC_AUD_END */

			if (insert >= 0) {
				if (!insert && mbhc->current_plug != MBHC_PLUG_TYPE_NONE) {
					WCD_MBHC_RSC_LOCK(mbhc);
					pr_err("%s: force remove hph_status=%d, current_plug=%d\n",
						__func__, mbhc->hph_status, mbhc->current_plug);
					mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
					mbhc->hph_status = 0;
					wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
						mbhc->hph_status, WCD_MBHC_JACK_MASK);
					pr_info("%s %d: Reporting removal (%x)\n",
						__func__, __LINE__, mbhc->hph_status);
					htc_button_detection(mbhc, false);
					/* Set the detection type appropriately */
					WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_MECH_DETECTION_TYPE, !insert);
					WCD_MBHC_RSC_UNLOCK(mbhc);
				}
			}
		}
#endif
/* HTC_AUD_END */
		mbhc->mbhc_cb->lock_sleep(mbhc, false);
	}
//HTC_AUD_START - debug purpose
#if 0
	pr_debug("%s: leave %d\n", __func__, r);
#else
	pr_info("%s: leave %d\n", __func__, r);
#endif
	return r;
}

static int wcd_mbhc_get_button_mask(struct wcd_mbhc *mbhc)
{
	int mask = 0;
	int btn;

	btn = mbhc->mbhc_cb->map_btn_code_to_num(mbhc->codec);

	switch (btn) {
	case 0:
		mask = SND_JACK_BTN_0;
		break;
	case 1:
		mask = SND_JACK_BTN_1;
		break;
	case 2:
		mask = SND_JACK_BTN_2;
		break;
	case 3:
		mask = SND_JACK_BTN_3;
		break;
	case 4:
		mask = SND_JACK_BTN_4;
		break;
	case 5:
		mask = SND_JACK_BTN_5;
		break;
	default:
/* HTC_AUD_START - debug purpose */
		pr_info("%s: btn not match\n", __func__);
		btn = -1;
/* HTC_AUD_END */
		break;
	}

/* HTC_AUD_START - debug purpose */
	if (btn >= 0)
		pr_info("%s: btn %d, key_code %d, mask 0x%x\n", __func__, btn, mbhc->mbhc_cfg->key_code[btn], mask);
/* HTC_AUD_END */

	return mask;
}

static irqreturn_t wcd_mbhc_hs_ins_irq(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	bool detection_type = 0, hphl_sch = 0, mic_sch = 0;
	u16 elect_result = 0;
/* HTC_AUD_START - fix klockwork */
#if 0
	static u16 hphl_trigerred;
	static u16 mic_trigerred;
#else
	static u16 hphl_trigerred = 0;
	static u16 mic_trigerred = 0;
#endif
/* HTC_AUD_END */

	pr_debug("%s: enter\n", __func__);
	if (!mbhc->mbhc_cfg->detect_extn_cable) {
		pr_debug("%s: Returning as Extension cable feature not enabled\n",
			__func__);
		return IRQ_HANDLED;
	}
	WCD_MBHC_RSC_LOCK(mbhc);

	WCD_MBHC_REG_READ(WCD_MBHC_ELECT_DETECTION_TYPE, detection_type);
	WCD_MBHC_REG_READ(WCD_MBHC_ELECT_RESULT, elect_result);

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: detection_type %d, elect_result %x\n", __func__,
				detection_type, elect_result);
#else
	pr_info("%s: detection_type %d, elect_result %x\n", __func__,
				detection_type, elect_result);
#endif
/* HTC_AUD_END */
	if (detection_type) {
		/* check if both Left and MIC Schmitt triggers are triggered */
		WCD_MBHC_REG_READ(WCD_MBHC_HPHL_SCHMT_RESULT, hphl_sch);
		WCD_MBHC_REG_READ(WCD_MBHC_MIC_SCHMT_RESULT, mic_sch);
		if (hphl_sch && mic_sch) {
			/* Go for plug type determination */
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: Go for plug type determination\n",
				  __func__);
#else
			pr_info("%s: Go for plug type determination\n",
				  __func__);
#endif
/* HTC_AUD_END */
			goto determine_plug;

		} else {
			if (mic_sch) {
				mic_trigerred++;
				pr_debug("%s: Insertion MIC trigerred %d\n",
					 __func__, mic_trigerred);
				WCD_MBHC_REG_UPDATE_BITS(
						WCD_MBHC_ELECT_SCHMT_ISRC,
						0);
				msleep(20);
				WCD_MBHC_REG_UPDATE_BITS(
						WCD_MBHC_ELECT_SCHMT_ISRC,
						1);
			}
			if (hphl_sch) {
				hphl_trigerred++;
				pr_debug("%s: Insertion HPHL trigerred %d\n",
					 __func__, hphl_trigerred);
			}
			if (mic_trigerred && hphl_trigerred) {
				/* Go for plug type determination */
/* HTC_AUD_START - debug purpose */
#if 0
				pr_debug("%s: Go for plug type determination\n",
					 __func__);
#else
				pr_info("%s: Go for plug type determination\n",
					 __func__);
#endif
/* HTC_AUD_END */
				goto determine_plug;
			}
		}
	}
	WCD_MBHC_RSC_UNLOCK(mbhc);
	pr_debug("%s: leave\n", __func__);
	return IRQ_HANDLED;

determine_plug:
	/*
	 * Disable HPHL trigger and MIC Schmitt triggers.
	 * Setup for insertion detection.
	 */
	pr_debug("%s: Disable insertion interrupt\n", __func__);
	wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_INS,
			     false);

	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC, 0);
	hphl_trigerred = 0;
	mic_trigerred = 0;
	mbhc->is_extn_cable = true;
	mbhc->btn_press_intr = false;
	mbhc->is_btn_press = false;
	wcd_mbhc_detect_plug_type(mbhc);
	WCD_MBHC_RSC_UNLOCK(mbhc);
	pr_debug("%s: leave\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t wcd_mbhc_hs_rem_irq(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	u8 hs_comp_result = 0, hphl_sch = 0, mic_sch = 0;
/* HTC_AUD_START - klockwork */
#if 0
	static u16 hphl_trigerred;
	static u16 mic_trigerred;
	unsigned long timeout;
#else
	static u16 hphl_trigerred = 0;
	static u16 mic_trigerred = 0;
	unsigned long timeout = 0;
#endif
/* HTC_AUD_END */

	bool removed = true;
	int retry = 0;

	pr_debug("%s: enter\n", __func__);

	WCD_MBHC_RSC_LOCK(mbhc);

	timeout = jiffies +
		  msecs_to_jiffies(WCD_FAKE_REMOVAL_MIN_PERIOD_MS);
	do {
		retry++;
		/*
		 * read the result register every 10ms to look for
		 * any change in HS_COMP_RESULT bit
		 */
		usleep_range(10000, 10100);
		WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_result);
		pr_debug("%s: Check result reg for fake removal: hs_comp_res %x\n",
			 __func__, hs_comp_result);
		if ((!hs_comp_result) &&
		    retry > FAKE_REM_RETRY_ATTEMPTS) {
			removed = false;
			break;
		}
	} while (!time_after(jiffies, timeout));

	if (wcd_swch_level_remove(mbhc)) {
		pr_debug("%s: Switch level is low ", __func__);
		goto exit;
	}
/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: headset %s actually removed\n", __func__,
		removed ? "" : "not ");
#else
	pr_info("%s: headset %s actually removed\n", __func__,
		removed ? "" : "not ");
#endif
/* HTC_AUD_END */

	WCD_MBHC_REG_READ(WCD_MBHC_HPHL_SCHMT_RESULT, hphl_sch);
	WCD_MBHC_REG_READ(WCD_MBHC_MIC_SCHMT_RESULT, mic_sch);
	WCD_MBHC_REG_READ(WCD_MBHC_HS_COMP_RESULT, hs_comp_result);

	if (removed) {
		if (!(hphl_sch && mic_sch && hs_comp_result)) {
			/*
			 * extension cable is still plugged in
			 * report it as LINEOUT device
			 */
			goto report_unplug;
		} else {
			if (!mic_sch) {
				mic_trigerred++;
				pr_debug("%s: Removal MIC trigerred %d\n",
					 __func__, mic_trigerred);
			}
			if (!hphl_sch) {
				hphl_trigerred++;
				pr_debug("%s: Removal HPHL trigerred %d\n",
					 __func__, hphl_trigerred);
			}
			if (mic_trigerred && hphl_trigerred) {
				/*
				 * extension cable is still plugged in
				 * report it as LINEOUT device
				 */
				goto report_unplug;
			}
		}
	}
exit:
	WCD_MBHC_RSC_UNLOCK(mbhc);
	pr_debug("%s: leave\n", __func__);
	return IRQ_HANDLED;

report_unplug:

	/* cancel pending button press */
	if (wcd_cancel_btn_work(mbhc))
		pr_debug("%s: button press is canceled\n", __func__);
	/* cancel correct work function */
	wcd_cancel_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: Report extension cable\n", __func__);
#else
	pr_info("%s: Report extension cable\n", __func__);
#endif
/* HTC_AUD_END */
	wcd_mbhc_report_plug(mbhc, 1, SND_JACK_LINEOUT);
	/*
	 * If PA is enabled HPHL schmitt trigger can
	 * be unreliable, make sure to disable it
	 */
	if (test_bit(WCD_MBHC_EVENT_PA_HPHL,
		&mbhc->event_state))
		wcd_mbhc_set_and_turnoff_hph_padac(mbhc);
	/*
	 * Disable HPHL trigger and MIC Schmitt triggers.
	 * Setup for insertion detection.
	 */
	wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_REM,
			     false);
	wcd_enable_curr_micbias(mbhc, WCD_MBHC_EN_NONE);
	/* Disable HW FSM */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_FSM_EN, 0);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_SCHMT_ISRC, 3);

	/* Set the detection type appropriately */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_ELECT_DETECTION_TYPE, 1);
	wcd_mbhc_hs_elec_irq(mbhc, WCD_MBHC_ELEC_HS_INS,
			     true);
	hphl_trigerred = 0;
	mic_trigerred = 0;
	WCD_MBHC_RSC_UNLOCK(mbhc);
	pr_debug("%s: leave\n", __func__);
	return IRQ_HANDLED;
}

static void wcd_btn_lpress_fn(struct work_struct *work)
{
	struct delayed_work *dwork;
	struct wcd_mbhc *mbhc;
	s16 btn_result = 0;

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: Enter\n", __func__);
#else
	pr_info("%s: Enter\n", __func__);
#endif
/* HTC_AUD_END */

	dwork = to_delayed_work(work);
	mbhc = container_of(dwork, struct wcd_mbhc, mbhc_btn_dwork);

	WCD_MBHC_REG_READ(WCD_MBHC_BTN_RESULT, btn_result);
/* HTC_AUD_START - AS HS */
#if 0
	if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET) {
#else
	if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADSET ||
			mbhc->current_plug == MBHC_PLUG_TYPE_AS_HEADSET) {
#endif
/* HTC_AUD_END */
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Reporting long button press event, btn_result: %d\n",
			 __func__, btn_result);
#else
		pr_info("%s: Reporting long button press event, btn_result: %d\n",
			 __func__, btn_result);
#endif
/* HTC_AUD_END */
		wcd_mbhc_jack_report(mbhc, &mbhc->button_jack,
				mbhc->buttons_pressed, mbhc->buttons_pressed);
	}
	pr_debug("%s: leave\n", __func__);
	mbhc->mbhc_cb->lock_sleep(mbhc, false);
}

static bool wcd_mbhc_fw_validate(const void *data, size_t size)
{
	u32 cfg_offset;
	struct wcd_mbhc_btn_detect_cfg *btn_cfg;
	struct firmware_cal fw;

	fw.data = (void *)data;
	fw.size = size;

	if (fw.size < WCD_MBHC_CAL_MIN_SIZE)
		return false;

	/*
	 * Previous check guarantees that there is enough fw data up
	 * to num_btn
	 */
	btn_cfg = WCD_MBHC_CAL_BTN_DET_PTR(fw.data);
	cfg_offset = (u32) ((void *) btn_cfg - (void *) fw.data);
	if (fw.size < (cfg_offset + WCD_MBHC_CAL_BTN_SZ(btn_cfg)))
		return false;

	return true;
}

static irqreturn_t wcd_mbhc_btn_press_handler(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	int mask;
	unsigned long msec_val;

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: enter\n", __func__);
#else
	pr_info("%s: enter\n", __func__);
#endif
/* HTC_AUD_END */
	complete(&mbhc->btn_press_compl);
	WCD_MBHC_RSC_LOCK(mbhc);
	wcd_cancel_btn_work(mbhc);
	if (wcd_swch_level_remove(mbhc)) {
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Switch level is low ", __func__);
#else
		pr_info("%s: Switch level is low ", __func__);
#endif
/* HTC_AUD_END */
		goto done;
	}

	mbhc->is_btn_press = true;
	msec_val = jiffies_to_msecs(jiffies - mbhc->jiffies_atreport);
	pr_debug("%s: msec_val = %ld\n", __func__, msec_val);
	if (msec_val < MBHC_BUTTON_PRESS_THRESHOLD_MIN) {
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Too short, ignore button press\n", __func__);
#else
		pr_info("%s: Too short, ignore button press\n", __func__);
#endif
/* HTC_AUD_END */
		goto done;
	}

	/* If switch interrupt already kicked in, ignore button press */
	if (mbhc->in_swch_irq_handler) {
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Swtich level changed, ignore button press\n",
			 __func__);
#else
		pr_info("%s: Swtich level changed, ignore button press\n",
			 __func__);
#endif
/* HTC_AUD_END */
		goto done;
	}

	mask = wcd_mbhc_get_button_mask(mbhc);
	if (mask == SND_JACK_BTN_0)
		mbhc->btn_press_intr = true;

/* HTC_AUD_START - AS HS */
#if 0
	if (mbhc->current_plug != MBHC_PLUG_TYPE_HEADSET) {
#else
	if (mbhc->current_plug != MBHC_PLUG_TYPE_HEADSET &&
			mbhc->current_plug != MBHC_PLUG_TYPE_AS_HEADSET) {
#endif
/* HTC_AUD_END */
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Plug isn't headset, ignore button press\n",
				__func__);
#else
#endif
/* HTC_AUD_END */
		goto done;
	}
	mbhc->buttons_pressed |= mask;
	mbhc->mbhc_cb->lock_sleep(mbhc, true);
/* HTC_AUD_START - apply HTC setting */
#if 0
	if (schedule_delayed_work(&mbhc->mbhc_btn_dwork,
				msecs_to_jiffies(400)) == 0) {
#else
	if (schedule_delayed_work(&mbhc->mbhc_btn_dwork,
				msecs_to_jiffies(500)) == 0) {
#endif
/* HTC_AUD_END */
		WARN(1, "Button pressed twice without release event\n");
		mbhc->mbhc_cb->lock_sleep(mbhc, false);
	}
done:
	pr_debug("%s: leave\n", __func__);
	WCD_MBHC_RSC_UNLOCK(mbhc);
	return IRQ_HANDLED;
}

static irqreturn_t wcd_mbhc_release_handler(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	int ret;

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: enter\n", __func__);
#else
	pr_info("%s: enter\n", __func__);
#endif
/* HTC_AUD_END */
	WCD_MBHC_RSC_LOCK(mbhc);
	if (wcd_swch_level_remove(mbhc)) {
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: Switch level is low ", __func__);
#else
		pr_info("%s: Switch level is low ", __func__);
#endif
/* HTC_AUD_END */
		goto exit;
	}

	if (mbhc->is_btn_press) {
		mbhc->is_btn_press = false;
	} else {
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: This release is for fake btn press\n", __func__);
#else
		pr_info("%s: This release is for fake btn press\n", __func__);
#endif
/* HTC_AUD_END */
		goto exit;
	}

	/*
	 * If current plug is headphone then there is no chance to
	 * get btn release interrupt, so connected cable should be
	 * headset not headphone.
	 */
	if (mbhc->current_plug == MBHC_PLUG_TYPE_HEADPHONE) {
		pr_info("%s: report headset due to it has no chance to get btn release irq if current plug is headphone\n", __func__); //HTC_AUD
		wcd_mbhc_find_plug_and_report(mbhc, MBHC_PLUG_TYPE_HEADSET);
		goto exit;

	}
	if (mbhc->buttons_pressed & WCD_MBHC_JACK_BUTTON_MASK) {
		ret = wcd_cancel_btn_work(mbhc);
		if (ret == 0) {
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: Reporting long button release event\n",
				 __func__);
#else
			pr_info("%s: Reporting long button release event\n",
				 __func__);
#endif
/* HTC_AUD_END */
			wcd_mbhc_jack_report(mbhc, &mbhc->button_jack,
					0, mbhc->buttons_pressed);
		} else {
			if (mbhc->in_swch_irq_handler) {
/* HTC_AUD_START - debug purpose */
#if 0
				pr_debug("%s: Switch irq kicked in, ignore\n",
					__func__);
#else
				pr_info("%s: Switch irq kicked in, ignore\n",
					__func__);
#endif
/* HTC_AUD_END */
			} else {
/* HTC_AUD_START - debug purpose */
#if 0
				pr_debug("%s: Reporting btn press\n",
					 __func__);
#else
				pr_info("%s: Reporting btn press\n",
					 __func__);
#endif
/* HTC_AUD_END */
				wcd_mbhc_jack_report(mbhc,
						     &mbhc->button_jack,
						     mbhc->buttons_pressed,
						     mbhc->buttons_pressed);
/* HTC_AUD_START - debug purpose */
#if 0
				pr_debug("%s: Reporting btn release\n",
					 __func__);
#else
				pr_info("%s: Reporting btn release\n",
					 __func__);
#endif
/* HTC_AUD_END */
				wcd_mbhc_jack_report(mbhc,
						&mbhc->button_jack,
						0, mbhc->buttons_pressed);
			}
		}
		mbhc->buttons_pressed &= ~WCD_MBHC_JACK_BUTTON_MASK;
	}
exit:
	pr_debug("%s: leave\n", __func__);
	WCD_MBHC_RSC_UNLOCK(mbhc);
	return IRQ_HANDLED;
}

static irqreturn_t wcd_mbhc_hphl_ocp_irq(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;
	int val;

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: received HPHL OCP irq\n", __func__);
#else
	pr_info("%s: received HPHL OCP irq\n", __func__);
#endif
/* HTC_AUD_END */
	if (mbhc) {
		if (mbhc->mbhc_cb->hph_register_recovery) {
			if (mbhc->mbhc_cb->hph_register_recovery(mbhc)) {
				WCD_MBHC_REG_READ(WCD_MBHC_HPHR_OCP_STATUS,
						  val);
				if ((val != -EINVAL) && val)
					mbhc->is_hph_ocp_pending = true;
				goto done;
			}
		}

		if (mbhc->hphlocp_cnt < OCP_ATTEMPT) {
			mbhc->hphlocp_cnt++;
/* HTC_AUD_START - debug purpose */
#if 0
			pr_debug("%s: retry, hphlocp_cnt: %d\n", __func__,
				 mbhc->hphlocp_cnt);
#else
			pr_info("%s: retry, hphlocp_cnt: %d\n", __func__,
				 mbhc->hphlocp_cnt);
#endif
/* HTC_AUD_END */
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_OCP_FSM_EN, 0);
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_OCP_FSM_EN, 1);
		} else {
			mbhc->mbhc_cb->irq_control(mbhc->codec,
						   mbhc->intr_ids->hph_left_ocp,
						   false);
			mbhc->hph_status |= SND_JACK_OC_HPHL;
			wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
					    mbhc->hph_status,
					    WCD_MBHC_JACK_MASK);
		}
	} else {
		pr_err("%s: Bad wcd9xxx_spmi private data\n", __func__);
	}
done:
	return IRQ_HANDLED;
}

static irqreturn_t wcd_mbhc_hphr_ocp_irq(int irq, void *data)
{
	struct wcd_mbhc *mbhc = data;

/* HTC_AUD_START - debug purpose */
#if 0
	pr_debug("%s: received HPHR OCP irq\n", __func__);
#else
	pr_info("%s: received HPHR OCP irq\n", __func__);
#endif
/* HTC_AUD_END */

	if (!mbhc) {
		pr_err("%s: Bad mbhc private data\n", __func__);
		goto done;
	}

	if (mbhc->is_hph_ocp_pending) {
		mbhc->is_hph_ocp_pending = false;
		goto done;
	}

	if (mbhc->mbhc_cb->hph_register_recovery) {
		if (mbhc->mbhc_cb->hph_register_recovery(mbhc))
			/* register corruption, hence reset registers */
			goto done;
	}
	if (mbhc->hphrocp_cnt < OCP_ATTEMPT) {
		mbhc->hphrocp_cnt++;
/* HTC_AUD_START - debug purpose */
#if 0
		pr_debug("%s: retry, hphrocp_cnt: %d\n", __func__,
			 mbhc->hphrocp_cnt);
#else
		pr_info("%s: retry, hphrocp_cnt: %d\n", __func__,
			 mbhc->hphrocp_cnt);
#endif
/* HTC_AUD_END */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_OCP_FSM_EN, 0);
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_OCP_FSM_EN, 1);
	} else {
		mbhc->mbhc_cb->irq_control(mbhc->codec,
					   mbhc->intr_ids->hph_right_ocp,
					   false);
		mbhc->hph_status |= SND_JACK_OC_HPHR;
		wcd_mbhc_jack_report(mbhc, &mbhc->headset_jack,
				    mbhc->hph_status, WCD_MBHC_JACK_MASK);
	}
done:
	return IRQ_HANDLED;
}

static int wcd_mbhc_initialise(struct wcd_mbhc *mbhc)
{
	int ret = 0;
	struct snd_soc_codec *codec = mbhc->codec;

	pr_debug("%s: enter\n", __func__);
	WCD_MBHC_RSC_LOCK(mbhc);

	/* enable HS detection */
	if (mbhc->mbhc_cb->hph_pull_up_control)
		mbhc->mbhc_cb->hph_pull_up_control(codec, I_DEFAULT);
	else
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HS_L_DET_PULL_UP_CTRL, 3);

	if (mbhc->mbhc_cfg->moisture_en && mbhc->mbhc_cb->mbhc_moisture_config)
		mbhc->mbhc_cb->mbhc_moisture_config(mbhc);

	/*
	 * For USB analog we need to override the switch configuration.
	 * Also, disable hph_l pull-up current source as HS_DET_L is driven
	 * by an external source
	 */
	if (mbhc->mbhc_cfg->enable_usbc_analog) {
		mbhc->hphl_swh = 1;
		mbhc->gnd_swh = 1;

		if (mbhc->mbhc_cb->hph_pull_up_control)
			mbhc->mbhc_cb->hph_pull_up_control(codec, I_OFF);
		else
			WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HS_L_DET_PULL_UP_CTRL,
						 0);
	}

	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HPHL_PLUG_TYPE, mbhc->hphl_swh);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_GND_PLUG_TYPE, mbhc->gnd_swh);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_SW_HPH_LP_100K_TO_GND, 1);
	if (mbhc->mbhc_cfg->gnd_det_en && mbhc->mbhc_cb->mbhc_gnd_det_ctrl)
		mbhc->mbhc_cb->mbhc_gnd_det_ctrl(codec, true);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_HS_L_DET_PULL_UP_COMP_CTRL, 1);
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_L_DET_EN, 1);

/* HTC_AUD_START - apply HTC setting */
#if 0
	if (mbhc->mbhc_cfg->enable_usbc_analog) {
		/* Insertion debounce set to 48ms */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_INSREM_DBNC, 4);
	} else {
		/* Insertion debounce set to 96ms */
		WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_INSREM_DBNC, 6);
	}

	/* Button Debounce set to 16ms */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_DBNC, 2);
#else
	/* Insertion debounce set to 384ms */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_INSREM_DBNC, 0xA); /* case 02318120 */
	/* Button Debounce set to 32ms */
	WCD_MBHC_REG_UPDATE_BITS(WCD_MBHC_BTN_DBNC, 3); /* case 02423163 */
#endif
/* HTC_AUD_END */

	/* Enable micbias ramp */
	if (mbhc->mbhc_cb->mbhc_micb_ramp_control)
		mbhc->mbhc_cb->mbhc_micb_ramp_control(codec, true);
	/* enable bias */
	mbhc->mbhc_cb->mbhc_bias(codec, true);
	/* enable MBHC clock */
	if (mbhc->mbhc_cb->clk_setup)
		mbhc->mbhc_cb->clk_setup(codec, true);

	/* program HS_VREF value */
	wcd_program_hs_vref(mbhc);

	wcd_program_btn_threshold(mbhc, false);

	INIT_WORK(&mbhc->correct_plug_swch, wcd_correct_swch_plug);

	init_completion(&mbhc->btn_press_compl);

	WCD_MBHC_RSC_UNLOCK(mbhc);
	pr_debug("%s: leave\n", __func__);
	return ret;
}

static void wcd_mbhc_fw_read(struct work_struct *work)
{
	struct delayed_work *dwork;
	struct wcd_mbhc *mbhc;
	struct snd_soc_codec *codec;
	const struct firmware *fw;
	struct firmware_cal *fw_data = NULL;
	int ret = -1, retry = 0;
	bool use_default_cal = false;

	dwork = to_delayed_work(work);
	mbhc = container_of(dwork, struct wcd_mbhc, mbhc_firmware_dwork);
	codec = mbhc->codec;

	while (retry < FW_READ_ATTEMPTS) {
		retry++;
		pr_debug("%s:Attempt %d to request MBHC firmware\n",
			__func__, retry);
		if (mbhc->mbhc_cb->get_hwdep_fw_cal)
			fw_data = mbhc->mbhc_cb->get_hwdep_fw_cal(mbhc,
					WCD9XXX_MBHC_CAL);
		if (!fw_data)
			ret = request_firmware(&fw, "wcd9320/wcd9320_mbhc.bin",
				       codec->dev);
		/*
		 * if request_firmware and hwdep cal both fail then
		 * sleep for 4sec for the userspace to send data to kernel
		 * retry for few times before bailing out
		 */
		if ((ret != 0) && !fw_data) {
			usleep_range(FW_READ_TIMEOUT, FW_READ_TIMEOUT +
					WCD_MBHC_USLEEP_RANGE_MARGIN_US);
		} else {
			pr_debug("%s: MBHC Firmware read succesful\n",
					__func__);
			break;
		}
	}
	if (!fw_data)
		pr_debug("%s: using request_firmware\n", __func__);
	else
		pr_debug("%s: using hwdep cal\n", __func__);

	if (ret != 0 && !fw_data) {
		pr_err("%s: Cannot load MBHC firmware use default cal\n",
		       __func__);
		use_default_cal = true;
	}
	if (!use_default_cal) {
		const void *data;
		size_t size;

		if (fw_data) {
			data = fw_data->data;
			size = fw_data->size;
		} else {
			data = fw->data;
			size = fw->size;
		}
		if (wcd_mbhc_fw_validate(data, size) == false) {
			pr_err("%s: Invalid MBHC cal data size use default cal\n",
				__func__);
			if (!fw_data)
				release_firmware(fw);
		} else {
			if (fw_data) {
				mbhc->mbhc_cfg->calibration =
					(void *)fw_data->data;
				mbhc->mbhc_cal = fw_data;
			} else {
				mbhc->mbhc_cfg->calibration =
					(void *)fw->data;
				mbhc->mbhc_fw = fw;
			}
		}

	}

	(void) wcd_mbhc_initialise(mbhc);
}

int wcd_mbhc_set_keycode(struct wcd_mbhc *mbhc)
{
	enum snd_jack_types type;
	int i, ret, result = 0;
	int *btn_key_code;

	btn_key_code = mbhc->mbhc_cfg->key_code;

	for (i = 0 ; i < WCD_MBHC_KEYCODE_NUM ; i++) {
		if (btn_key_code[i] != 0) {
			switch (i) {
			case 0:
				type = SND_JACK_BTN_0;
				break;
			case 1:
				type = SND_JACK_BTN_1;
				break;
			case 2:
				type = SND_JACK_BTN_2;
				break;
			case 3:
				type = SND_JACK_BTN_3;
				break;
			case 4:
				type = SND_JACK_BTN_4;
				break;
			case 5:
				type = SND_JACK_BTN_5;
				break;
			default:
				WARN_ONCE(1, "Wrong button number:%d\n", i);
				result = -1;
				return result;
			}
			ret = snd_jack_set_key(mbhc->button_jack.jack,
							type,
							btn_key_code[i]);
			if (ret) {
				pr_err("%s: Failed to set code for %d\n",
					__func__, btn_key_code[i]);
				result = -1;
				return result;
			}
			input_set_capability(
				mbhc->button_jack.jack->input_dev,
				EV_KEY, btn_key_code[i]);
			pr_debug("%s: set btn%d key code:%d\n", __func__,
				i, btn_key_code[i]);
		}
	}
	if (btn_key_code[0])
		mbhc->is_btn_already_regd = true;
	return result;
}

static int wcd_mbhc_usb_c_analog_setup_gpios(struct wcd_mbhc *mbhc,
					     bool active)
{
	int rc = 0;
	struct usbc_ana_audio_config *config =
		&mbhc->mbhc_cfg->usbc_analog_cfg;
	union power_supply_propval pval;

	dev_dbg(mbhc->codec->dev, "%s: setting GPIOs active = %d\n",
		__func__, active);

	memset(&pval, 0, sizeof(pval));

	if (active) {
		pval.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		if (power_supply_set_property(mbhc->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &pval))
			dev_info(mbhc->codec->dev, "%s: force PR_SOURCE mode unsuccessful\n",
				 __func__);
		else
			mbhc->usbc_force_pr_mode = true;

		if (config->usbc_en1_gpio_p)
			rc = msm_cdc_pinctrl_select_active_state(
				config->usbc_en1_gpio_p);
		if (rc == 0 && config->usbc_en2n_gpio_p)
			rc = msm_cdc_pinctrl_select_active_state(
				config->usbc_en2n_gpio_p);
		if (rc == 0 && config->usbc_force_gpio_p)
			rc = msm_cdc_pinctrl_select_active_state(
				config->usbc_force_gpio_p);
		mbhc->usbc_mode = POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER;
	} else {
		/* no delay is required when disabling GPIOs */
		if (config->usbc_en2n_gpio_p)
			msm_cdc_pinctrl_select_sleep_state(
				config->usbc_en2n_gpio_p);
		if (config->usbc_en1_gpio_p)
			msm_cdc_pinctrl_select_sleep_state(
				config->usbc_en1_gpio_p);
		if (config->usbc_force_gpio_p)
			msm_cdc_pinctrl_select_sleep_state(
				config->usbc_force_gpio_p);

		if (mbhc->usbc_force_pr_mode) {
			pval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
			if (power_supply_set_property(mbhc->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &pval))
				dev_info(mbhc->codec->dev, "%s: force PR_DUAL mode unsuccessful\n",
					 __func__);

			mbhc->usbc_force_pr_mode = false;
		}

		mbhc->usbc_mode = POWER_SUPPLY_TYPEC_NONE;
	}

	return rc;
}

/* workqueue */
static void wcd_mbhc_usbc_analog_work_fn(struct work_struct *work)
{
	struct wcd_mbhc *mbhc =
		container_of(work, struct wcd_mbhc, usbc_analog_work);

	wcd_mbhc_usb_c_analog_setup_gpios(mbhc,
			mbhc->usbc_mode != POWER_SUPPLY_TYPEC_NONE);
}

/* this callback function is used to process PMI notification */
static int wcd_mbhc_usb_c_event_changed(struct notifier_block *nb,
					unsigned long evt, void *ptr)
{
	int ret;
	union power_supply_propval mode;
	struct wcd_mbhc *mbhc = container_of(nb, struct wcd_mbhc, psy_nb);
	struct snd_soc_codec *codec = mbhc->codec;

	if (ptr != mbhc->usb_psy || evt != PSY_EVENT_PROP_CHANGED)
		return 0;

	ret = power_supply_get_property(mbhc->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_MODE, &mode);
	if (ret) {
		dev_err(codec->dev, "%s: Unable to read USB TYPEC_MODE: %d\n",
			__func__, ret);
		return ret;
	}

	dev_dbg(codec->dev, "%s: USB change event received\n",
		__func__);
	dev_dbg(codec->dev, "%s: supply mode %d, expected %d\n", __func__,
		mode.intval, POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER);

	switch (mode.intval) {
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
	case POWER_SUPPLY_TYPEC_NONE:
		dev_dbg(codec->dev, "%s: usbc_mode: %d; mode.intval: %d\n",
			__func__, mbhc->usbc_mode, mode.intval);

		if (mbhc->usbc_mode == mode.intval)
			break; /* filter notifications received before */
		mbhc->usbc_mode = mode.intval;

		dev_dbg(codec->dev, "%s: queueing usbc_analog_work\n",
			__func__);
		schedule_work(&mbhc->usbc_analog_work);
		break;
	default:
		break;
	}
	return ret;
}

/* PMI registration code */
static int wcd_mbhc_usb_c_analog_init(struct wcd_mbhc *mbhc)
{
	int ret = 0;
	struct snd_soc_codec *codec = mbhc->codec;

	dev_dbg(mbhc->codec->dev, "%s: usb-c analog setup start\n", __func__);
	INIT_WORK(&mbhc->usbc_analog_work, wcd_mbhc_usbc_analog_work_fn);

	mbhc->usb_psy = power_supply_get_by_name("usb");
	if (IS_ERR_OR_NULL(mbhc->usb_psy)) {
		dev_err(codec->dev, "%s: could not get USB psy info\n",
			__func__);
		ret = -EPROBE_DEFER;
		if (IS_ERR(mbhc->usb_psy))
			ret = PTR_ERR(mbhc->usb_psy);
		mbhc->usb_psy = NULL;
		goto err;
	}

	ret = wcd_mbhc_usb_c_analog_setup_gpios(mbhc, false);
	if (ret) {
		dev_err(codec->dev, "%s: error while setting USBC ana gpios\n",
			__func__);
		goto err;
	}

	mbhc->psy_nb.notifier_call = wcd_mbhc_usb_c_event_changed;
	mbhc->psy_nb.priority = 0;
	ret = power_supply_reg_notifier(&mbhc->psy_nb);
	if (ret) {
		dev_err(codec->dev, "%s: power supply registration failed\n",
			__func__);
		goto err;
	}

	/*
	* as part of the init sequence check if there is a connected
	* USB C analog adapter
	*/
	dev_dbg(mbhc->codec->dev, "%s: verify if USB adapter is already inserted\n",
		__func__);
	ret = wcd_mbhc_usb_c_event_changed(&mbhc->psy_nb,
					   PSY_EVENT_PROP_CHANGED,
					   mbhc->usb_psy);

err:
	return ret;
}

static int wcd_mbhc_usb_c_analog_deinit(struct wcd_mbhc *mbhc)
{
	wcd_mbhc_usb_c_analog_setup_gpios(mbhc, false);

	/* deregister from PMI */
	power_supply_unreg_notifier(&mbhc->psy_nb);

	return 0;
}

static int wcd_mbhc_init_gpio(struct wcd_mbhc *mbhc,
			      struct wcd_mbhc_config *mbhc_cfg,
			      const char *gpio_dt_str,
			      int *gpio, struct device_node **gpio_dn)
{
	int rc = 0;
	struct snd_soc_codec *codec = mbhc->codec;
	struct snd_soc_card *card = codec->component.card;

	dev_dbg(mbhc->codec->dev, "%s: gpio %s\n", __func__, gpio_dt_str);

	*gpio_dn = of_parse_phandle(card->dev->of_node, gpio_dt_str, 0);

	if (!(*gpio_dn)) {
		*gpio = of_get_named_gpio(card->dev->of_node, gpio_dt_str, 0);
		if (!gpio_is_valid(*gpio)) {
			dev_err(card->dev, "%s, property %s not in node %s",
				__func__, gpio_dt_str,
				card->dev->of_node->full_name);
			rc = -EINVAL;
		}
	}

	return rc;
}

int wcd_mbhc_start(struct wcd_mbhc *mbhc, struct wcd_mbhc_config *mbhc_cfg)
{
	int rc = 0;
	struct usbc_ana_audio_config *config;
	struct snd_soc_codec *codec;
	struct snd_soc_card *card;
	const char *usb_c_dt = "qcom,msm-mbhc-usbc-audio-supported";

	if (!mbhc || !mbhc_cfg)
		return -EINVAL;

	config = &mbhc_cfg->usbc_analog_cfg;
	codec = mbhc->codec;
	card = codec->component.card;

	/* update the mbhc config */
	mbhc->mbhc_cfg = mbhc_cfg;

	dev_dbg(mbhc->codec->dev, "%s: enter\n", __func__);

	/* check if USB C analog is defined on device tree */
	mbhc_cfg->enable_usbc_analog = 0;
	if (of_find_property(card->dev->of_node, usb_c_dt, NULL)) {
		rc = of_property_read_u32(card->dev->of_node, usb_c_dt,
				&mbhc_cfg->enable_usbc_analog);
	}
	if (mbhc_cfg->enable_usbc_analog == 0 || rc != 0) {
		dev_info(card->dev,
				"%s: %s in dt node is missing or false\n",
				__func__, usb_c_dt);
		dev_info(card->dev,
			"%s: skipping USB c analog configuration\n", __func__);
	}

	/* initialize GPIOs */
	if (mbhc_cfg->enable_usbc_analog) {
		dev_dbg(mbhc->codec->dev, "%s: usbc analog enabled\n",
				__func__);
		rc = wcd_mbhc_init_gpio(mbhc, mbhc_cfg,
				"qcom,usbc-analog-en1_gpio",
				&config->usbc_en1_gpio,
				&config->usbc_en1_gpio_p);
		if (rc)
			goto err;

		rc = wcd_mbhc_init_gpio(mbhc, mbhc_cfg,
				"qcom,usbc-analog-en2_n_gpio",
				&config->usbc_en2n_gpio,
				&config->usbc_en2n_gpio_p);
		if (rc)
			goto err;

		if (of_find_property(card->dev->of_node,
				     "qcom,usbc-analog-force_detect_gpio",
				     NULL)) {
			rc = wcd_mbhc_init_gpio(mbhc, mbhc_cfg,
					"qcom,usbc-analog-force_detect_gpio",
					&config->usbc_force_gpio,
					&config->usbc_force_gpio_p);
			if (rc)
				goto err;
		}

		dev_dbg(mbhc->codec->dev, "%s: calling usb_c_analog_init\n",
			__func__);
		/* init PMI notifier */
		rc = wcd_mbhc_usb_c_analog_init(mbhc);
		if (rc) {
			rc = EPROBE_DEFER;
			goto err;
		}
	}

	/* Set btn key code */
	if ((!mbhc->is_btn_already_regd) && wcd_mbhc_set_keycode(mbhc))
		pr_err("Set btn key code error!!!\n");

	if (!mbhc->mbhc_cfg->read_fw_bin ||
	    (mbhc->mbhc_cfg->read_fw_bin && mbhc->mbhc_fw) ||
	    (mbhc->mbhc_cfg->read_fw_bin && mbhc->mbhc_cal)) {
		rc = wcd_mbhc_initialise(mbhc);
	} else {
		if (!mbhc->mbhc_fw || !mbhc->mbhc_cal)
			schedule_delayed_work(&mbhc->mbhc_firmware_dwork,
				      usecs_to_jiffies(FW_READ_TIMEOUT));
		else
			pr_err("%s: Skipping to read mbhc fw, 0x%pK %pK\n",
				 __func__, mbhc->mbhc_fw, mbhc->mbhc_cal);
	}

	return rc;
err:
	if (config->usbc_en1_gpio > 0) {
		dev_dbg(card->dev, "%s free usb en1 gpio %d\n",
			__func__, config->usbc_en1_gpio);
		gpio_free(config->usbc_en1_gpio);
		config->usbc_en1_gpio = 0;
	}
	if (config->usbc_en2n_gpio > 0) {
		dev_dbg(card->dev, "%s free usb_en2 gpio %d\n",
			__func__, config->usbc_en2n_gpio);
		gpio_free(config->usbc_en2n_gpio);
		config->usbc_en2n_gpio = 0;
	}
	if (config->usbc_force_gpio > 0) {
		dev_dbg(card->dev, "%s free usb_force gpio %d\n",
			__func__, config->usbc_force_gpio);
		gpio_free(config->usbc_force_gpio);
		config->usbc_force_gpio = 0;
	}
	if (config->usbc_en1_gpio_p)
		of_node_put(config->usbc_en1_gpio_p);
	if (config->usbc_en2n_gpio_p)
		of_node_put(config->usbc_en2n_gpio_p);
	if (config->usbc_force_gpio_p)
		of_node_put(config->usbc_force_gpio_p);
	dev_dbg(mbhc->codec->dev, "%s: leave %d\n", __func__, rc);
	return rc;
}
EXPORT_SYMBOL(wcd_mbhc_start);

void wcd_mbhc_stop(struct wcd_mbhc *mbhc)
{
	struct usbc_ana_audio_config *config = &mbhc->mbhc_cfg->usbc_analog_cfg;

	pr_debug("%s: enter\n", __func__);

	if (mbhc->current_plug != MBHC_PLUG_TYPE_NONE) {
		if (mbhc->mbhc_cb && mbhc->mbhc_cb->skip_imped_detect)
			mbhc->mbhc_cb->skip_imped_detect(mbhc->codec);
	}
	mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
	mbhc->hph_status = 0;
	if (mbhc->mbhc_cb && mbhc->mbhc_cb->irq_control) {
		mbhc->mbhc_cb->irq_control(mbhc->codec,
				mbhc->intr_ids->hph_left_ocp,
				false);
		mbhc->mbhc_cb->irq_control(mbhc->codec,
				mbhc->intr_ids->hph_right_ocp,
				false);
	}
	if (mbhc->mbhc_fw || mbhc->mbhc_cal) {
		cancel_delayed_work_sync(&mbhc->mbhc_firmware_dwork);
		if (!mbhc->mbhc_cal)
			release_firmware(mbhc->mbhc_fw);
		mbhc->mbhc_fw = NULL;
		mbhc->mbhc_cal = NULL;
	}

	if (mbhc->mbhc_cfg->enable_usbc_analog) {
		wcd_mbhc_usb_c_analog_deinit(mbhc);
		/* free GPIOs */
		if (config->usbc_en1_gpio > 0)
			gpio_free(config->usbc_en1_gpio);
		if (config->usbc_en2n_gpio > 0)
			gpio_free(config->usbc_en2n_gpio);
		if (config->usbc_force_gpio)
			gpio_free(config->usbc_force_gpio);

		if (config->usbc_en1_gpio_p)
			of_node_put(config->usbc_en1_gpio_p);
		if (config->usbc_en2n_gpio_p)
			of_node_put(config->usbc_en2n_gpio_p);
		if (config->usbc_force_gpio_p)
			of_node_put(config->usbc_force_gpio_p);
	}

	pr_debug("%s: leave\n", __func__);
}
EXPORT_SYMBOL(wcd_mbhc_stop);

/* HTC_AUD_START - AS HS */
/* report unsupport if it's non HTC analog adapter */
static ssize_t headset_print_name(struct switch_dev *sdev, char *buf)
{
	return snprintf(buf, 20, "Unsupported_device\n");
}
/* HTC_AUD_END */

/*
 * wcd_mbhc_init : initialize MBHC internal structures.
 *
 * NOTE: mbhc->mbhc_cfg is not YET configure so shouldn't be used
 */
int wcd_mbhc_init(struct wcd_mbhc *mbhc, struct snd_soc_codec *codec,
		      const struct wcd_mbhc_cb *mbhc_cb,
		      const struct wcd_mbhc_intr *mbhc_cdc_intr_ids,
		      struct wcd_mbhc_register *wcd_mbhc_regs,
		      bool impedance_det_en)
{
	int ret = 0;
	int hph_swh = 0;
	int gnd_swh = 0;
	u32 hph_moist_config[3];
	struct snd_soc_card *card = codec->component.card;
	const char *hph_switch = "qcom,msm-mbhc-hphl-swh";
	const char *gnd_switch = "qcom,msm-mbhc-gnd-swh";

	pr_err("%s: enter ++\n", __func__); /* HTC_AUD */
	pr_debug("%s: enter\n", __func__);

	ret = of_property_read_u32(card->dev->of_node, hph_switch, &hph_swh);
	if (ret) {
		dev_err(card->dev,
			"%s: missing %s in dt node\n", __func__, hph_switch);
		goto err;
	}

	ret = of_property_read_u32(card->dev->of_node, gnd_switch, &gnd_swh);
	if (ret) {
		dev_err(card->dev,
			"%s: missing %s in dt node\n", __func__, gnd_switch);
		goto err;
	}

	ret = of_property_read_u32_array(card->dev->of_node,
					 "qcom,msm-mbhc-moist-cfg",
					 hph_moist_config, 3);
	if (ret) {
		dev_dbg(card->dev, "%s: no qcom,msm-mbhc-moist-cfg in DT\n",
			__func__);
		mbhc->moist_vref = V_45_MV;
		mbhc->moist_iref = I_3P0_UA;
		mbhc->moist_rref = R_24_KOHM;
	} else {
		mbhc->moist_vref = hph_moist_config[0];
		mbhc->moist_iref = hph_moist_config[1];
		mbhc->moist_rref = hph_moist_config[2];
	}

	mbhc->in_swch_irq_handler = false;
	mbhc->current_plug = MBHC_PLUG_TYPE_NONE;
	mbhc->is_btn_press = false;
	mbhc->codec = codec;
	mbhc->intr_ids = mbhc_cdc_intr_ids;
	mbhc->impedance_detect = impedance_det_en;
	mbhc->hphl_swh = hph_swh;
	mbhc->gnd_swh = gnd_swh;
	mbhc->micbias_enable = false;
	mbhc->mbhc_cb = mbhc_cb;
	mbhc->btn_press_intr = false;
	mbhc->is_hs_recording = false;
	mbhc->is_extn_cable = false;
	mbhc->hph_type = WCD_MBHC_HPH_NONE;
	mbhc->wcd_mbhc_regs = wcd_mbhc_regs;

	if (mbhc->intr_ids == NULL) {
		pr_err("%s: Interrupt mapping not provided\n", __func__);
		return -EINVAL;
	}
	if (!mbhc->wcd_mbhc_regs) {
		dev_err(codec->dev, "%s: mbhc registers are not defined\n",
			__func__);
		return -EINVAL;
	}

	/* Check if IRQ and other required callbacks are defined or not */
	if (!mbhc_cb || !mbhc_cb->request_irq || !mbhc_cb->irq_control ||
	    !mbhc_cb->free_irq || !mbhc_cb->map_btn_code_to_num ||
	    !mbhc_cb->lock_sleep || !mbhc_cb->mbhc_bias ||
	    !mbhc_cb->set_btn_thr) {
		dev_err(codec->dev, "%s: required mbhc callbacks are not defined\n",
			__func__);
		return -EINVAL;
	}

	if (mbhc->headset_jack.jack == NULL) {
		ret = snd_soc_card_jack_new(codec->component.card,
					    "Headset Jack", WCD_MBHC_JACK_MASK,
					    &mbhc->headset_jack, NULL, 0);
		if (ret) {
			pr_err("%s: Failed to create new jack\n", __func__);
			return ret;
		}

		ret = snd_soc_card_jack_new(codec->component.card,
					    "Button Jack",
					    WCD_MBHC_JACK_BUTTON_MASK,
					    &mbhc->button_jack, NULL, 0);
		if (ret) {
			pr_err("Failed to create new jack\n");
			return ret;
		}

		ret = snd_jack_set_key(mbhc->button_jack.jack,
				       SND_JACK_BTN_0,
				       KEY_MEDIA);
		if (ret) {
			pr_err("%s: Failed to set code for btn-0\n",
				__func__);
			return ret;
		}

		set_bit(INPUT_PROP_NO_DUMMY_RELEASE,
			mbhc->button_jack.jack->input_dev->propbit);

		INIT_DELAYED_WORK(&mbhc->mbhc_firmware_dwork,
				  wcd_mbhc_fw_read);
		INIT_DELAYED_WORK(&mbhc->mbhc_btn_dwork, wcd_btn_lpress_fn);
	}
	mutex_init(&mbhc->hphl_pa_lock);
	mutex_init(&mbhc->hphr_pa_lock);

	/* Register event notifier */
	mbhc->nblock.notifier_call = wcd_event_notify;
	if (mbhc->mbhc_cb->register_notifier) {
		ret = mbhc->mbhc_cb->register_notifier(mbhc, &mbhc->nblock,
						       true);
		if (ret) {
			pr_err("%s: Failed to register notifier %d\n",
				__func__, ret);
			return ret;
		}
	}

	init_waitqueue_head(&mbhc->wait_btn_press);
	mutex_init(&mbhc->codec_resource_lock);

	ret = mbhc->mbhc_cb->request_irq(codec, mbhc->intr_ids->mbhc_sw_intr,
				  wcd_mbhc_mech_plug_detect_irq,
				  "mbhc sw intr", mbhc);
	if (ret) {
		pr_err("%s: Failed to request irq %d, ret = %d\n", __func__,
		       mbhc->intr_ids->mbhc_sw_intr, ret);
		goto err_mbhc_sw_irq;
	}

	ret = mbhc->mbhc_cb->request_irq(codec,
					 mbhc->intr_ids->mbhc_btn_press_intr,
					 wcd_mbhc_btn_press_handler,
					 "Button Press detect",
					 mbhc);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->mbhc_btn_press_intr);
		goto err_btn_press_irq;
	}

	ret = mbhc->mbhc_cb->request_irq(codec,
					 mbhc->intr_ids->mbhc_btn_release_intr,
					 wcd_mbhc_release_handler,
					 "Button Release detect", mbhc);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
			mbhc->intr_ids->mbhc_btn_release_intr);
		goto err_btn_release_irq;
	}

	ret = mbhc->mbhc_cb->request_irq(codec,
					 mbhc->intr_ids->mbhc_hs_ins_intr,
					 wcd_mbhc_hs_ins_irq,
					 "Elect Insert", mbhc);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->mbhc_hs_ins_intr);
		goto err_mbhc_hs_ins_irq;
	}
	mbhc->mbhc_cb->irq_control(codec, mbhc->intr_ids->mbhc_hs_ins_intr,
				   false);
	clear_bit(WCD_MBHC_ELEC_HS_INS, &mbhc->intr_status);

	ret = mbhc->mbhc_cb->request_irq(codec,
					 mbhc->intr_ids->mbhc_hs_rem_intr,
					 wcd_mbhc_hs_rem_irq,
					 "Elect Remove", mbhc);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->mbhc_hs_rem_intr);
		goto err_mbhc_hs_rem_irq;
	}
	mbhc->mbhc_cb->irq_control(codec, mbhc->intr_ids->mbhc_hs_rem_intr,
				   false);
	clear_bit(WCD_MBHC_ELEC_HS_REM, &mbhc->intr_status);

	ret = mbhc->mbhc_cb->request_irq(codec, mbhc->intr_ids->hph_left_ocp,
				  wcd_mbhc_hphl_ocp_irq, "HPH_L OCP detect",
				  mbhc);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->hph_left_ocp);
		goto err_hphl_ocp_irq;
	}

	ret = mbhc->mbhc_cb->request_irq(codec, mbhc->intr_ids->hph_right_ocp,
				  wcd_mbhc_hphr_ocp_irq, "HPH_R OCP detect",
				  mbhc);
	if (ret) {
		pr_err("%s: Failed to request irq %d\n", __func__,
		       mbhc->intr_ids->hph_right_ocp);
		goto err_hphr_ocp_irq;
	}

/* HTC_AUD_START - AS HS */
/* report unsupport if it's non HTC analog adapter */
        mbhc->unsupported_type.name = "Unsupported_device";
        mbhc->unsupported_type.print_name = headset_print_name;

        ret = switch_dev_register(&mbhc->unsupported_type);
        if (ret < 0) {
            pr_err("failed to register headset switch device!\n");
        }
/* HTC_AUD_END */

/* HTC_AUD_START - for simulate */
	ret = register_attributes(mbhc);
	if (ret)
		pr_err("%s: register debug attributes fail errcode = %d\n", __func__, ret);

	__MBHC = mbhc;
	__MBHC->debug_reg_count = 0;
/* HTC_AUD_END */

	pr_err("%s: ret %d --\n", __func__, ret); /* HTC_AUD */
	pr_debug("%s: leave ret %d\n", __func__, ret);
	return ret;

err_hphr_ocp_irq:
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->hph_left_ocp, mbhc);
err_hphl_ocp_irq:
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_hs_rem_intr, mbhc);
err_mbhc_hs_rem_irq:
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_hs_ins_intr, mbhc);
err_mbhc_hs_ins_irq:
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_btn_release_intr,
				mbhc);
err_btn_release_irq:
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_btn_press_intr,
				mbhc);
err_btn_press_irq:
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_sw_intr, mbhc);
err_mbhc_sw_irq:
	if (mbhc->mbhc_cb->register_notifier)
		mbhc->mbhc_cb->register_notifier(mbhc, &mbhc->nblock, false);
	mutex_destroy(&mbhc->codec_resource_lock);
err:
	pr_debug("%s: leave ret %d\n", __func__, ret);
	return ret;
}
EXPORT_SYMBOL(wcd_mbhc_init);

void wcd_mbhc_deinit(struct wcd_mbhc *mbhc)
{
	struct snd_soc_codec *codec = mbhc->codec;

	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_sw_intr, mbhc);
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_btn_press_intr,
				mbhc);
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_btn_release_intr,
				mbhc);
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_hs_ins_intr, mbhc);
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->mbhc_hs_rem_intr, mbhc);
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->hph_left_ocp, mbhc);
	mbhc->mbhc_cb->free_irq(codec, mbhc->intr_ids->hph_right_ocp, mbhc);
	if (mbhc->mbhc_cb && mbhc->mbhc_cb->register_notifier)
		mbhc->mbhc_cb->register_notifier(mbhc, &mbhc->nblock, false);
	wcd_cancel_hs_detect_plug(mbhc, &mbhc->correct_plug_swch);
	unregister_attributes(mbhc); /* HTC_AUD */
	mutex_destroy(&mbhc->codec_resource_lock);
	mutex_destroy(&mbhc->hphl_pa_lock);
	mutex_destroy(&mbhc->hphr_pa_lock);
}
EXPORT_SYMBOL(wcd_mbhc_deinit);

MODULE_DESCRIPTION("wcd MBHC v2 module");
MODULE_LICENSE("GPL v2");