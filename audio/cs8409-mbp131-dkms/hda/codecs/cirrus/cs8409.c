// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HD audio codec driver for Cirrus Logic CS8409 HDA bridge chip
 *
 * Copyright (C) 2021 Cirrus Logic, Inc. and
 *                    Cirrus Logic International Semiconductor Ltd.
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/cleanup.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <sound/core.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/iopoll.h>

#include "cs8409.h"
#include "../side-codecs/hda_component.h"

/******************************************************************************
 *                        CS8409 Specific Functions
 ******************************************************************************/

static int cs8409_parse_auto_config(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	int err;
	int i;

	err = snd_hda_parse_pin_defcfg(codec, &spec->gen.autocfg, NULL, 0);
	if (err < 0)
		return err;

	err = snd_hda_gen_parse_auto_config(codec, &spec->gen.autocfg);
	if (err < 0)
		return err;

	/* keep the ADCs powered up when it's dynamically switchable */
	if (spec->gen.dyn_adc_switch) {
		unsigned int done = 0;

		for (i = 0; i < spec->gen.input_mux.num_items; i++) {
			int idx = spec->gen.dyn_adc_idx[i];

			if (done & (1 << idx))
				continue;
			snd_hda_gen_fix_pin_power(codec, spec->gen.adc_nids[idx]);
			done |= 1 << idx;
		}
	}

	return 0;
}

static void cs8409_disable_i2c_clock_worker(struct work_struct *work);

static void cs8409_irq_sync_worker(struct work_struct *work)
{
	struct cs8409_spec *spec = container_of(work, struct cs8409_spec, irq_sync_work.work);

	if (spec->irq_sync)
		spec->irq_sync(spec->codec);
}

static struct cs8409_spec *cs8409_alloc_spec(struct hda_codec *codec)
{
	struct cs8409_spec *spec;

	spec = kzalloc_obj(*spec);
	if (!spec)
		return NULL;
	codec->spec = spec;
	spec->codec = codec;
	codec->power_save_node = 1;
	mutex_init(&spec->i2c_mux);
	mutex_init(&spec->jack_lock);
	INIT_DELAYED_WORK(&spec->i2c_clk_work, cs8409_disable_i2c_clock_worker);
	INIT_DELAYED_WORK(&spec->irq_sync_work, cs8409_irq_sync_worker);
	snd_hda_gen_spec_init(&spec->gen);

	return spec;
}

static inline int cs8409_vendor_coef_get(struct hda_codec *codec, unsigned int idx)
{
	snd_hda_codec_write(codec, CS8409_PIN_VENDOR_WIDGET, 0, AC_VERB_SET_COEF_INDEX, idx);
	return snd_hda_codec_read(codec, CS8409_PIN_VENDOR_WIDGET, 0, AC_VERB_GET_PROC_COEF, 0);
}

static inline void cs8409_vendor_coef_set(struct hda_codec *codec, unsigned int idx,
					  unsigned int coef)
{
	snd_hda_codec_write(codec, CS8409_PIN_VENDOR_WIDGET, 0, AC_VERB_SET_COEF_INDEX, idx);
	snd_hda_codec_write(codec, CS8409_PIN_VENDOR_WIDGET, 0, AC_VERB_SET_PROC_COEF, coef);
}

/*
 * cs8409_enable_i2c_clock - Disable I2C clocks
 * @codec: the codec instance
 * Disable I2C clocks.
 * This must be called when the i2c mutex is unlocked.
 */
static void cs8409_disable_i2c_clock(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;

	guard(mutex)(&spec->i2c_mux);
	if (spec->i2c_clck_enabled) {
		cs8409_vendor_coef_set(spec->codec, 0x0,
			       cs8409_vendor_coef_get(spec->codec, 0x0) & 0xfffffff7);
		spec->i2c_clck_enabled = 0;
	}
}

/*
 * cs8409_disable_i2c_clock_worker - Worker that disable the I2C Clock after 25ms without use
 */
static void cs8409_disable_i2c_clock_worker(struct work_struct *work)
{
	struct cs8409_spec *spec = container_of(work, struct cs8409_spec, i2c_clk_work.work);

	cs8409_disable_i2c_clock(spec->codec);
}

/*
 * cs8409_enable_i2c_clock - Enable I2C clocks
 * @codec: the codec instance
 * Enable I2C clocks.
 * This must be called when the i2c mutex is locked.
 */
static void cs8409_enable_i2c_clock(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;

	/* Cancel the disable timer, but do not wait for any running disable functions to finish.
	 * If the disable timer runs out before cancel, the delayed work thread will be blocked,
	 * waiting for the mutex to become unlocked. This mutex will be locked for the duration of
	 * any i2c transaction, so the disable function will run to completion immediately
	 * afterwards in the scenario. The next enable call will re-enable the clock, regardless.
	 */
	cancel_delayed_work(&spec->i2c_clk_work);

	if (!spec->i2c_clck_enabled) {
		cs8409_vendor_coef_set(codec, 0x0, cs8409_vendor_coef_get(codec, 0x0) | 0x8);
		spec->i2c_clck_enabled = 1;
	}
	queue_delayed_work(system_power_efficient_wq, &spec->i2c_clk_work, msecs_to_jiffies(25));
}

/**
 * cs8409_i2c_wait_complete - Wait for I2C transaction
 * @codec: the codec instance
 *
 * Wait for I2C transaction to complete.
 * Return -ETIMEDOUT if transaction wait times out.
 */
static int cs8409_i2c_wait_complete(struct hda_codec *codec)
{
	unsigned int retval;

	return read_poll_timeout(cs8409_vendor_coef_get, retval, retval & 0x18,
		CS42L42_I2C_SLEEP_US, CS42L42_I2C_TIMEOUT_US, false, codec, CS8409_I2C_STS);
}

/**
 * cs8409_set_i2c_dev_addr - Set i2c address for transaction
 * @codec: the codec instance
 * @addr: I2C Address
 */
static void cs8409_set_i2c_dev_addr(struct hda_codec *codec, unsigned int addr)
{
	struct cs8409_spec *spec = codec->spec;

	if (spec->dev_addr != addr) {
		cs8409_vendor_coef_set(codec, CS8409_I2C_ADDR, addr);
		spec->dev_addr = addr;
	}
}

/**
 * cs8409_i2c_set_page - CS8409 I2C set page register.
 * @scodec: the codec instance
 * @i2c_reg: Page register
 *
 * Returns negative on error.
 */
static int cs8409_i2c_set_page(struct sub_codec *scodec, unsigned int i2c_reg)
{
	struct hda_codec *codec = scodec->codec;

	if (scodec->paged && (scodec->last_page != (i2c_reg >> 8))) {
		cs8409_vendor_coef_set(codec, CS8409_I2C_QWRITE, i2c_reg >> 8);
		if (cs8409_i2c_wait_complete(codec) < 0)
			return -EIO;
		scodec->last_page = i2c_reg >> 8;
	}

	return 0;
}

/**
 * cs8409_i2c_read - CS8409 I2C Read.
 * @scodec: the codec instance
 * @addr: Register to read
 *
 * Returns negative on error, otherwise returns read value in bits 0-7.
 */
static int cs8409_i2c_read(struct sub_codec *scodec, unsigned int addr)
{
	struct hda_codec *codec = scodec->codec;
	struct cs8409_spec *spec = codec->spec;
	unsigned int i2c_reg_data;
	unsigned int read_data;

	if (scodec->suspended)
		return -EPERM;

	guard(mutex)(&spec->i2c_mux);
	cs8409_enable_i2c_clock(codec);
	cs8409_set_i2c_dev_addr(codec, scodec->addr);

	if (cs8409_i2c_set_page(scodec, addr))
		goto error;

	i2c_reg_data = (addr << 8) & 0x0ffff;
	cs8409_vendor_coef_set(codec, CS8409_I2C_QREAD, i2c_reg_data);
	if (cs8409_i2c_wait_complete(codec) < 0)
		goto error;

	/* Register in bits 15-8 and the data in 7-0 */
	read_data = cs8409_vendor_coef_get(codec, CS8409_I2C_QREAD);

	return read_data & 0x0ff;

error:
	codec_err(codec, "%s() Failed 0x%02x : 0x%04x\n", __func__, scodec->addr, addr);
	return -EIO;
}

/**
 * cs8409_i2c_bulk_read - CS8409 I2C Read Sequence.
 * @scodec: the codec instance
 * @seq: Register Sequence to read
 * @count: Number of registeres to read
 *
 * Returns negative on error, values are read into value element of cs8409_i2c_param sequence.
 */
static int cs8409_i2c_bulk_read(struct sub_codec *scodec, struct cs8409_i2c_param *seq, int count)
{
	struct hda_codec *codec = scodec->codec;
	struct cs8409_spec *spec = codec->spec;
	unsigned int i2c_reg_data;
	int i;

	if (scodec->suspended)
		return -EPERM;

	guard(mutex)(&spec->i2c_mux);
	cs8409_set_i2c_dev_addr(codec, scodec->addr);

	for (i = 0; i < count; i++) {
		cs8409_enable_i2c_clock(codec);
		if (cs8409_i2c_set_page(scodec, seq[i].addr))
			goto error;

		i2c_reg_data = (seq[i].addr << 8) & 0x0ffff;
		cs8409_vendor_coef_set(codec, CS8409_I2C_QREAD, i2c_reg_data);

		if (cs8409_i2c_wait_complete(codec) < 0)
			goto error;

		seq[i].value = cs8409_vendor_coef_get(codec, CS8409_I2C_QREAD) & 0xff;
	}

	return 0;

error:
	codec_err(codec, "I2C Bulk Read Failed 0x%02x\n", scodec->addr);
	return -EIO;
}

/**
 * cs8409_i2c_write - CS8409 I2C Write.
 * @scodec: the codec instance
 * @addr: Register to write to
 * @value: Data to write
 *
 * Returns negative on error, otherwise returns 0.
 */
static int cs8409_i2c_write(struct sub_codec *scodec, unsigned int addr, unsigned int value)
{
	struct hda_codec *codec = scodec->codec;
	struct cs8409_spec *spec = codec->spec;
	unsigned int i2c_reg_data;

	if (scodec->suspended)
		return -EPERM;

	guard(mutex)(&spec->i2c_mux);

	cs8409_enable_i2c_clock(codec);
	cs8409_set_i2c_dev_addr(codec, scodec->addr);

	if (cs8409_i2c_set_page(scodec, addr))
		goto error;

	i2c_reg_data = ((addr << 8) & 0x0ff00) | (value & 0x0ff);
	cs8409_vendor_coef_set(codec, CS8409_I2C_QWRITE, i2c_reg_data);

	if (cs8409_i2c_wait_complete(codec) < 0)
		goto error;

	return 0;

error:
	codec_err(codec, "%s() Failed 0x%02x : 0x%04x\n", __func__, scodec->addr, addr);
	return -EIO;
}

/**
 * cs8409_i2c_bulk_write - CS8409 I2C Write Sequence.
 * @scodec: the codec instance
 * @seq: Register Sequence to write
 * @count: Number of registeres to write
 *
 * Returns negative on error.
 */
