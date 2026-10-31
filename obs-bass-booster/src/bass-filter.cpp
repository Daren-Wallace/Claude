/*
Bass Booster for OBS
Copyright (C) 2026 Daren Wallace

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// libobs glue: registers the "Bass Booster" audio filter, builds its
// properties UI, and hands parameters from the UI thread to the audio thread
// without ever blocking the audio thread.

#include "bass-filter.h"

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <plugin-support.h>

#include "dsp/bass_engine.hpp"
#include "dsp/param_fields.hpp"
#include "dsp/params.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>

using bassbooster::BassEngine;
using bassbooster::BassParams;
using bassbooster::BoolField;
using bassbooster::FloatField;
using bassbooster::findFloatField;
using bassbooster::kBoolFields;
using bassbooster::kFloatFields;

namespace {

// ------------------------------------------------------------ settings ----

constexpr const char *S_PRESET = "preset";
constexpr const char *S_BYPASS = "bypass";
constexpr const char *S_PUNCH_ENABLED = "punch_enabled";
constexpr const char *S_HARMONICS_ENABLED = "harmonics_enabled";
constexpr const char *S_SUB_ENABLED = "sub_enabled";
constexpr const char *S_LIMITER_ENABLED = "limiter_enabled";

BassParams paramsFromSettings(obs_data_t *settings)
{
	BassParams p;
	for (const FloatField &f : kFloatFields)
		p.*f.member = obs_data_get_double(settings, f.key);
	for (const BoolField &f : kBoolFields)
		p.*f.member = obs_data_get_bool(settings, f.key);
	return bassbooster::sanitize(p);
}

// Writes every preset-controlled field (all except bypass) into settings.
void writePresetToSettings(obs_data_t *settings, const BassParams &p)
{
	for (const FloatField &f : kFloatFields)
		obs_data_set_double(settings, f.key, p.*f.member);
	for (const BoolField &f : kBoolFields) {
		if (f.inPreset)
			obs_data_set_bool(settings, f.key, p.*f.member);
	}
}

// --------------------------------------------------------------- filter ----

struct BassFilter {
	obs_source_t *context = nullptr;
	BassEngine engine;

	// UI thread -> audio thread hand-off. update() locks briefly; the audio
	// thread only ever try_lock()s, so it can never be blocked by the UI.
	std::mutex pendingMutex;
	BassParams pending;
	std::atomic<bool> pendingDirty{false};

	uint32_t sampleRate = 0;
	size_t channels = 0;
};

void queryAudioFormat(uint32_t &sampleRate, size_t &channels)
{
	audio_t *audio = obs_get_audio();
	sampleRate = audio ? audio_output_get_sample_rate(audio) : 48000;
	channels = audio ? audio_output_get_channels(audio) : 2;
	if (sampleRate == 0)
		sampleRate = 48000;
	if (channels == 0)
		channels = 2;
}

const char *bbGetName(void *)
{
	return obs_module_text("BassBooster");
}

void bbUpdate(void *data, obs_data_t *settings)
{
	auto *f = static_cast<BassFilter *>(data);
	const BassParams p = paramsFromSettings(settings);
	{
		std::lock_guard<std::mutex> lock(f->pendingMutex);
		f->pending = p;
	}
	f->pendingDirty.store(true, std::memory_order_release);
}

void *bbCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new (std::nothrow) BassFilter();
	if (!f)
		return nullptr;
	f->context = source;

	queryAudioFormat(f->sampleRate, f->channels);
	f->engine.prepare(static_cast<double>(f->sampleRate), static_cast<int>(f->channels));
	f->engine.setParams(paramsFromSettings(settings));
	f->engine.snapToTargets();

	obs_log(LOG_INFO, "filter '%s' created (%u Hz, %zu ch)", obs_source_get_name(source), f->sampleRate,
		f->channels);
	return f;
}

void bbDestroy(void *data)
{
	delete static_cast<BassFilter *>(data);
}

obs_audio_data *bbFilterAudio(void *data, obs_audio_data *audio)
{
	auto *f = static_cast<BassFilter *>(data);
	if (!audio || audio->frames == 0)
		return audio;

	uint32_t sampleRate;
	size_t channels;
	queryAudioFormat(sampleRate, channels);
	if (sampleRate != f->sampleRate || channels != f->channels) {
		f->sampleRate = sampleRate;
		f->channels = channels;
		f->engine.prepare(static_cast<double>(sampleRate), static_cast<int>(channels));
	}

	if (f->pendingDirty.load(std::memory_order_acquire)) {
		std::unique_lock<std::mutex> lock(f->pendingMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			f->pendingDirty.store(false, std::memory_order_relaxed);
			f->engine.setParams(f->pending);
		}
	}

	float *planes[BassEngine::kMaxChannels] = {};
	const size_t n = channels < static_cast<size_t>(BassEngine::kMaxChannels) ? channels : BassEngine::kMaxChannels;
	size_t valid = 0;
	for (size_t ch = 0; ch < n; ++ch) {
		if (!audio->data[ch])
			break;
		planes[ch] = reinterpret_cast<float *>(audio->data[ch]);
		++valid;
	}
	if (valid > 0)
		f->engine.process(planes, static_cast<int>(valid), static_cast<int>(audio->frames));
	return audio;
}

void bbDefaults(obs_data_t *settings)
{
	const bassbooster::Preset *def = bassbooster::findPreset(bassbooster::kDefaultPresetId);
	const BassParams p = def ? def->params : BassParams{};

	obs_data_set_default_string(settings, S_PRESET, bassbooster::kDefaultPresetId);
	obs_data_set_default_bool(settings, S_BYPASS, false);
	for (const FloatField &fld : kFloatFields)
		obs_data_set_default_double(settings, fld.key, p.*fld.member);
	for (const BoolField &fld : kBoolFields) {
		if (fld.inPreset)
			obs_data_set_default_bool(settings, fld.key, p.*fld.member);
	}
}

// ----------------------------------------------------------- properties ----

// A factory preset was picked: copy its values into the sliders. When the
// sliders already match (e.g. when the dialog first opens), nothing changes.
bool presetModified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const bassbooster::Preset *preset = bassbooster::findPreset(obs_data_get_string(settings, S_PRESET));
	if (!preset)
		return false; // "Custom": keep whatever the user has dialled in
	if (bassbooster::presetMatches(paramsFromSettings(settings), preset->params))
		return false;
	writePresetToSettings(settings, preset->params);
	return true;
}

// Any control moved: if the values no longer match the selected preset,
// switch the preset list to "Custom" so the UI never lies about its state.
bool paramModified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const bassbooster::Preset *preset = bassbooster::findPreset(obs_data_get_string(settings, S_PRESET));
	if (!preset)
		return false;
	if (bassbooster::presetMatches(paramsFromSettings(settings), preset->params))
		return false;
	obs_data_set_string(settings, S_PRESET, bassbooster::kCustomPresetId);
	return true;
}

void setTooltip(obs_property_t *p, const char *textKey)
{
	char key[128];
	snprintf(key, sizeof(key), "%s.Tooltip", textKey);
	obs_property_set_long_description(p, obs_module_text(key));
}

void addSlider(obs_properties_t *props, const char *key)
{
	const FloatField *f = findFloatField(key);
	if (!f)
		return;
	obs_property_t *p = obs_properties_add_float_slider(props, f->key, obs_module_text(f->text), f->range->min,
							    f->range->max, f->range->step);
	obs_property_float_set_suffix(p, f->suffix);
	setTooltip(p, f->text);
	obs_property_set_modified_callback(p, paramModified);
}

obs_properties_t *addGroup(obs_properties_t *props, const char *key, const char *textKey, obs_group_type type)
{
	obs_properties_t *group = obs_properties_create();
	obs_property_t *p = obs_properties_add_group(props, key, obs_module_text(textKey), type, group);
	setTooltip(p, textKey);
	if (type == OBS_GROUP_CHECKABLE)
		obs_property_set_modified_callback(p, paramModified);
	return group;
}

obs_properties_t *bbProperties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "intro", obs_module_text("Intro"), OBS_TEXT_INFO);

	obs_property_t *preset = obs_properties_add_list(props, S_PRESET, obs_module_text("Preset"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	size_t count = 0;
	const bassbooster::Preset *list = bassbooster::presets(count);
	for (size_t i = 0; i < count; ++i)
		obs_property_list_add_string(preset, obs_module_text(list[i].localeKey), list[i].id);
	obs_property_list_add_string(preset, obs_module_text("Preset.Custom"), bassbooster::kCustomPresetId);
	setTooltip(preset, "Preset");
	obs_property_set_modified_callback(preset, presetModified);

	obs_property_t *bypass = obs_properties_add_bool(props, S_BYPASS, obs_module_text("Bypass"));
	setTooltip(bypass, "Bypass");

	obs_properties_t *boost = addGroup(props, "group_boost", "Group.Boost", OBS_GROUP_NORMAL);
	addSlider(boost, "shelf_gain_db");
	addSlider(boost, "shelf_freq_hz");
	addSlider(boost, "shelf_q");

	obs_properties_t *punch = addGroup(props, S_PUNCH_ENABLED, "Group.Punch", OBS_GROUP_CHECKABLE);
	addSlider(punch, "punch_gain_db");
	addSlider(punch, "punch_freq_hz");
	addSlider(punch, "punch_q");

	obs_properties_t *harm = addGroup(props, S_HARMONICS_ENABLED, "Group.Harmonics", OBS_GROUP_CHECKABLE);
	addSlider(harm, "harmonics_amount");
	addSlider(harm, "harmonics_drive");
	addSlider(harm, "harmonics_low_hz");
	addSlider(harm, "harmonics_high_hz");

	obs_properties_t *sub = addGroup(props, S_SUB_ENABLED, "Group.Sub", OBS_GROUP_CHECKABLE);
	addSlider(sub, "sub_amount");
	addSlider(sub, "sub_freq_hz");
	addSlider(sub, "sub_tightness");

	obs_properties_t *protection = addGroup(props, "group_protection", "Group.Protection", OBS_GROUP_NORMAL);
	addSlider(protection, "rumble_freq_hz");

	obs_properties_t *limiter = addGroup(props, S_LIMITER_ENABLED, "Group.Limiter", OBS_GROUP_CHECKABLE);
	addSlider(limiter, "limiter_ceiling_db");
	addSlider(limiter, "limiter_release_ms");

	obs_properties_t *output = addGroup(props, "group_output", "Group.Output", OBS_GROUP_NORMAL);
	addSlider(output, "input_gain_db");
	addSlider(output, "mix");
	addSlider(output, "output_gain_db");

	obs_property_t *help = obs_properties_add_button2(props, "help", obs_module_text("Help"), nullptr, nullptr);
	obs_property_button_set_type(help, OBS_BUTTON_URL);
	obs_property_button_set_url(help, const_cast<char *>("https://github.com/daren-wallace/claude/blob/main/"
							     "obs-bass-booster/docs/USER_GUIDE.md"));

	return props;
}

} // namespace

extern "C" void bass_booster_register(void)
{
	obs_source_info info = {};
	info.id = BASS_BOOSTER_FILTER_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = bbGetName;
	info.create = bbCreate;
	info.destroy = bbDestroy;
	info.update = bbUpdate;
	info.get_defaults = bbDefaults;
	info.get_properties = bbProperties;
	info.filter_audio = bbFilterAudio;
	obs_register_source(&info);
}
