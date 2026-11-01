/*
Instrumental Isolator for OBS
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

// libobs glue: registers the "Instrumental Isolator" audio filter, builds its
// properties UI and hands parameters from the UI thread to the audio thread
// without ever blocking the audio thread.

#include "isolator-filter.h"

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <plugin-support.h>

#include "dsp/isolator_engine.hpp"
#include "dsp/param_fields.hpp"
#include "dsp/params.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

using isolator::BoolField;
using isolator::findFloatField;
using isolator::FloatField;
using isolator::IsolatorEngine;
using isolator::IsolatorParams;
using isolator::kBoolFields;
using isolator::kFloatFields;
using isolator::kOutputModeKey;
using isolator::OutputMode;

namespace {

constexpr const char *S_PRESET = "preset";
constexpr const char *S_BYPASS = "bypass";
constexpr const char *S_PITCH_ENABLED = "pitch_enabled";
constexpr const char *S_AUTO_MAKEUP = "auto_makeup";

IsolatorParams paramsFromSettings(obs_data_t *settings)
{
	IsolatorParams p;
	for (const FloatField &f : kFloatFields)
		p.*f.member = obs_data_get_double(settings, f.key);
	for (const BoolField &f : kBoolFields)
		p.*f.member = obs_data_get_bool(settings, f.key);
	p.outputMode = obs_data_get_int(settings, kOutputModeKey) == 1 ? OutputMode::Vocals : OutputMode::Instrumental;
	return isolator::sanitize(p);
}

void writePresetToSettings(obs_data_t *settings, const IsolatorParams &p)
{
	for (const FloatField &f : kFloatFields)
		obs_data_set_double(settings, f.key, p.*f.member);
	for (const BoolField &f : kBoolFields)
		if (f.inPreset)
			obs_data_set_bool(settings, f.key, p.*f.member);
	obs_data_set_int(settings, kOutputModeKey, static_cast<long long>(p.outputMode));
}

// Index of the dedicated centre speaker in OBS's channel order, or -1.
int centreChannelFor(speaker_layout layout)
{
	switch (layout) {
	case SPEAKERS_4POINT0: // FL FR FC RC
	case SPEAKERS_4POINT1: // FL FR FC LFE RC
	case SPEAKERS_5POINT1: // FL FR FC LFE RL RR
	case SPEAKERS_7POINT1: // FL FR FC LFE RL RR SL SR
		return 2;
	default:
		return -1;
	}
}

struct AudioFormat {
	uint32_t sampleRate = 48000;
	size_t channels = 2;
	int centre = -1;

	bool operator!=(const AudioFormat &o) const
	{
		return sampleRate != o.sampleRate || channels != o.channels || centre != o.centre;
	}
};

AudioFormat queryAudioFormat()
{
	AudioFormat f;
	audio_t *audio = obs_get_audio();
	if (audio) {
		f.sampleRate = audio_output_get_sample_rate(audio);
		f.channels = audio_output_get_channels(audio);
		const audio_output_info *info = audio_output_get_info(audio);
		if (info)
			f.centre = centreChannelFor(info->speakers);
	}
	if (f.sampleRate == 0)
		f.sampleRate = 48000;
	if (f.channels == 0)
		f.channels = 2;
	return f;
}

struct IsolatorFilter {
	obs_source_t *context = nullptr;
	IsolatorEngine engine;

	// UI thread -> audio thread: update() locks briefly; the audio thread
	// only ever try_lock()s, so the UI can never stall audio.
	std::mutex pendingMutex;
	IsolatorParams pending;
	std::atomic<bool> pendingDirty{false};

	AudioFormat format;
};

const char *ifGetName(void *)
{
	return obs_module_text("InstrumentalIsolator");
}

void ifUpdate(void *data, obs_data_t *settings)
{
	auto *f = static_cast<IsolatorFilter *>(data);
	const IsolatorParams p = paramsFromSettings(settings);
	{
		std::lock_guard<std::mutex> lock(f->pendingMutex);
		f->pending = p;
	}
	f->pendingDirty.store(true, std::memory_order_release);
}

void *ifCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new (std::nothrow) IsolatorFilter();
	if (!f)
		return nullptr;
	f->context = source;
	f->format = queryAudioFormat();
	f->engine.prepare(static_cast<double>(f->format.sampleRate), static_cast<int>(f->format.channels),
			  f->format.centre);
	f->engine.setParams(paramsFromSettings(settings));
	f->engine.snapToTargets();
	obs_log(LOG_INFO, "filter '%s' created (%u Hz, %zu ch, %d-tap predictor)", obs_source_get_name(source),
		f->format.sampleRate, f->format.channels, f->engine.getPredictorTaps());
	return f;
}

void ifDestroy(void *data)
{
	delete static_cast<IsolatorFilter *>(data);
}

obs_audio_data *ifFilterAudio(void *data, obs_audio_data *audio)
{
	auto *f = static_cast<IsolatorFilter *>(data);
	if (!audio || audio->frames == 0)
		return audio;

	const AudioFormat now = queryAudioFormat();
	if (now != f->format) {
		f->format = now;
		f->engine.prepare(static_cast<double>(now.sampleRate), static_cast<int>(now.channels), now.centre);
	}

	if (f->pendingDirty.load(std::memory_order_acquire)) {
		std::unique_lock<std::mutex> lock(f->pendingMutex, std::try_to_lock);
		if (lock.owns_lock()) {
			f->pendingDirty.store(false, std::memory_order_relaxed);
			f->engine.setParams(f->pending);
		}
	}

	float *planes[IsolatorEngine::kMaxChannels] = {};
	const size_t n = now.channels < static_cast<size_t>(IsolatorEngine::kMaxChannels)
				 ? now.channels
				 : IsolatorEngine::kMaxChannels;
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

void ifDefaults(obs_data_t *settings)
{
	const isolator::Preset *def = isolator::findPreset(isolator::kDefaultPresetId);
	const IsolatorParams p = def ? def->params : IsolatorParams{};
	obs_data_set_default_string(settings, S_PRESET, isolator::kDefaultPresetId);
	obs_data_set_default_bool(settings, S_BYPASS, false);
	obs_data_set_default_int(settings, kOutputModeKey, static_cast<long long>(p.outputMode));
	for (const FloatField &fld : kFloatFields)
		obs_data_set_default_double(settings, fld.key, p.*fld.member);
	for (const BoolField &fld : kBoolFields)
		if (fld.inPreset)
			obs_data_set_default_bool(settings, fld.key, p.*fld.member);
}

// A factory preset was picked: copy its values in (only if they differ, so
// opening the dialog never changes anything).
bool presetModified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const isolator::Preset *preset = isolator::findPreset(obs_data_get_string(settings, S_PRESET));
	if (!preset || isolator::presetMatches(paramsFromSettings(settings), preset->params))
		return false;
	writePresetToSettings(settings, preset->params);
	return true;
}

// Any control changed: flip the preset list to Custom once values no longer
// match the selected preset.
bool paramModified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const isolator::Preset *preset = isolator::findPreset(obs_data_get_string(settings, S_PRESET));
	if (!preset || isolator::presetMatches(paramsFromSettings(settings), preset->params))
		return false;
	obs_data_set_string(settings, S_PRESET, isolator::kCustomPresetId);
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

obs_properties_t *ifProperties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "intro", obs_module_text("Intro"), OBS_TEXT_INFO);

	obs_property_t *preset = obs_properties_add_list(props, S_PRESET, obs_module_text("Preset"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	size_t count = 0;
	const isolator::Preset *list = isolator::presets(count);
	for (size_t i = 0; i < count; ++i)
		obs_property_list_add_string(preset, obs_module_text(list[i].localeKey), list[i].id);
	obs_property_list_add_string(preset, obs_module_text("Preset.Custom"), isolator::kCustomPresetId);
	setTooltip(preset, "Preset");
	obs_property_set_modified_callback(preset, presetModified);

	obs_property_t *mode = obs_properties_add_list(props, kOutputModeKey, obs_module_text("OutputMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("OutputMode.Instrumental"), 0);
	obs_property_list_add_int(mode, obs_module_text("OutputMode.Vocals"), 1);
	setTooltip(mode, "OutputMode");
	obs_property_set_modified_callback(mode, paramModified);

	obs_property_t *bypass = obs_properties_add_bool(props, S_BYPASS, obs_module_text("Bypass"));
	setTooltip(bypass, "Bypass");

	obs_properties_t *removal = addGroup(props, "group_removal", "Group.Removal", OBS_GROUP_NORMAL);
	addSlider(removal, "strength");
	addSlider(removal, "centre_width");
	addSlider(removal, "response_ms");

	obs_properties_t *protection = addGroup(props, "group_protection", "Group.Protection", OBS_GROUP_NORMAL);
	addSlider(protection, "protect_low_hz");
	addSlider(protection, "protect_high_hz");
	addSlider(protection, "transient_protect");

	obs_properties_t *pitch = addGroup(props, S_PITCH_ENABLED, "Group.Pitch", OBS_GROUP_CHECKABLE);
	addSlider(pitch, "pitch_amount");
	addSlider(pitch, "pitch_harmonics");
	addSlider(pitch, "voice_low_hz");
	addSlider(pitch, "voice_high_hz");

	obs_properties_t *output = addGroup(props, "group_output", "Group.Output", OBS_GROUP_NORMAL);
	addSlider(output, "stereo_width");
	obs_property_t *makeup = obs_properties_add_bool(output, S_AUTO_MAKEUP, obs_module_text("AutoMakeup"));
	setTooltip(makeup, "AutoMakeup");
	obs_property_set_modified_callback(makeup, paramModified);
	addSlider(output, "output_gain_db");

	obs_property_t *help = obs_properties_add_button2(props, "help", obs_module_text("Help"), nullptr, nullptr);
	obs_property_button_set_type(help, OBS_BUTTON_URL);
	obs_property_button_set_url(help, const_cast<char *>("https://github.com/daren-wallace/claude/blob/main/"
							     "obs-instrumental-isolator/docs/USER_GUIDE.md"));
	return props;
}

} // namespace

extern "C" void instrumental_isolator_register(void)
{
	obs_source_info info = {};
	info.id = INSTRUMENTAL_ISOLATOR_FILTER_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = ifGetName;
	info.create = ifCreate;
	info.destroy = ifDestroy;
	info.update = ifUpdate;
	info.get_defaults = ifDefaults;
	info.get_properties = ifProperties;
	info.filter_audio = ifFilterAudio;
	obs_register_source(&info);
}