static int cs8409_i2c_bulk_write(struct sub_codec *scodec, const struct cs8409_i2c_param *seq,
				 int count)
{
	struct hda_codec *codec = scodec->codec;
	struct cs8409_spec *spec = codec->spec;
	unsigned int i2c_reg_data;
	int i;

	if (scodec->suspended)
		return -EPERM;

	guard(mutex)(&spec->i2c_mux);
	cs8409_set_i2c_dev_addr(codec, scodec->addr);

	for (i = 0; i < count; i++) {
		cs8409_enable_i2c_clock(codec);
		if (cs8409_i2c_set_page(scodec, seq[i].addr))
			goto error;

		i2c_reg_data = ((seq[i].addr << 8) & 0x0ff00) | (seq[i].value & 0x0ff);
		cs8409_vendor_coef_set(codec, CS8409_I2C_QWRITE, i2c_reg_data);

		if (cs8409_i2c_wait_complete(codec) < 0)
			goto error;
		/* Certain use cases may require a delay
		 * after a write operation before proceeding.
		 */
		if (seq[i].delay)
			fsleep(seq[i].delay);
	}

	return 0;

error:
	codec_err(codec, "I2C Bulk Write Failed 0x%02x\n", scodec->addr);
	return -EIO;
}

static int cs8409_init(struct hda_codec *codec)
{
	int ret = snd_hda_gen_init(codec);

	if (!ret)
		snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_INIT);

	return ret;
}

static int cs8409_build_controls(struct hda_codec *codec)
{
	int err;

	err = snd_hda_gen_build_controls(codec);
	if (err < 0)
		return err;
	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_BUILD);

	return 0;
}

/* Enable/Disable Unsolicited Response */
static void cs8409_enable_ur(struct hda_codec *codec, int flag)
{
	struct cs8409_spec *spec = codec->spec;
	unsigned int ur_gpios = 0;
	int i;

	for (i = 0; i < spec->num_scodecs; i++)
		ur_gpios |= spec->scodecs[i]->irq_mask;

	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_UNSOLICITED_RSP_MASK,
			    flag ? ur_gpios : 0);

	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_UNSOLICITED_ENABLE,
			    flag ? AC_UNSOL_ENABLED : 0);
}

static void cs8409_fix_caps(struct hda_codec *codec, unsigned int nid)
{
	int caps;

	/* CS8409 is simple HDA bridge and intended to be used with a remote
	 * companion codec. Most of input/output PIN(s) have only basic
	 * capabilities. Receive and Transmit NID(s) have only OUTC and INC
	 * capabilities and no presence detect capable (PDC) and call to
	 * snd_hda_gen_build_controls() will mark them as non detectable
	 * phantom jacks. However, a companion codec may be
	 * connected to these pins which supports jack detect
	 * capabilities. We have to override pin capabilities,
	 * otherwise they will not be created as input devices.
	 */
	caps = snd_hdac_read_parm(&codec->core, nid, AC_PAR_PIN_CAP);
	if (caps >= 0)
		snd_hdac_override_parm(&codec->core, nid, AC_PAR_PIN_CAP,
				       (caps | (AC_PINCAP_IMP_SENSE | AC_PINCAP_PRES_DETECT)));

	snd_hda_override_wcaps(codec, nid, (get_wcaps(codec, nid) | AC_WCAP_UNSOL_CAP));
}

static int cs8409_spk_sw_gpio_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cs8409_spec *spec = codec->spec;

	ucontrol->value.integer.value[0] = !!(spec->gpio_data & spec->speaker_pdn_gpio);
	return 0;
}

static int cs8409_spk_sw_gpio_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cs8409_spec *spec = codec->spec;
	unsigned int gpio_data;

	gpio_data = (spec->gpio_data & ~spec->speaker_pdn_gpio) |
		(ucontrol->value.integer.value[0] ? spec->speaker_pdn_gpio : 0);
	if (gpio_data == spec->gpio_data)
		return 0;
	spec->gpio_data = gpio_data;
	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DATA, spec->gpio_data);
	return 1;
}

static const struct snd_kcontrol_new cs8409_spk_sw_ctrl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.info = snd_ctl_boolean_mono_info,
	.get = cs8409_spk_sw_gpio_get,
	.put = cs8409_spk_sw_gpio_put,
};

/******************************************************************************
 *                        CS42L42 Specific Functions
 ******************************************************************************/

int cs42l42_volume_info(struct snd_kcontrol *kctrl, struct snd_ctl_elem_info *uinfo)
{
	unsigned int ofs = get_amp_offset(kctrl);
	u8 chs = get_amp_channels(kctrl);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->value.integer.step = 1;
	uinfo->count = chs == 3 ? 2 : 1;

	switch (ofs) {
	case CS42L42_VOL_DAC:
		uinfo->value.integer.min = CS42L42_HP_VOL_REAL_MIN;
		uinfo->value.integer.max = CS42L42_HP_VOL_REAL_MAX;
		break;
	case CS42L42_VOL_ADC:
		uinfo->value.integer.min = CS42L42_AMIC_VOL_REAL_MIN;
		uinfo->value.integer.max = CS42L42_AMIC_VOL_REAL_MAX;
		break;
	default:
		break;
	}

	return 0;
}

int cs42l42_volume_get(struct snd_kcontrol *kctrl, struct snd_ctl_elem_value *uctrl)
{
	struct hda_codec *codec = snd_kcontrol_chip(kctrl);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[get_amp_index(kctrl)];
	int chs = get_amp_channels(kctrl);
	unsigned int ofs = get_amp_offset(kctrl);
	long *valp = uctrl->value.integer.value;

	switch (ofs) {
	case CS42L42_VOL_DAC:
		if (chs & BIT(0))
			*valp++ = cs42l42->vol[ofs];
		if (chs & BIT(1))
			*valp = cs42l42->vol[ofs+1];
		break;
	case CS42L42_VOL_ADC:
		if (chs & BIT(0))
			*valp = cs42l42->vol[ofs];
		break;
	default:
		break;
	}

	return 0;
}

static void cs42l42_mute(struct sub_codec *cs42l42, int vol_type,
	unsigned int chs, bool mute)
{
	if (mute) {
		if (vol_type == CS42L42_VOL_DAC) {
			if (chs & BIT(0))
				cs8409_i2c_write(cs42l42, CS42L42_MIXER_CHA_VOL, 0x3f);
			if (chs & BIT(1))
				cs8409_i2c_write(cs42l42, CS42L42_MIXER_CHB_VOL, 0x3f);
		} else if (vol_type == CS42L42_VOL_ADC) {
			if (chs & BIT(0))
				cs8409_i2c_write(cs42l42, CS42L42_ADC_VOLUME, 0x9f);
		}
	} else {
		if (vol_type == CS42L42_VOL_DAC) {
			if (chs & BIT(0))
				cs8409_i2c_write(cs42l42, CS42L42_MIXER_CHA_VOL,
					-(cs42l42->vol[CS42L42_DAC_CH0_VOL_OFFSET])
					& CS42L42_MIXER_CH_VOL_MASK);
			if (chs & BIT(1))
				cs8409_i2c_write(cs42l42, CS42L42_MIXER_CHB_VOL,
					-(cs42l42->vol[CS42L42_DAC_CH1_VOL_OFFSET])
					& CS42L42_MIXER_CH_VOL_MASK);
		} else if (vol_type == CS42L42_VOL_ADC) {
			if (chs & BIT(0))
				cs8409_i2c_write(cs42l42, CS42L42_ADC_VOLUME,
					cs42l42->vol[CS42L42_ADC_VOL_OFFSET]
					& CS42L42_REG_AMIC_VOL_MASK);
		}
	}
}

int cs42l42_volume_put(struct snd_kcontrol *kctrl, struct snd_ctl_elem_value *uctrl)
{
	struct hda_codec *codec = snd_kcontrol_chip(kctrl);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[get_amp_index(kctrl)];
	int chs = get_amp_channels(kctrl);
	unsigned int ofs = get_amp_offset(kctrl);
	long *valp = uctrl->value.integer.value;

	switch (ofs) {
	case CS42L42_VOL_DAC:
		if (chs & BIT(0))
			cs42l42->vol[ofs] = *valp;
		if (chs & BIT(1)) {
			valp++;
			cs42l42->vol[ofs + 1] = *valp;
		}
		if (spec->playback_started)
			cs42l42_mute(cs42l42, CS42L42_VOL_DAC, chs, false);
		break;
	case CS42L42_VOL_ADC:
		if (chs & BIT(0))
			cs42l42->vol[ofs] = *valp;
		if (spec->capture_started)
			cs42l42_mute(cs42l42, CS42L42_VOL_ADC, chs, false);
		break;
	default:
		break;
	}

	return 0;
}

static void cs42l42_playback_pcm_hook(struct hda_pcm_stream *hinfo,
				   struct hda_codec *codec,
				   struct snd_pcm_substream *substream,
				   int action)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42;
	int i;
	bool mute;

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		mute = false;
		spec->playback_started = 1;
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		mute = true;
		spec->playback_started = 0;
		break;
	default:
		return;
	}

	for (i = 0; i < spec->num_scodecs; i++) {
		cs42l42 = spec->scodecs[i];
		cs42l42_mute(cs42l42, CS42L42_VOL_DAC, 0x3, mute);
	}
}

static void cs42l42_capture_pcm_hook(struct hda_pcm_stream *hinfo,
				   struct hda_codec *codec,
				   struct snd_pcm_substream *substream,
				   int action)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42;
	int i;
	bool mute;

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		mute = false;
		spec->capture_started = 1;
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		mute = true;
		spec->capture_started = 0;
		break;
	default:
		return;
	}

	for (i = 0; i < spec->num_scodecs; i++) {
		cs42l42 = spec->scodecs[i];
		cs42l42_mute(cs42l42, CS42L42_VOL_ADC, 0x3, mute);
	}
}

/* Configure CS42L42 slave codec for jack autodetect */
static void cs42l42_enable_jack_detect(struct sub_codec *cs42l42)
{
	cs8409_i2c_write(cs42l42, CS42L42_HSBIAS_SC_AUTOCTL, cs42l42->hsbias_hiz);
	/* Clear WAKE# */
	cs8409_i2c_write(cs42l42, CS42L42_WAKE_CTL, 0x00C1);
	/* Wait ~2.5ms */
	usleep_range(2500, 3000);
	/* Set mode WAKE# output follows the combination logic directly */
	cs8409_i2c_write(cs42l42, CS42L42_WAKE_CTL, 0x00C0);
	/* Clear interrupts status */
	cs8409_i2c_read(cs42l42, CS42L42_TSRS_PLUG_STATUS);
	/* Enable interrupt */
	cs8409_i2c_write(cs42l42, CS42L42_TSRS_PLUG_INT_MASK, 0xF3);
}

/* Enable and run CS42L42 slave codec jack auto detect */
static void cs42l42_run_jack_detect(struct sub_codec *cs42l42)
{
	/* Clear interrupts */
	cs8409_i2c_read(cs42l42, CS42L42_CODEC_STATUS);
	cs8409_i2c_read(cs42l42, CS42L42_DET_STATUS1);
	cs8409_i2c_write(cs42l42, CS42L42_TSRS_PLUG_INT_MASK, 0xFF);
	cs8409_i2c_read(cs42l42, CS42L42_TSRS_PLUG_STATUS);

	cs8409_i2c_write(cs42l42, CS42L42_PWR_CTL2, 0x87);
	cs8409_i2c_write(cs42l42, CS42L42_DAC_CTL2, 0x86);
	cs8409_i2c_write(cs42l42, CS42L42_MISC_DET_CTL, 0x07);
	cs8409_i2c_write(cs42l42, CS42L42_CODEC_INT_MASK, 0xFD);
	cs8409_i2c_write(cs42l42, CS42L42_HSDET_CTL2, 0x80);
	/* Wait ~20ms*/
	usleep_range(20000, 25000);
	cs8409_i2c_write(cs42l42, CS42L42_HSDET_CTL1, 0x77);
	cs8409_i2c_write(cs42l42, CS42L42_HSDET_CTL2, 0xc0);
}

static int cs42l42_manual_hs_det(struct sub_codec *cs42l42)
{
	unsigned int hs_det_status;
	unsigned int hs_det_comp1;
	unsigned int hs_det_comp2;
	unsigned int hs_det_sw;
	unsigned int hs_type;

	/* Set hs detect to manual, active mode */
	cs8409_i2c_write(cs42l42, CS42L42_HSDET_CTL2,
			 (1 << CS42L42_HSDET_CTRL_SHIFT) |
			 (0 << CS42L42_HSDET_SET_SHIFT) |
			 (0 << CS42L42_HSBIAS_REF_SHIFT) |
			 (0 << CS42L42_HSDET_AUTO_TIME_SHIFT));

	/* Configure HS DET comparator reference levels. */
	cs8409_i2c_write(cs42l42, CS42L42_HSDET_CTL1,
			 (CS42L42_HSDET_COMP1_LVL_VAL << CS42L42_HSDET_COMP1_LVL_SHIFT) |
			 (CS42L42_HSDET_COMP2_LVL_VAL << CS42L42_HSDET_COMP2_LVL_SHIFT));

	/* Open the SW_HSB_HS3 switch and close SW_HSB_HS4 for a Type 1 headset. */
	cs8409_i2c_write(cs42l42, CS42L42_HS_SWITCH_CTL, CS42L42_HSDET_SW_COMP1);

	msleep(100);

	hs_det_status = cs8409_i2c_read(cs42l42, CS42L42_HS_DET_STATUS);

	hs_det_comp1 = (hs_det_status & CS42L42_HSDET_COMP1_OUT_MASK) >>
			CS42L42_HSDET_COMP1_OUT_SHIFT;
	hs_det_comp2 = (hs_det_status & CS42L42_HSDET_COMP2_OUT_MASK) >>
			CS42L42_HSDET_COMP2_OUT_SHIFT;

	/* Close the SW_HSB_HS3 switch for a Type 2 headset. */
	cs8409_i2c_write(cs42l42, CS42L42_HS_SWITCH_CTL, CS42L42_HSDET_SW_COMP2);

	msleep(100);

	hs_det_status = cs8409_i2c_read(cs42l42, CS42L42_HS_DET_STATUS);

	hs_det_comp1 |= ((hs_det_status & CS42L42_HSDET_COMP1_OUT_MASK) >>
			CS42L42_HSDET_COMP1_OUT_SHIFT) << 1;
	hs_det_comp2 |= ((hs_det_status & CS42L42_HSDET_COMP2_OUT_MASK) >>
			CS42L42_HSDET_COMP2_OUT_SHIFT) << 1;

	/* Use Comparator 1 with 1.25V Threshold. */
	switch (hs_det_comp1) {
	case CS42L42_HSDET_COMP_TYPE1:
		hs_type = CS42L42_PLUG_CTIA;
		hs_det_sw = CS42L42_HSDET_SW_TYPE1;
		break;
	case CS42L42_HSDET_COMP_TYPE2:
		hs_type = CS42L42_PLUG_OMTP;
		hs_det_sw = CS42L42_HSDET_SW_TYPE2;
		break;
	default:
		/* Fallback to Comparator 2 with 1.75V Threshold. */
		switch (hs_det_comp2) {
		case CS42L42_HSDET_COMP_TYPE1:
			hs_type = CS42L42_PLUG_CTIA;
			hs_det_sw = CS42L42_HSDET_SW_TYPE1;
			break;
		case CS42L42_HSDET_COMP_TYPE2:
			hs_type = CS42L42_PLUG_OMTP;
			hs_det_sw = CS42L42_HSDET_SW_TYPE2;
			break;
		case CS42L42_HSDET_COMP_TYPE3:
			hs_type = CS42L42_PLUG_HEADPHONE;
			hs_det_sw = CS42L42_HSDET_SW_TYPE3;
			break;
		default:
			hs_type = CS42L42_PLUG_INVALID;
			hs_det_sw = CS42L42_HSDET_SW_TYPE4;
			break;
		}
	}

	/* Set Switches */
	cs8409_i2c_write(cs42l42, CS42L42_HS_SWITCH_CTL, hs_det_sw);

	/* Set HSDET mode to Manual—Disabled */
	cs8409_i2c_write(cs42l42, CS42L42_HSDET_CTL2,
			 (0 << CS42L42_HSDET_CTRL_SHIFT) |
			 (0 << CS42L42_HSDET_SET_SHIFT) |
			 (0 << CS42L42_HSBIAS_REF_SHIFT) |
			 (0 << CS42L42_HSDET_AUTO_TIME_SHIFT));

	/* Configure HS DET comparator reference levels. */
	cs8409_i2c_write(cs42l42, CS42L42_HSDET_CTL1,
			 (CS42L42_HSDET_COMP1_LVL_DEFAULT << CS42L42_HSDET_COMP1_LVL_SHIFT) |
			 (CS42L42_HSDET_COMP2_LVL_DEFAULT << CS42L42_HSDET_COMP2_LVL_SHIFT));

	return hs_type;
}

static int cs42l42_handle_tip_sense(struct sub_codec *cs42l42, unsigned int reg_ts_status)
{
	int status_changed = 0;

	/* TIP_SENSE INSERT/REMOVE */
	switch (reg_ts_status) {
	case CS42L42_TS_PLUG:
		/*
		 * With a rise debounce the status of a plug that was in at the
		 * init gets here after the detection run from the init.
		 */
		if (cs42l42->ts_rise_dbnc && cs42l42->hp_jack_in)
			break;
		if (cs42l42->no_type_dect) {
			status_changed = 1;
			cs42l42->hp_jack_in = 1;
			cs42l42->mic_jack_in = 0;
		} else {
			cs42l42_run_jack_detect(cs42l42);
		}
		break;

	case CS42L42_TS_UNPLUG:
		status_changed = 1;
		cs42l42->hp_jack_in = 0;
		cs42l42->mic_jack_in = 0;
		break;
	default:
		/* jack in transition */
		break;
	}

	codec_dbg(cs42l42->codec, "Tip Sense Detection: (%d)\n", reg_ts_status);

	return status_changed;
}

static int cs42l42_jack_unsol_event(struct sub_codec *cs42l42)
{
	int current_plug_status;
	int status_changed = 0;
	int reg_cdc_status;
	int reg_hs_status;
	int reg_ts_status;
	int type;

	/* Read jack detect status registers */
	reg_cdc_status = cs8409_i2c_read(cs42l42, CS42L42_CODEC_STATUS);
	reg_hs_status = cs8409_i2c_read(cs42l42, CS42L42_HS_DET_STATUS);
	reg_ts_status = cs8409_i2c_read(cs42l42, CS42L42_TSRS_PLUG_STATUS);

	/* If status values are < 0, read error has occurred. */
	if (reg_cdc_status < 0 || reg_hs_status < 0 || reg_ts_status < 0)
		return -EIO;

	current_plug_status = (reg_ts_status & (CS42L42_TS_PLUG_MASK | CS42L42_TS_UNPLUG_MASK))
				>> CS42L42_TS_PLUG_SHIFT;

	/* HSDET_AUTO_DONE */
	if (reg_cdc_status & CS42L42_HSDET_AUTO_DONE_MASK) {

		/* Disable HSDET_AUTO_DONE */
		cs8409_i2c_write(cs42l42, CS42L42_CODEC_INT_MASK, 0xFF);

		type = (reg_hs_status & CS42L42_HSDET_TYPE_MASK) >> CS42L42_HSDET_TYPE_SHIFT;

		/* Configure the HSDET mode. */
		cs8409_i2c_write(cs42l42, CS42L42_HSDET_CTL2, 0x80);

		if (cs42l42->no_type_dect) {
			status_changed = cs42l42_handle_tip_sense(cs42l42, current_plug_status);
		} else {
			if (type == CS42L42_PLUG_INVALID || type == CS42L42_PLUG_HEADPHONE) {
				codec_dbg(cs42l42->codec,
					  "Auto detect value not valid (%d), running manual det\n",
					  type);
				type = cs42l42_manual_hs_det(cs42l42);
			}

			switch (type) {
			case CS42L42_PLUG_CTIA:
			case CS42L42_PLUG_OMTP:
				status_changed = 1;
				cs42l42->hp_jack_in = 1;
				cs42l42->mic_jack_in = 1;
				break;
			case CS42L42_PLUG_HEADPHONE:
				status_changed = 1;
				cs42l42->hp_jack_in = 1;
				cs42l42->mic_jack_in = 0;
				break;
			default:
				status_changed = 1;
				cs42l42->hp_jack_in = 0;
				cs42l42->mic_jack_in = 0;
				break;
			}
			codec_dbg(cs42l42->codec, "Detection done (%d)\n", type);
		}

		/* Enable the HPOUT ground clamp and configure the HP pull-down */
		cs8409_i2c_write(cs42l42, CS42L42_DAC_CTL2, 0x02);
		/* Re-Enable Tip Sense Interrupt */
		cs8409_i2c_write(cs42l42, CS42L42_TSRS_PLUG_INT_MASK, 0xF3);

		/*
		 * The tip sense was masked while the detection ran. A board
		 * that will not detect again a jack it believes in must not
		 * believe in one pulled meanwhile: look once more, now that
		 * the interrupt is back.
		 */
		if (cs42l42->ts_rise_dbnc && cs42l42->hp_jack_in) {
			reg_ts_status = cs8409_i2c_read(cs42l42, CS42L42_TSRS_PLUG_STATUS);
			if (reg_ts_status >= 0 &&
			    !(reg_ts_status & (CS42L42_TS_PLUG_MASK | CS42L42_TS_UNPLUG_MASK))) {
				cs42l42->hp_jack_in = 0;
				cs42l42->mic_jack_in = 0;
			}
		}
	} else {
		status_changed = cs42l42_handle_tip_sense(cs42l42, current_plug_status);
	}

	return status_changed;
}

static void cs42l42_resume(struct sub_codec *cs42l42)
{
	struct hda_codec *codec = cs42l42->codec;
	struct cs8409_spec *spec = codec->spec;
	struct cs8409_i2c_param irq_regs[] = {
		{ CS42L42_CODEC_STATUS, 0x00 },
		{ CS42L42_DET_INT_STATUS1, 0x00 },
		{ CS42L42_DET_INT_STATUS2, 0x00 },
		{ CS42L42_TSRS_PLUG_STATUS, 0x00 },
	};
	unsigned int fsv;

	/* Bring CS42L42 out of Reset */
	spec->gpio_data = snd_hda_codec_read(codec, CS8409_PIN_AFG, 0, AC_VERB_GET_GPIO_DATA, 0);
	spec->gpio_data |= cs42l42->reset_gpio;
	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DATA, spec->gpio_data);
	usleep_range(10000, 15000);

	cs42l42->suspended = 0;

	/* Initialize CS42L42 companion codec */
	cs8409_i2c_bulk_write(cs42l42, cs42l42->init_seq, cs42l42->init_seq_num);

	/* Clear interrupts, by reading interrupt status registers */
	cs8409_i2c_bulk_read(cs42l42, irq_regs, ARRAY_SIZE(irq_regs));

	fsv = cs8409_i2c_read(cs42l42, CS42L42_HP_CTL);
	if (cs42l42->full_scale_vol) {
		// Set the full scale volume bit
		fsv |= CS42L42_FULL_SCALE_VOL_MASK;
		cs8409_i2c_write(cs42l42, CS42L42_HP_CTL, fsv);
	}
	// Unmute analog channels A and B
	fsv = (fsv & ~CS42L42_ANA_MUTE_AB);
	cs8409_i2c_write(cs42l42, CS42L42_HP_CTL, fsv);

	/* we have to explicitly allow unsol event handling even during the
	 * resume phase so that the jack event is processed properly
	 */
	snd_hda_codec_allow_unsol_events(cs42l42->codec);

	cs42l42_enable_jack_detect(cs42l42);
}

static void cs42l42_suspend(struct sub_codec *cs42l42)
{
	struct hda_codec *codec = cs42l42->codec;
	struct cs8409_spec *spec = codec->spec;
	int reg_cdc_status = 0;
	const struct cs8409_i2c_param cs42l42_pwr_down_seq[] = {
		{ CS42L42_DAC_CTL2, 0x02 },
		{ CS42L42_HS_CLAMP_DISABLE, 0x00 },
		{ CS42L42_MIXER_CHA_VOL, 0x3F },
		{ CS42L42_MIXER_ADC_VOL, 0x3F },
		{ CS42L42_MIXER_CHB_VOL, 0x3F },
		{ CS42L42_HP_CTL, 0x0D },
		{ CS42L42_ASP_RX_DAI0_EN, 0x00 },
		{ CS42L42_ASP_CLK_CFG, 0x00 },
		{ CS42L42_PWR_CTL1, 0xFE },
		{ CS42L42_PWR_CTL2, 0x8C },
		{ CS42L42_PWR_CTL1, 0xFF },
	};

	cs8409_i2c_bulk_write(cs42l42, cs42l42_pwr_down_seq, ARRAY_SIZE(cs42l42_pwr_down_seq));

	if (read_poll_timeout(cs8409_i2c_read, reg_cdc_status,
			(reg_cdc_status & 0x1), CS42L42_PDN_SLEEP_US, CS42L42_PDN_TIMEOUT_US,
			true, cs42l42, CS42L42_CODEC_STATUS) < 0)
		codec_warn(codec, "Timeout waiting for PDN_DONE for CS42L42\n");

	/* Power down CS42L42 ASP/EQ/MIX/HP */
	cs8409_i2c_write(cs42l42, CS42L42_PWR_CTL2, 0x9C);
	cs42l42->suspended = 1;
	cs42l42->last_page = 0;
	cs42l42->hp_jack_in = 0;
	cs42l42->mic_jack_in = 0;

	/* Put CS42L42 into Reset */
	spec->gpio_data = snd_hda_codec_read(codec, CS8409_PIN_AFG, 0, AC_VERB_GET_GPIO_DATA, 0);
	spec->gpio_data &= ~cs42l42->reset_gpio;
	snd_hda_codec_write(codec, CS8409_PIN_AFG, 0, AC_VERB_SET_GPIO_DATA, spec->gpio_data);
}

static void cs8409_remove(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;

	/* disabled, not only cancelled: an init on the way out must not arm it */
	disable_delayed_work_sync(&spec->irq_sync_work);

	/* Cancel i2c clock disable timer, and disable clock if left enabled */
	cancel_delayed_work_sync(&spec->i2c_clk_work);
	cs8409_disable_i2c_clock(codec);

	snd_hda_gen_remove(codec);
}

/******************************************************************************
 *                   BULLSEYE / WARLOCK / CYBORG Specific Functions
 *                               CS8409/CS42L42
 ******************************************************************************/

/*
 * In the case of CS8409 we do not have unsolicited events from NID's 0x24
 * and 0x34 where hs mic and hp are connected. Companion codec CS42L42 will
 * generate interrupt via gpio 4 to notify jack events. We have to overwrite
 * generic snd_hda_jack_unsol_event(), read CS42L42 jack detect status registers
 * and then notify status via generic snd_hda_jack_unsol_event() call.
 */
static void cs8409_cs42l42_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[CS8409_CODEC0];
	struct hda_jack_tbl *jk;

	/* jack_unsol_event() will be called every time gpio line changing state.
	 * In this case gpio4 line goes up as a result of reading interrupt status
	 * registers in previous cs8409_jack_unsol_event() call.
	 * We don't need to handle this event, ignoring...
	 */
	if (res & cs42l42->irq_mask)
		return;

	if (cs42l42_jack_unsol_event(cs42l42)) {
		snd_hda_set_pin_ctl(codec, CS8409_CS42L42_SPK_PIN_NID,
				    cs42l42->hp_jack_in ? 0 : PIN_OUT);
		/* Report jack*/
		jk = snd_hda_jack_tbl_get_mst(codec, CS8409_CS42L42_HP_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec, (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
							AC_UNSOL_RES_TAG);
		/* Report jack*/
		jk = snd_hda_jack_tbl_get_mst(codec, CS8409_CS42L42_AMIC_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec, (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
							 AC_UNSOL_RES_TAG);
	}
}

static void cs8409_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs8409_spec *spec = codec->spec;

	if (spec->unsol_event)
		spec->unsol_event(codec, res);
	else
		cs8409_cs42l42_jack_unsol_event(codec, res);
}

static void cs8409_suspend_i2c(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;

	/* Cancel i2c clock disable timer, and disable clock if left enabled */
	cancel_delayed_work_sync(&spec->i2c_clk_work);
	cs8409_disable_i2c_clock(codec);

	/*
	 * The CS8409 may lose power while suspended, and its I2C address
	 * register with it. With one device on the bus the cached address
	 * would never be written again.
	 */
	spec->dev_addr = 0;
}

/* Manage PDREF, when transition to D3hot */
static int cs8409_cs42l42_suspend(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	int i;

	if (spec->suspend)
		return spec->suspend(codec);

	spec->init_done = 0;

	cs8409_enable_ur(codec, 0);

	for (i = 0; i < spec->num_scodecs; i++)
		cs42l42_suspend(spec->scodecs[i]);

	cs8409_suspend_i2c(codec);

	snd_hda_shutup_pins(codec);

	return 0;
}

/* Vendor specific HW configuration
 * PLL, ASP, I2C, SPI, GPIOs, DMIC etc...
 */
static void cs8409_cs42l42_hw_init(struct hda_codec *codec)
{
	const struct cs8409_cir_param *seq = cs8409_cs42l42_hw_cfg;
	const struct cs8409_cir_param *seq_bullseye = cs8409_cs42l42_bullseye_atn;
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[CS8409_CODEC0];

	if (spec->gpio_mask)
		snd_hda_codec_set_gpio(codec, spec->gpio_mask, spec->gpio_dir,
				       spec->gpio_data, 0);

	for (; seq->nid; seq++)
		cs8409_vendor_coef_set(codec, seq->cir, seq->coeff);

	if (codec->fixup_id == CS8409_BULLSEYE) {
		for (; seq_bullseye->nid; seq_bullseye++)
			cs8409_vendor_coef_set(codec, seq_bullseye->cir, seq_bullseye->coeff);
	}

	switch (codec->fixup_id) {
	case CS8409_CYBORG:
	case CS8409_WARLOCK_MLK_DUAL_MIC:
		/* DMIC1_MO=00b, DMIC1/2_SR=1 */
		cs8409_vendor_coef_set(codec, CS8409_DMIC_CFG, 0x0003);
		break;
	case CS8409_ODIN:
		/* ASP1/2_xxx_EN=1, ASP1/2_MCLK_EN=0, DMIC1_SCL_EN=0 */
		cs8409_vendor_coef_set(codec, CS8409_PAD_CFG_SLW_RATE_CTRL, 0xfc00);
		break;
	default:
		break;
	}

	cs42l42_resume(cs42l42);

	/* Enable Unsolicited Response */
	cs8409_enable_ur(codec, 1);
}

static int cs8409_cs42l42_exec_verb(struct hdac_device *dev, unsigned int cmd, unsigned int flags,
				    unsigned int *res)
{
	struct hda_codec *codec = container_of(dev, struct hda_codec, core);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[CS8409_CODEC0];

	unsigned int nid = ((cmd >> 20) & 0x07f);
	unsigned int verb = ((cmd >> 8) & 0x0fff);

	/* CS8409 pins have no AC_PINSENSE_PRESENCE
	 * capabilities. We have to intercept 2 calls for pins 0x24 and 0x34
	 * and return correct pin sense values for read_pin_sense() call from
	 * hda_jack based on CS42L42 jack detect status.
	 */
	switch (nid) {
	case CS8409_CS42L42_HP_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l42->hp_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	case CS8409_CS42L42_AMIC_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l42->mic_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	default:
		break;
	}

	return spec->exec_verb(dev, cmd, flags, res);
}

void cs8409_cs42l42_fixups(struct hda_codec *codec, const struct hda_fixup *fix, int action)
{
	struct cs8409_spec *spec = codec->spec;

	switch (action) {
	case HDA_FIXUP_ACT_PRE_PROBE:
		snd_hda_add_verbs(codec, cs8409_cs42l42_init_verbs);
		/* verb exec op override */
		spec->exec_verb = codec->core.exec_verb;
		codec->core.exec_verb = cs8409_cs42l42_exec_verb;

		spec->scodecs[CS8409_CODEC0] = &cs8409_cs42l42_codec;
		spec->num_scodecs = 1;
		spec->scodecs[CS8409_CODEC0]->codec = codec;

		spec->gen.suppress_auto_mute = 1;
		spec->gen.no_primary_hp = 1;
		spec->gen.suppress_vmaster = 1;

		spec->speaker_pdn_gpio = 0;

		/* GPIO 5 out, 3,4 in */
		spec->gpio_dir = spec->scodecs[CS8409_CODEC0]->reset_gpio;
		spec->gpio_data = 0;
		spec->gpio_mask = 0x03f;

		/* Basic initial sequence for specific hw configuration */
		snd_hda_sequence_write(codec, cs8409_cs42l42_init_verbs);

		cs8409_fix_caps(codec, CS8409_CS42L42_HP_PIN_NID);
		cs8409_fix_caps(codec, CS8409_CS42L42_AMIC_PIN_NID);

		spec->scodecs[CS8409_CODEC0]->hsbias_hiz = 0x0020;

		switch (codec->fixup_id) {
		case CS8409_CYBORG:
			spec->scodecs[CS8409_CODEC0]->full_scale_vol =
				CS42L42_FULL_SCALE_VOL_MINUS6DB;
			spec->speaker_pdn_gpio = CS8409_CYBORG_SPEAKER_PDN;
			break;
		case CS8409_ODIN:
			spec->scodecs[CS8409_CODEC0]->full_scale_vol = CS42L42_FULL_SCALE_VOL_0DB;
			spec->speaker_pdn_gpio = CS8409_CYBORG_SPEAKER_PDN;
			break;
		case CS8409_WARLOCK_MLK:
		case CS8409_WARLOCK_MLK_DUAL_MIC:
			spec->scodecs[CS8409_CODEC0]->full_scale_vol = CS42L42_FULL_SCALE_VOL_0DB;
			spec->speaker_pdn_gpio = CS8409_WARLOCK_SPEAKER_PDN;
			break;
		default:
			spec->scodecs[CS8409_CODEC0]->full_scale_vol =
				CS42L42_FULL_SCALE_VOL_MINUS6DB;
			spec->speaker_pdn_gpio = CS8409_WARLOCK_SPEAKER_PDN;
			break;
		}

		if (spec->speaker_pdn_gpio > 0) {
			spec->gpio_dir |= spec->speaker_pdn_gpio;
			spec->gpio_data |= spec->speaker_pdn_gpio;
		}

		break;
	case HDA_FIXUP_ACT_PROBE:
		/* Fix Sample Rate to 48kHz */
		spec->gen.stream_analog_playback = &cs42l42_48k_pcm_analog_playback;
		spec->gen.stream_analog_capture = &cs42l42_48k_pcm_analog_capture;
		/* add hooks */
		spec->gen.pcm_playback_hook = cs42l42_playback_pcm_hook;
		spec->gen.pcm_capture_hook = cs42l42_capture_pcm_hook;
		if (codec->fixup_id != CS8409_ODIN)
			/* Set initial DMIC volume to -26 dB */
			snd_hda_codec_amp_init_stereo(codec, CS8409_CS42L42_DMIC_ADC_PIN_NID,
						      HDA_INPUT, 0, 0xff, 0x19);
		snd_hda_gen_add_kctl(&spec->gen, "Headphone Playback Volume",
				&cs42l42_dac_volume_mixer);
		snd_hda_gen_add_kctl(&spec->gen, "Mic Capture Volume",
				&cs42l42_adc_volume_mixer);
		if (spec->speaker_pdn_gpio > 0)
			snd_hda_gen_add_kctl(&spec->gen, "Speaker Playback Switch",
					     &cs8409_spk_sw_ctrl);
		/* Disable Unsolicited Response during boot */
		cs8409_enable_ur(codec, 0);
		snd_hda_codec_set_name(codec, "CS8409/CS42L42");
		break;
	case HDA_FIXUP_ACT_INIT:
		cs8409_cs42l42_hw_init(codec);
		spec->init_done = 1;
		if (spec->init_done && spec->build_ctrl_done
			&& !spec->scodecs[CS8409_CODEC0]->hp_jack_in)
			cs42l42_run_jack_detect(spec->scodecs[CS8409_CODEC0]);
		break;
	case HDA_FIXUP_ACT_BUILD:
		spec->build_ctrl_done = 1;
		/* Run jack auto detect first time on boot
		 * after controls have been added, to check if jack has
		 * been already plugged in.
		 * Run immediately after init.
		 */
		if (spec->init_done && spec->build_ctrl_done
			&& !spec->scodecs[CS8409_CODEC0]->hp_jack_in)
			cs42l42_run_jack_detect(spec->scodecs[CS8409_CODEC0]);
		break;
	default:
		break;
	}
}

static int cs8409_comp_bind(struct device *dev)
{
	struct hda_codec *codec = dev_to_hda_codec(dev);
	struct cs8409_spec *spec = codec->spec;

	return hda_component_manager_bind(codec, &spec->comps);
}

static void cs8409_comp_unbind(struct device *dev)
{
	struct hda_codec *codec = dev_to_hda_codec(dev);
	struct cs8409_spec *spec = codec->spec;

	hda_component_manager_unbind(codec, &spec->comps);
}

static const struct component_master_ops cs8409_comp_master_ops = {
	.bind = cs8409_comp_bind,
	.unbind = cs8409_comp_unbind,
};

static void cs8409_comp_playback_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
				      struct snd_pcm_substream *sub, int action)
{
	struct cs8409_spec *spec = codec->spec;

	hda_component_manager_playback_hook(&spec->comps, action);
}

static void cs8409_cdb35l56_four_hw_init(struct hda_codec *codec)
{
	const struct cs8409_cir_param *seq = cs8409_cdb35l56_four_hw_cfg;

	for (; seq->nid; seq++)
		cs8409_vendor_coef_set(codec, seq->cir, seq->coeff);
}

static int cs8409_spk_sw_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cs8409_spec *spec = codec->spec;

	ucontrol->value.integer.value[0] = !spec->speaker_muted;

	return 0;
}

static int cs8409_spk_sw_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct cs8409_spec *spec = codec->spec;
	bool muted = !ucontrol->value.integer.value[0];

	if (muted == spec->speaker_muted)
		return 0;

	spec->speaker_muted = muted;

	return 1;
}

static const struct snd_kcontrol_new cs8409_spk_sw_component_ctrl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.info = snd_ctl_boolean_mono_info,
	.get = cs8409_spk_sw_get,
	.put = cs8409_spk_sw_put,
};

void cs8409_cdb35l56_four_autodet_fixup(struct hda_codec *codec,
				  const struct hda_fixup *fix,
				  int action)
{
	struct device *dev = hda_codec_dev(codec);
	struct cs8409_spec *spec = codec->spec;
	struct acpi_device *adev;
	const char *bus = NULL;
	static const struct {
		const char *hid;
		const char *name;
	} acpi_ids[] = {{ "CSC3554", "cs35l54-hda" },
			{ "CSC3556", "cs35l56-hda" },
			{ "CSC3557", "cs35l57-hda" }};
	char *match;
	int i, count = 0, count_devindex = 0;
	int ret;

	switch (action) {
	case HDA_FIXUP_ACT_PRE_PROBE: {
		for (i = 0; i < ARRAY_SIZE(acpi_ids); ++i) {
			adev = acpi_dev_get_first_match_dev(acpi_ids[i].hid, NULL, -1);
			if (adev)
				break;
		}
		if (!adev) {
			dev_err(dev, "Failed to find ACPI entry for a Cirrus Amp\n");
			return;
		}

		count = i2c_acpi_client_count(adev);
		if (count > 0) {
			bus = "i2c";
		} else {
			count = acpi_spi_count_resources(adev);
			if (count > 0)
				bus = "spi";
		}

		struct fwnode_handle *fwnode __free(fwnode_handle) =
			fwnode_handle_get(acpi_fwnode_handle(adev));
		acpi_dev_put(adev);

		if (!bus) {
			dev_err(dev, "Did not find any buses for %s\n", acpi_ids[i].hid);
			return;
		}

		if (!fwnode) {
			dev_err(dev, "Could not get fwnode for %s\n", acpi_ids[i].hid);
			return;
		}

		/*
		 * When available the cirrus,dev-index property is an accurate
		 * count of the amps in a system and is used in preference to
		 * the count of bus devices that can contain additional address
		 * alias entries.
		 */
		count_devindex = fwnode_property_count_u32(fwnode, "cirrus,dev-index");
		if (count_devindex > 0)
			count = count_devindex;

		match = devm_kasprintf(dev, GFP_KERNEL, "-%%s:00-%s.%%d", acpi_ids[i].name);
		if (!match)
			return;
		dev_info(dev, "Found %d %s on %s (%s)\n", count, acpi_ids[i].hid, bus, match);

		ret = hda_component_manager_init(codec, &spec->comps, count, bus,
						 acpi_ids[i].hid, match,
						 &cs8409_comp_master_ops);
		if (ret)
			return;

		spec->gen.pcm_playback_hook = cs8409_comp_playback_hook;

		snd_hda_add_verbs(codec, cs8409_cdb35l56_four_init_verbs);
		snd_hda_sequence_write(codec, cs8409_cdb35l56_four_init_verbs);
		break;
	}
	case HDA_FIXUP_ACT_PROBE:
		spec->speaker_muted = 0; /* speakers begin enabled */
		snd_hda_gen_add_kctl(&spec->gen, "Speaker Playback Switch",
				     &cs8409_spk_sw_component_ctrl);
		spec->gen.stream_analog_playback = &cs42l42_48k_pcm_analog_playback;
		snd_hda_codec_set_name(codec, "CS8409/CS35L56");
		break;
	case HDA_FIXUP_ACT_INIT:
		cs8409_cdb35l56_four_hw_init(codec);
		break;
	case HDA_FIXUP_ACT_FREE:
		hda_component_manager_free(&spec->comps, &cs8409_comp_master_ops);
		break;
	}
}

/******************************************************************************
 *                          Dolphin Specific Functions
 *                               CS8409/ 2 X CS42L42
 ******************************************************************************/

/*
 * In the case of CS8409 we do not have unsolicited events when
 * hs mic and hp are connected. Companion codec CS42L42 will
 * generate interrupt via irq_mask to notify jack events. We have to overwrite
 * generic snd_hda_jack_unsol_event(), read CS42L42 jack detect status registers
 * and then notify status via generic snd_hda_jack_unsol_event() call.
 */
static void dolphin_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42;
	struct hda_jack_tbl *jk;

	cs42l42 = spec->scodecs[CS8409_CODEC0];
	if (!cs42l42->suspended && (~res & cs42l42->irq_mask) &&
	    cs42l42_jack_unsol_event(cs42l42)) {
		jk = snd_hda_jack_tbl_get_mst(codec, DOLPHIN_HP_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec,
						 (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
						  AC_UNSOL_RES_TAG);

		jk = snd_hda_jack_tbl_get_mst(codec, DOLPHIN_AMIC_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec,
						 (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
						  AC_UNSOL_RES_TAG);
	}

	cs42l42 = spec->scodecs[CS8409_CODEC1];
	if (!cs42l42->suspended && (~res & cs42l42->irq_mask) &&
	    cs42l42_jack_unsol_event(cs42l42)) {
		jk = snd_hda_jack_tbl_get_mst(codec, DOLPHIN_LO_PIN_NID, 0);
		if (jk)
			snd_hda_jack_unsol_event(codec,
						 (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
						  AC_UNSOL_RES_TAG);
	}
}

/* Vendor specific HW configuration
 * PLL, ASP, I2C, SPI, GPIOs, DMIC etc...
 */
static void dolphin_hw_init(struct hda_codec *codec)
{
	const struct cs8409_cir_param *seq = dolphin_hw_cfg;
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42;
	int i;

	if (spec->gpio_mask)
		snd_hda_codec_set_gpio(codec, spec->gpio_mask, spec->gpio_dir,
				       spec->gpio_data, 0);

	for (; seq->nid; seq++)
		cs8409_vendor_coef_set(codec, seq->cir, seq->coeff);

	for (i = 0; i < spec->num_scodecs; i++) {
		cs42l42 = spec->scodecs[i];
		cs42l42_resume(cs42l42);
	}

	/* Enable Unsolicited Response */
	cs8409_enable_ur(codec, 1);
}

static int dolphin_exec_verb(struct hdac_device *dev, unsigned int cmd, unsigned int flags,
			     unsigned int *res)
{
	struct hda_codec *codec = container_of(dev, struct hda_codec, core);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l42 = spec->scodecs[CS8409_CODEC0];

	unsigned int nid = ((cmd >> 20) & 0x07f);
	unsigned int verb = ((cmd >> 8) & 0x0fff);

	/* CS8409 pins have no AC_PINSENSE_PRESENCE
	 * capabilities. We have to intercept calls for CS42L42 pins
	 * and return correct pin sense values for read_pin_sense() call from
	 * hda_jack based on CS42L42 jack detect status.
	 */
	switch (nid) {
	case DOLPHIN_HP_PIN_NID:
	case DOLPHIN_LO_PIN_NID:
		if (nid == DOLPHIN_LO_PIN_NID)
			cs42l42 = spec->scodecs[CS8409_CODEC1];
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l42->hp_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	case DOLPHIN_AMIC_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l42->mic_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	default:
		break;
	}

	return spec->exec_verb(dev, cmd, flags, res);
}

void dolphin_fixups(struct hda_codec *codec, const struct hda_fixup *fix, int action)
{
	struct cs8409_spec *spec = codec->spec;
	struct snd_kcontrol_new *kctrl;
	int i;

	switch (action) {
	case HDA_FIXUP_ACT_PRE_PROBE:
		snd_hda_add_verbs(codec, dolphin_init_verbs);
		/* verb exec op override */
		spec->exec_verb = codec->core.exec_verb;
		codec->core.exec_verb = dolphin_exec_verb;

		spec->scodecs[CS8409_CODEC0] = &dolphin_cs42l42_0;
		spec->scodecs[CS8409_CODEC0]->codec = codec;
		spec->scodecs[CS8409_CODEC1] = &dolphin_cs42l42_1;
		spec->scodecs[CS8409_CODEC1]->codec = codec;
		spec->num_scodecs = 2;
		spec->gen.suppress_vmaster = 1;

		spec->unsol_event = dolphin_jack_unsol_event;

		/* GPIO 1,5 out, 0,4 in */
		spec->gpio_dir = spec->scodecs[CS8409_CODEC0]->reset_gpio |
				 spec->scodecs[CS8409_CODEC1]->reset_gpio;
		spec->gpio_data = 0;
		spec->gpio_mask = 0x03f;

		/* Basic initial sequence for specific hw configuration */
		snd_hda_sequence_write(codec, dolphin_init_verbs);

		snd_hda_jack_add_kctl(codec, DOLPHIN_LO_PIN_NID, "Line Out", true,
				      SND_JACK_HEADPHONE, NULL);

		snd_hda_jack_add_kctl(codec, DOLPHIN_AMIC_PIN_NID, "Microphone", true,
				      SND_JACK_MICROPHONE, NULL);

		cs8409_fix_caps(codec, DOLPHIN_HP_PIN_NID);
		cs8409_fix_caps(codec, DOLPHIN_LO_PIN_NID);
		cs8409_fix_caps(codec, DOLPHIN_AMIC_PIN_NID);

		spec->scodecs[CS8409_CODEC0]->full_scale_vol = CS42L42_FULL_SCALE_VOL_MINUS6DB;
		spec->scodecs[CS8409_CODEC1]->full_scale_vol = CS42L42_FULL_SCALE_VOL_MINUS6DB;

		break;
	case HDA_FIXUP_ACT_PROBE:
		/* Fix Sample Rate to 48kHz */
		spec->gen.stream_analog_playback = &cs42l42_48k_pcm_analog_playback;
		spec->gen.stream_analog_capture = &cs42l42_48k_pcm_analog_capture;
		/* add hooks */
		spec->gen.pcm_playback_hook = cs42l42_playback_pcm_hook;
		spec->gen.pcm_capture_hook = cs42l42_capture_pcm_hook;
		snd_hda_gen_add_kctl(&spec->gen, "Headphone Playback Volume",
				     &cs42l42_dac_volume_mixer);
		snd_hda_gen_add_kctl(&spec->gen, "Mic Capture Volume", &cs42l42_adc_volume_mixer);
		kctrl = snd_hda_gen_add_kctl(&spec->gen, "Line Out Playback Volume",
					     &cs42l42_dac_volume_mixer);
		/* Update Line Out kcontrol template */
		if (kctrl)
			kctrl->private_value = HDA_COMPOSE_AMP_VAL_OFS(DOLPHIN_HP_PIN_NID, 3, CS8409_CODEC1,
					       HDA_OUTPUT, CS42L42_VOL_DAC) | HDA_AMP_VAL_MIN_MUTE;
		cs8409_enable_ur(codec, 0);
		snd_hda_codec_set_name(codec, "CS8409/CS42L42");
		break;
	case HDA_FIXUP_ACT_INIT:
		dolphin_hw_init(codec);
		spec->init_done = 1;
		if (spec->init_done && spec->build_ctrl_done) {
			for (i = 0; i < spec->num_scodecs; i++) {
				if (!spec->scodecs[i]->hp_jack_in)
					cs42l42_run_jack_detect(spec->scodecs[i]);
			}
		}
		break;
	case HDA_FIXUP_ACT_BUILD:
		spec->build_ctrl_done = 1;
		/* Run jack auto detect first time on boot
		 * after controls have been added, to check if jack has
		 * been already plugged in.
		 * Run immediately after init.
		 */
		if (spec->init_done && spec->build_ctrl_done) {
			for (i = 0; i < spec->num_scodecs; i++) {
				if (!spec->scodecs[i]->hp_jack_in)
					cs42l42_run_jack_detect(spec->scodecs[i]);
			}
		}
		break;
	default:
		break;
	}
}

/******************************************************************************
 *                          Apple MacBookPro13,1
 *                CS8409 / CS42L83 / 4 x SSM3515 speaker amps
 ******************************************************************************/

/*
 * The four amps sit on the CS8409's own I2C master next to the CS42L83. They
 * are not CS42L42s, so they are kept out of spec->scodecs[]; a sub_codec each
 * is only what the I2C helpers need to address them.
 */
static struct sub_codec mbp131_ssm3515[MBP131_NUM_AMPS];

/*
 * TDM slot of each amp. The two converters carry the same stereo pair, so the
 * frame is L R L R; with this mapping, the one of the out-of-tree driver and
 * not AppleHDA's 0 1 2 3, the amps at 0x14 and 0x15 get the left channel and
 * left and right come out on their sides. Which amp drives which driver of a
 * side is not known.
 */
static const unsigned int mbp131_ssm3515_slot[MBP131_NUM_AMPS] = { 0, 2, 1, 3 };

static void mbp131_amps_power(bool on)
{
	int i;

	for (i = 0; i < MBP131_NUM_AMPS; i++)
		cs8409_i2c_write(&mbp131_ssm3515[i], SSM3515_PWR,
				 on ? 0 : SSM3515_PWR_SPWDN);
}

/* Keep the I2C helpers off the amps while the codec is suspended */
static void mbp131_amps_suspended(bool suspended)
{
	int i;

	for (i = 0; i < MBP131_NUM_AMPS; i++)
		mbp131_ssm3515[i].suspended = suspended;
}

static void mbp131_amps_init(void)
{
	struct sub_codec *amp;
	int i;

	for (i = 0; i < MBP131_NUM_AMPS; i++) {
		amp = &mbp131_ssm3515[i];
		amp->suspended = 0;

		cs8409_i2c_write(amp, SSM3515_PWR,
				 SSM3515_PWR_APWDN_EN | SSM3515_PWR_S_RST | SSM3515_PWR_SPWDN);
		usleep_range(1000, 2000);
		cs8409_i2c_write(amp, SSM3515_SAI2, mbp131_ssm3515_slot[i]);
		cs8409_i2c_bulk_write(amp, mbp131_ssm3515_init_reg_seq,
				      mbp131_ssm3515_init_reg_seq_num);
		cs8409_i2c_write(amp, SSM3515_PWR, SSM3515_PWR_SPWDN);
	}
}

/*
 * Speaker volume is the digital volume of the amps. The control counts steps
 * up from mute and stops at the level AppleHDA programs: nothing here knows
 * what the drivers stand above it.
 */
static const DECLARE_TLV_DB_MINMAX_MUTE(mbp131_spk_db_scale, -7162, -300);

static void mbp131_amps_volume(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	int i;

	/* odd slots carry the right channel */
	for (i = 0; i < MBP131_NUM_AMPS; i++)
		cs8409_i2c_write(&mbp131_ssm3515[i], SSM3515_DAC_VOL, SSM3515_DAC_VOL_MUTE -
				 spec->speaker_vol[mbp131_ssm3515_slot[i] & 1]);
}

static int mbp131_spk_volume_info(struct snd_kcontrol *kctrl, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = MBP131_SPK_VOL_MAX;
	uinfo->value.integer.step = 1;

	return 0;
}

static int mbp131_spk_volume_get(struct snd_kcontrol *kctrl, struct snd_ctl_elem_value *uctrl)
{
	struct hda_codec *codec = snd_kcontrol_chip(kctrl);
	struct cs8409_spec *spec = codec->spec;

	uctrl->value.integer.value[0] = spec->speaker_vol[0];
	uctrl->value.integer.value[1] = spec->speaker_vol[1];

	return 0;
}

static int mbp131_spk_volume_put(struct snd_kcontrol *kctrl, struct snd_ctl_elem_value *uctrl)
{
	struct hda_codec *codec = snd_kcontrol_chip(kctrl);
	struct cs8409_spec *spec = codec->spec;
	long *valp = uctrl->value.integer.value;
	int i, changed = 0;

	for (i = 0; i < 2; i++)
		if (valp[i] < 0 || valp[i] > MBP131_SPK_VOL_MAX)
			return -EINVAL;

	guard(mutex)(&spec->jack_lock);

	for (i = 0; i < 2; i++) {
		if (spec->speaker_vol[i] != valp[i]) {
			spec->speaker_vol[i] = valp[i];
			changed = 1;
		}
	}

	/* otherwise the next prepare writes it; no need to wake the codec */
	if (changed && spec->playback_started)
		mbp131_amps_volume(codec);

	return changed;
}

static const struct snd_kcontrol_new mbp131_spk_volume_mixer = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = (SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ),
	.info = mbp131_spk_volume_info,
	.get = mbp131_spk_volume_get,
	.put = mbp131_spk_volume_put,
	.tlv = { .p = mbp131_spk_db_scale },
};

/* Speakers follow the headphone jack by hand, as on the other boards */
static void mbp131_update_speakers(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	bool on = !spec->scodecs[CS8409_CODEC0]->hp_jack_in;

	snd_hda_set_pin_ctl(codec, MBP131_SPK_A_PIN_NID, on ? PIN_OUT : 0);
	snd_hda_set_pin_ctl(codec, MBP131_SPK_B_PIN_NID, on ? PIN_OUT : 0);
	mbp131_amps_power(on && spec->playback_started);
}

/* The CS42L83 back on its oscillator; the bit clock has to be there still */
static void mbp131_cs42l83_clk_stop(struct sub_codec *cs42l83)
{
	struct cs8409_spec *spec = cs42l83->codec->spec;

	if (!spec->scodec_clk_on)
		return;

	cs8409_i2c_write(cs42l83, CS42L42_OSC_SWITCH, 0);
	usleep_range(CS42L42_CLOCK_SWITCH_DELAY_US, CS42L42_CLOCK_SWITCH_DELAY_US * 2);
	spec->scodec_clk_on = 0;
}

/*
 * The CS42L83 around a stream. On its oscillator and powered down while idle,
 * it gets the blocks of the stream's direction powered and is switched to the
 * bit clock of ASP2 when a stream is prepared, and goes back when the last one
 * is cleaned up; the ASoC cs42l42 driver and AppleHDA work per stream as well.
 * Put on the bit clock once from the init with everything up, the way of the
 * Dell boards, the headphone path came up right for one stream and distorted
 * or silent for the next.
 */
static void mbp131_cs42l83_stream(struct hda_codec *codec, unsigned int pdn_bits, bool on)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];
	bool playback = pdn_bits & CS42L42_HP_PDN_MASK;
	int pwr, val;

	pwr = cs8409_i2c_read(cs42l83, CS42L42_PWR_CTL1);
	if (pwr < 0)
		return;

	if (!on) {
		if (playback) {
			cs8409_i2c_write(cs42l83, CS42L42_ASP_RX_DAI0_EN, 0x00);
		} else {
			cs8409_i2c_write(cs42l83, CS42L42_ASP_TX_SZ_EN, 0x00);
			cs8409_i2c_write(cs42l83, CS42L42_ASP_TX_CH_EN, 0x00);
		}
		if (!spec->playback_started && !spec->capture_started)
			mbp131_cs42l83_clk_stop(cs42l83);
		cs8409_i2c_write(cs42l83, CS42L42_PWR_CTL1, pwr | pdn_bits);
		return;
	}

	/*
	 * All blocks of the direction at once, the headphone amplifier too:
	 * AppleHDA powers that one last, after the converter has locked. No
	 * pop was heard this way, and the mixer is still muted here.
	 */
	cs8409_i2c_write(cs42l83, CS42L42_PWR_CTL1, pwr & ~pdn_bits);

	if (!spec->scodec_clk_on) {
		cs8409_i2c_write(cs42l83, CS42L42_OSC_SWITCH, CS42L42_SCLK_PRESENT_MASK);
		usleep_range(CS42L42_CLOCK_SWITCH_DELAY_US, CS42L42_CLOCK_SWITCH_DELAY_US * 2);
		spec->scodec_clk_on = 1;
	}

	/*
	 * The serial port channels only now, on the bit clock. This order is
	 * a finding on this board, not the ASoC driver's: enabled ahead of the
	 * switch from the oscillator, the receive side now and then stayed
	 * deaf for the whole stream, everything locked and the headphone amp
	 * alive, until the enable was taken away and set again.
	 */
	if (!playback) {
		/* both channels, then the port */
		cs8409_i2c_write(cs42l83, CS42L42_ASP_TX_CH_EN, 0x03);
		cs8409_i2c_write(cs42l83, CS42L42_ASP_TX_SZ_EN, 0x01);
		return;
	}

	/* both channels of DAI0 */
	cs8409_i2c_write(cs42l83, CS42L42_ASP_RX_DAI0_EN, 0x0C);

	/* the hook unmutes the mixer once the converter has locked */
	if (read_poll_timeout(cs8409_i2c_read, val,
			      val < 0 || (val & CS42L42_SRCPL_DAC_LK_MASK),
			      2000, 100000, false, cs42l83, CS42L42_SRCPL_INT_STATUS) < 0)
		codec_warn(codec, "CS42L83 DAC SRC did not lock\n");
}

#define MBP131_L83_PLAYBACK_PDN	(CS42L42_ASP_DAI_PDN_MASK | CS42L42_MIXER_PDN_MASK | \
				 CS42L42_HP_PDN_MASK)
#define MBP131_L83_CAPTURE_PDN	(CS42L42_ASP_DAO_PDN_MASK | CS42L42_ADC_PDN_MASK)

static void mbp131_playback_pcm_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
				     struct snd_pcm_substream *substream, int action)
{
	struct cs8409_spec *spec = codec->spec;

	guard(mutex)(&spec->jack_lock);

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		mbp131_cs42l83_stream(codec, MBP131_L83_PLAYBACK_PDN, true);
		/* headphone DAC unmute, and spec->playback_started */
		cs42l42_playback_pcm_hook(hinfo, codec, substream, action);
		mbp131_amps_volume(codec);
		mbp131_update_speakers(codec);
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		cs42l42_playback_pcm_hook(hinfo, codec, substream, action);
		mbp131_update_speakers(codec);
		mbp131_cs42l83_stream(codec, MBP131_L83_PLAYBACK_PDN, false);
		break;
	default:
		break;
	}
}

static void mbp131_capture_pcm_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
				    struct snd_pcm_substream *substream, int action)
{
	struct cs8409_spec *spec = codec->spec;

	guard(mutex)(&spec->jack_lock);

	switch (action) {
	case HDA_GEN_PCM_ACT_PREPARE:
		mbp131_cs42l83_stream(codec, MBP131_L83_CAPTURE_PDN, true);
		cs42l42_capture_pcm_hook(hinfo, codec, substream, action);
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		cs42l42_capture_pcm_hook(hinfo, codec, substream, action);
		mbp131_cs42l83_stream(codec, MBP131_L83_CAPTURE_PDN, false);
		break;
	default:
		break;
	}
}

static void mbp131_jack_report(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];
	struct hda_jack_tbl *jk;

	codec_dbg(codec, "Jack hp %d mic %d\n", cs42l83->hp_jack_in, cs42l83->mic_jack_in);
	mbp131_update_speakers(codec);

	jk = snd_hda_jack_tbl_get_mst(codec, MBP131_HP_PIN_NID, 0);
	if (jk)
		snd_hda_jack_unsol_event(codec, (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
						AC_UNSOL_RES_TAG);
	jk = snd_hda_jack_tbl_get_mst(codec, MBP131_AMIC_PIN_NID, 0);
	if (jk)
		snd_hda_jack_unsol_event(codec, (jk->tag << AC_UNSOL_RES_TAG_SHIFT) &
						AC_UNSOL_RES_TAG);
}

/*
 * As on the Dell boards the pins of the companion codec raise nothing
 * themselves: the CS42L83 pulls GPIO 0 low, and its status registers say why.
 */
static void mbp131_jack_event(struct hda_codec *codec, unsigned int gpio)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];

	lockdep_assert_held(&spec->jack_lock);

	/* the line going back up after the status registers were read */
	if ((gpio & cs42l83->irq_mask) || cs42l83->suspended)
		return;

	if (cs42l42_jack_unsol_event(cs42l83) > 0)
		mbp131_jack_report(codec);
}

static void mbp131_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];
	struct device *dev = hda_codec_dev(codec);
	int active;

	/*
	 * Not while the codec is on its way down or up: the handler would run
	 * its I2C sequences into those of the standby. A reference keeps a
	 * runtime suspend away for as long as the handler takes. Without one,
	 * ask for a resume: it looks at the tip sense status itself, and the
	 * sync work after it at the line. Negative means no runtime PM.
	 */
	active = pm_runtime_get_if_active(dev);
	if (!active) {
		if (!(res & cs42l83->irq_mask))
			pm_request_resume(dev);
		return;
	}

	scoped_guard(mutex, &spec->jack_lock)
		mbp131_jack_event(codec, res);

	if (active > 0)
		pm_runtime_put_autosuspend(dev);
}

/*
 * The response to the interrupt is sent on an edge of the line, and the HDA
 * core drops responses until the card is registered and again around system
 * sleep. An interrupt raised by the jack detect run from init can so be lost,
 * and then all later ones with it: nothing reads the status registers, the
 * line stays low and makes no further edge. So look at the line itself once
 * the responses get through.
 */
static void mbp131_irq_sync(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	struct device *dev = hda_codec_dev(codec);
	unsigned int gpio;
	int active;

	if (!codec->core.registered ||
	    codec->core.dev.power.power_state.event != PM_EVENT_ON) {
		mod_delayed_work(system_dfl_wq, &spec->irq_sync_work, msecs_to_jiffies(100));
		return;
	}

	/*
	 * Not snd_hda_power_up(): the suspend waits for this work to end.
	 * Negative means no runtime PM; zero, not active: once more if the
	 * codec is on its way up or down, nothing to do if it is suspended.
	 */
	active = pm_runtime_get_if_active(dev);
	if (!active) {
		if (!pm_runtime_suspended(dev))
			mod_delayed_work(system_dfl_wq, &spec->irq_sync_work,
					 msecs_to_jiffies(100));
		return;
	}

	/* the level read under the lock: a handler ahead of us has cleared it */
	scoped_guard(mutex, &spec->jack_lock) {
		gpio = snd_hda_codec_read(codec, CS8409_PIN_AFG, 0, AC_VERB_GET_GPIO_DATA, 0);
		mbp131_jack_event(codec, gpio);
	}

	if (active > 0)
		pm_runtime_put_autosuspend(dev);
}

static int mbp131_exec_verb(struct hdac_device *dev, unsigned int cmd, unsigned int flags,
			    unsigned int *res)
{
	struct hda_codec *codec = container_of(dev, struct hda_codec, core);
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];

	unsigned int nid = ((cmd >> 20) & 0x07f);
	unsigned int verb = ((cmd >> 8) & 0x0fff);

	/* Pin sense for the two jack pins comes from the CS42L83 */
	switch (nid) {
	case MBP131_HP_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l83->hp_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	case MBP131_AMIC_PIN_NID:
		if (verb == AC_VERB_GET_PIN_SENSE) {
			*res = (cs42l83->mic_jack_in) ? AC_PINSENSE_PRESENCE : 0;
			return 0;
		}
		break;
	default:
		break;
	}

	return spec->exec_verb(dev, cmd, flags, res);
}

/*
 * Runtime suspend with nothing but the CS42L83 to notice a plug: the chip
 * stays out of reset, silent and powered down, on its own oscillator because
 * the bit clock stops. Its tip sense interrupt stays unmasked and comes through
 * as an unsolicited response of the CS8409; the handler asks for a resume, and
 * the resume looks at the tip sense status.
 */
static void mbp131_cs42l83_standby(struct sub_codec *cs42l83)
{
	static const struct cs8409_i2c_param standby_seq[] = {
		{ CS42L42_MIXER_CHA_VOL, 0x3F },
		{ CS42L42_MIXER_ADC_VOL, 0x3F },
		{ CS42L42_MIXER_CHB_VOL, 0x3F },
		{ CS42L42_HP_CTL, 0x0D },
		{ CS42L42_ASP_RX_DAI0_EN, 0x00 },
		{ CS42L42_OSC_SWITCH, 0x00 },
		{ CS42L42_PWR_CTL1, 0xFE },
	};

	cs8409_i2c_bulk_write(cs42l83, standby_seq, ARRAY_SIZE(standby_seq));

	/* the jack state stays: the chip is alive and reports any change */
	cs42l83->suspended = 1;
	cs42l83->last_page = 0;
}

/*
 * Out of standby nothing was lost: the CS8409 kept its registers in D3 and the
 * CS42L83 was never reset. Undo what the standby did and compare the tip sense
 * status with what was known before. Starting from reset each time would mean
 * a new type detection under every stream that starts from idle: speakers on
 * until it is done, and the headset switches moved around while the stream
 * plays.
 */
static void mbp131_standby_exit(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];
	unsigned int fsv;
	int ts;

	cs42l83->suspended = 0;
	spec->scodec_clk_on = 0;

	/* the standby wrote 0x0D: analog mutes off again, as cs42l42_resume() does */
	fsv = 0x0D & ~CS42L42_ANA_MUTE_AB;
	if (cs42l83->full_scale_vol)
		fsv |= CS42L42_FULL_SCALE_VOL_MASK;
	cs8409_i2c_write(cs42l83, CS42L42_HP_CTL, fsv);

	mbp131_amps_suspended(false);
	cs42l42_enable_jack_detect(cs42l83);
	cs8409_enable_ur(codec, 1);

	/*
	 * What woke the codec, if it was the jack; read with the interrupt
	 * enabled again, so that nothing falls between the two. An unplug
	 * shows at once. A plug shows only after the rise debounce: until then
	 * the status reads as in transition, and the interrupt for the plug
	 * is still to come.
	 */
	ts = cs8409_i2c_read(cs42l83, CS42L42_TSRS_PLUG_STATUS);
	if (ts < 0)
		return;
	ts = (ts & (CS42L42_TS_PLUG_MASK | CS42L42_TS_UNPLUG_MASK)) >> CS42L42_TS_PLUG_SHIFT;

	if (ts != CS42L42_TS_PLUG && cs42l83->hp_jack_in) {
		/* out, or out and on its way in again: what was in is gone */
		cs42l83->hp_jack_in = 0;
		cs42l83->mic_jack_in = 0;
		mbp131_jack_report(codec);
	} else if (ts == CS42L42_TS_PLUG && !cs42l83->hp_jack_in) {
		cs42l42_run_jack_detect(cs42l83);
	}
}

/*
 * A runtime suspend goes to standby; a system sleep, with the codec in use,
 * takes the CS42L83 down for good. power_state is ON for a runtime suspend and
 * something else around a system sleep; after a system suspend aborted before
 * this callback ran it stays at RESUME until the next resume, and a runtime
 * suspend in between takes the system branch, which the full init then mends.
 * A shutdown comes here as a runtime suspend and leaves the chip in standby.
 */
static int mbp131_suspend(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];
	bool standby = codec->core.dev.power.power_state.event == PM_EVENT_ON;

	spec->init_done = 0;

	cancel_delayed_work_sync(&spec->irq_sync_work);

	/* no cleanup hook has run if a stream was up */
	mbp131_cs42l83_clk_stop(cs42l83);
	mbp131_amps_power(false);
	mbp131_amps_suspended(true);

	if (standby) {
		mbp131_cs42l83_standby(cs42l83);
	} else {
		/*
		 * A forced suspend does not honour the reference of a jack
		 * handler in flight: let that one finish first.
		 */
		guard(mutex)(&spec->jack_lock);

		spec->playback_started = 0;
		spec->capture_started = 0;
		cs8409_enable_ur(codec, 0);
		cs42l42_suspend(cs42l83);
	}

	cs8409_suspend_i2c(codec);

	snd_hda_shutup_pins(codec);

	return 0;
}

/* GPIO setup, reset line and one slot of mbp131_hw_cfg still as they were left */
static bool mbp131_standby_intact(struct hda_codec *codec)
{
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];
	const struct cs8409_cir_param *seq;

	if (snd_hda_codec_read(codec, CS8409_PIN_AFG, 0, AC_VERB_GET_GPIO_MASK, 0) !=
	    spec->gpio_mask)
		return false;
	if (!(snd_hda_codec_read(codec, CS8409_PIN_AFG, 0, AC_VERB_GET_GPIO_DATA, 0) &
	      cs42l83->reset_gpio))
		return false;

	/* and one coefficient of the table, picked for not being a default */
	for (seq = mbp131_hw_cfg; seq->nid; seq++)
		if (seq->cir == ASP2_A_TX_CTRL1)
			break;

	guard(mutex)(&spec->i2c_mux);

	return seq->nid && cs8409_vendor_coef_get(codec, seq->cir) == seq->coeff;
}

/* Returns false when the light way out of standby was enough */
static bool mbp131_hw_init(struct hda_codec *codec)
{
	const struct cs8409_cir_param *seq = mbp131_hw_cfg;
	struct cs8409_spec *spec = codec->spec;
	struct sub_codec *cs42l83 = spec->scodecs[CS8409_CODEC0];
	unsigned int cir;

	/*
	 * The light way only for a runtime resume, power_state ON, that finds
	 * the CS8409 as the standby left it. After any system sleep the full
	 * init: S3 leaves the CS8409 at its power-on defaults, and what a link
	 * reset without a power loss or the firmware leave behind is not known.
	 */
	if (spec->full_init_done &&
	    codec->core.dev.power.power_state.event == PM_EVENT_ON &&
	    mbp131_standby_intact(codec)) {
		mbp131_standby_exit(codec);
		return false;
	}

	/* from reset: whatever was known about the jack is void */
	cs42l83->hp_jack_in = 0;
	cs42l83->mic_jack_in = 0;

	/*
	 * The CS42L83 starts from reset, as after cs42l42_suspend(): its init
	 * sequence is written for the reset defaults.
	 */
	spec->gpio_data &= ~cs42l83->reset_gpio;

	if (spec->gpio_mask)
		snd_hda_codec_set_gpio(codec, spec->gpio_mask, spec->gpio_dir,
				       spec->gpio_data, 0);

	/*
	 * Every slot off first: after a power loss they are not. The last
	 * register, ASP2_H_RX_CTRL2, is left out as AppleHDA leaves it out.
	 */
	for (cir = ASP1_A_TX_CTRL1; cir <= ASP2_H_RX_CTRL1; cir++)
		cs8409_vendor_coef_set(codec, cir, 0x8000);

	for (; seq->nid; seq++)
		cs8409_vendor_coef_set(codec, seq->cir, seq->coeff);

	spec->scodec_clk_on = 0;
	cs42l83->last_page = 0;
	cs42l42_resume(cs42l83);
	mbp131_amps_init();

	/* Enable Unsolicited Response */
	cs8409_enable_ur(codec, 1);
	spec->full_init_done = 1;

	return true;
}

void mbp131_fixups(struct hda_codec *codec, const struct hda_fixup *fix, int action)
{
	struct cs8409_spec *spec = codec->spec;
	struct snd_kcontrol_new *kctrl;
	bool full;
	int i;

	switch (action) {
	case HDA_FIXUP_ACT_PRE_PROBE:
		snd_hda_add_verbs(codec, mbp131_init_verbs);
		/* verb exec op override */
		spec->exec_verb = codec->core.exec_verb;
		codec->core.exec_verb = mbp131_exec_verb;

		spec->scodecs[CS8409_CODEC0] = &mbp131_cs42l83;
		spec->scodecs[CS8409_CODEC0]->codec = codec;
		spec->num_scodecs = 1;
		for (i = 0; i < MBP131_NUM_AMPS; i++) {
			mbp131_ssm3515[i].codec = codec;
			mbp131_ssm3515[i].addr = SSM3515_I2C_ADDR(i);
			mbp131_ssm3515[i].paged = 0;
		}
		spec->gen.suppress_auto_mute = 1;
		spec->gen.no_primary_hp = 1;
		spec->gen.suppress_vmaster = 1;
		spec->unsol_event = mbp131_jack_unsol_event;
		spec->suspend = mbp131_suspend;
		spec->irq_sync = mbp131_irq_sync;
		spec->speaker_vol[0] = MBP131_SPK_VOL_MAX;
		spec->speaker_vol[1] = MBP131_SPK_VOL_MAX;

		/* GPIO 1 out, 0, 2, 3 in */
		spec->gpio_dir = spec->scodecs[CS8409_CODEC0]->reset_gpio;
		spec->gpio_data = 0;
		spec->gpio_mask = 0x0f;

		/* Basic initial sequence for specific hw configuration */
		snd_hda_sequence_write(codec, mbp131_init_verbs);

		cs8409_fix_caps(codec, MBP131_HP_PIN_NID);
		cs8409_fix_caps(codec, MBP131_AMIC_PIN_NID);

		spec->scodecs[CS8409_CODEC0]->hsbias_hiz = 0x0020;
		spec->scodecs[CS8409_CODEC0]->full_scale_vol = CS42L42_FULL_SCALE_VOL_0DB;
		break;
	case HDA_FIXUP_ACT_PROBE:
		/* Fix Sample Rate to 44.1kHz */
		spec->gen.stream_analog_playback = &mbp131_44k_pcm_analog_playback;
		spec->gen.stream_analog_capture = &mbp131_44k_pcm_analog_capture;
		/*
		 * Two speaker pins, but one stereo pair: the second converter
		 * gets a copy of the first, never channels of its own.
		 */
		spec->gen.multiout.max_channels = 2;
		/* add hooks */
		spec->gen.pcm_playback_hook = mbp131_playback_pcm_hook;
		spec->gen.pcm_capture_hook = mbp131_capture_pcm_hook;
		/* DMIC gain as AppleHDA sets it */
		snd_hda_codec_amp_init_stereo(codec, MBP131_DMIC_ADC_PIN_NID,
					      HDA_INPUT, 0, 0xff, 0x33);
		snd_hda_gen_add_kctl(&spec->gen, "Speaker Playback Volume",
				     &mbp131_spk_volume_mixer);
		kctrl = snd_hda_gen_add_kctl(&spec->gen, "Headphone Playback Volume",
					     &cs42l42_dac_volume_mixer);
		if (kctrl)
			kctrl->private_value =
				HDA_COMPOSE_AMP_VAL_OFS(MBP131_HP_PIN_NID, 3, CS8409_CODEC0,
							HDA_OUTPUT, CS42L42_VOL_DAC) |
				HDA_AMP_VAL_MIN_MUTE;
		kctrl = snd_hda_gen_add_kctl(&spec->gen, "Mic Capture Volume",
					     &cs42l42_adc_volume_mixer);
		if (kctrl)
			kctrl->private_value =
				HDA_COMPOSE_AMP_VAL_OFS(MBP131_AMIC_PIN_NID, 1, CS8409_CODEC0,
							HDA_INPUT, CS42L42_VOL_ADC) |
				HDA_AMP_VAL_MIN_MUTE;
		/* Disable Unsolicited Response during boot */
		cs8409_enable_ur(codec, 0);
		snd_hda_codec_set_name(codec, "CS8409/CS42L83");
		break;
	case HDA_FIXUP_ACT_INIT:
		full = mbp131_hw_init(codec);
		spec->init_done = 1;
		if (full && spec->build_ctrl_done &&
		    !spec->scodecs[CS8409_CODEC0]->hp_jack_in)
			cs42l42_run_jack_detect(spec->scodecs[CS8409_CODEC0]);
		mod_delayed_work(system_dfl_wq, &spec->irq_sync_work, msecs_to_jiffies(200));
		break;
	case HDA_FIXUP_ACT_BUILD:
		spec->build_ctrl_done = 1;
		/* The jack may have been plugged in before the controls existed */
		if (spec->init_done && spec->build_ctrl_done &&
		    !spec->scodecs[CS8409_CODEC0]->hp_jack_in)
			cs42l42_run_jack_detect(spec->scodecs[CS8409_CODEC0]);
		mod_delayed_work(system_dfl_wq, &spec->irq_sync_work, msecs_to_jiffies(200));
		break;
	default:
		break;
	}
}

/*
 * Boards whose codec subsystem ID cannot be relied on: the one the firmware
 * writes at boot is not there after the controller has been probed again, and
 * the CS8409 reports its own.
 */
static const struct dmi_system_id cs8409_dmi_fixup_tbl[] = {
	{
		.ident = "MacBookPro13,1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,1"),
		},
		.driver_data = (void *)CS8409_MBP131,
	},
	{}
};

static void cs8409_pick_dmi_fixup(struct hda_codec *codec)
{
	const struct dmi_system_id *dmi;

	if (codec->fixup_id != HDA_FIXUP_ID_NOT_SET)
		return;

	dmi = dmi_first_match(cs8409_dmi_fixup_tbl);
	if (!dmi)
		return;

	codec->fixup_id = (long)dmi->driver_data;
	codec->fixup_list = cs8409_fixups;
	codec->fixup_name = dmi->ident;
}

static int cs8409_probe(struct hda_codec *codec, const struct hda_device_id *id)
{
	int err;

	if (!cs8409_alloc_spec(codec))
		return -ENOMEM;

	snd_hda_pick_fixup(codec, cs8409_models, cs8409_fixup_tbl, cs8409_fixups);
	cs8409_pick_dmi_fixup(codec);

	codec_dbg(codec, "Picked ID=%d, VID=%08x, DEV=%08x\n", codec->fixup_id,
			 codec->bus->pci->subsystem_vendor,
			 codec->bus->pci->subsystem_device);

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PRE_PROBE);

	err = cs8409_parse_auto_config(codec);
	if (err < 0) {
		cs8409_remove(codec);
		return err;
	}

	snd_hda_apply_fixup(codec, HDA_FIXUP_ACT_PROBE);
	return 0;
}

static const struct hda_codec_ops cs8409_codec_ops = {
	.probe = cs8409_probe,
	.remove = cs8409_remove,
	.build_controls = cs8409_build_controls,
	.build_pcms = snd_hda_gen_build_pcms,
	.init = cs8409_init,
	.unsol_event = cs8409_unsol_event,
	.suspend = cs8409_cs42l42_suspend,
	.stream_pm = snd_hda_gen_stream_pm,
};

static const struct hda_device_id snd_hda_id_cs8409[] = {
	HDA_CODEC_ID(0x10138409, "CS8409"),
	{} /* terminator */
};
MODULE_DEVICE_TABLE(hdaudio, snd_hda_id_cs8409);

static struct hda_codec_driver cs8409_driver = {
	.id = snd_hda_id_cs8409,
	.ops = &cs8409_codec_ops,
};
module_hda_codec_driver(cs8409_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cirrus Logic HDA bridge");
MODULE_IMPORT_NS("SND_HDA_SCODEC_COMPONENT");
