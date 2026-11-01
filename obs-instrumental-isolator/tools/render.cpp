/*
Instrumental Isolator for OBS - offline render / separation report tool
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later

Runs the exact engine used inside OBS on WAV files, and measures separation
quality on a synthetic mix with known ground truth.
*/

#include "dsp/isolator_engine.hpp"
#include "dsp/param_fields.hpp"
#include "dsp/params.hpp"
#include "testmix.hpp"
#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace isolator;

namespace {

void printUsage()
{
	std::printf("isolator-render: run the Instrumental Isolator engine on WAV files.\n"
		    "\n"
		    "Usage:\n"
		    "  isolator-render [options] <input.wav> <output.wav>\n"
		    "  isolator-render --report [options]                  separation report on the test mix\n"
		    "  isolator-render --generate <kind> <out.wav>         kind: testmix | testmix-vibrato\n"
		    "  isolator-render --list-presets | --list-params\n"
		    "\n"
		    "Options:\n"
		    "  --preset <id>        start from a factory preset (default: %s)\n"
		    "  --set key=value      override a parameter, e.g. --set strength=100 (repeatable)\n"
		    "  --mode <m>           instrumental | vocals\n"
		    "  --format <f>         output format: f32 (default) | s16 | s24 | s32\n"
		    "  --rate <hz>          sample rate for --report / --generate (default 48000)\n",
		    kDefaultPresetId);
}

bool applySetting(IsolatorParams &p, const std::string &assignment)
{
	const size_t eq = assignment.find('=');
	if (eq == std::string::npos) {
		std::fprintf(stderr, "error: --set expects key=value, got '%s'\n", assignment.c_str());
		return false;
	}
	const std::string key = assignment.substr(0, eq);
	const std::string value = assignment.substr(eq + 1);
	if (const FloatField *f = findFloatField(key.c_str())) {
		char *end = nullptr;
		const double v = std::strtod(value.c_str(), &end);
		if (end == value.c_str() || *end != '\0') {
			std::fprintf(stderr, "error: '%s' is not a number for %s\n", value.c_str(), key.c_str());
			return false;
		}
		if (v < f->range->min || v > f->range->max)
			std::fprintf(stderr, "warning: %s=%g is outside [%g, %g] and will be clamped\n", key.c_str(), v,
				     f->range->min, f->range->max);
		p.*f->member = v;
		return true;
	}
	if (const BoolField *b = findBoolField(key.c_str())) {
		if (value == "1" || value == "true" || value == "on" || value == "yes")
			p.*b->member = true;
		else if (value == "0" || value == "false" || value == "off" || value == "no")
			p.*b->member = false;
		else {
			std::fprintf(stderr, "error: '%s' is not a boolean for %s\n", value.c_str(), key.c_str());
			return false;
		}
		return true;
	}
	if (key == kOutputModeKey) {
		if (value == "vocals" || value == "1")
			p.outputMode = OutputMode::Vocals;
		else if (value == "instrumental" || value == "0")
			p.outputMode = OutputMode::Instrumental;
		else {
			std::fprintf(stderr, "error: output_mode must be instrumental or vocals\n");
			return false;
		}
		return true;
	}
	std::fprintf(stderr, "error: unknown parameter '%s' (see --list-params)\n", key.c_str());
	return false;
}

void processPlanar(IsolatorEngine &engine, std::vector<std::vector<float>> &channels)
{
	const int nch = static_cast<int>(std::min<size_t>(channels.size(), IsolatorEngine::kMaxChannels));
	const size_t frames = channels.empty() ? 0 : channels[0].size();
	std::vector<float *> ptrs(static_cast<size_t>(nch));
	for (size_t off = 0; off < frames; off += 1024) {
		const size_t n = std::min<size_t>(1024, frames - off);
		for (int c = 0; c < nch; ++c)
			ptrs[static_cast<size_t>(c)] = channels[static_cast<size_t>(c)].data() + off;
		engine.process(ptrs.data(), nch, static_cast<int>(n));
	}
}

wav::SampleFormat parseFormat(const std::string &s, bool &ok)
{
	ok = true;
	if (s == "f32")
		return wav::SampleFormat::Float32;
	if (s == "s16")
		return wav::SampleFormat::Pcm16;
	if (s == "s24")
		return wav::SampleFormat::Pcm24;
	if (s == "s32")
		return wav::SampleFormat::Pcm32;
	ok = false;
	return wav::SampleFormat::Float32;
}

int report(const IsolatorParams &params, const std::string &presetId, uint32_t rate)
{
	std::printf("Separation report: preset '%s'%s, %u Hz test mix (6 s, measured over the last 2 s)\n\n",
		    presetId.c_str(), params.outputMode == OutputMode::Vocals ? " [vocals-only output]" : "", rate);
	for (bool vibrato : {false, true}) {
		const testmix::Mix m = testmix::make(rate, 6.0, vibrato, 1.0);
		IsolatorEngine engine;
		engine.prepare(rate, 2);
		engine.setParams(params);
		engine.snapToTargets();
		std::vector<std::vector<float>> ch = {m.mix.left, m.mix.right};
		processPlanar(engine, ch);
		testmix::Stereo out(ch[0].size());
		out.left = ch[0];
		out.right = ch[1];
		const size_t len = static_cast<size_t>(rate) * 2;
		const size_t start = out.left.size() - len;
		std::printf("  %s vocal\n", vibrato ? "Vibrato" : "Steady");
		std::printf("    Stem      Pan                 Change\n");
		std::printf("    --------  ------------------  --------\n");
		const struct {
			const testmix::Stem *stem;
			const char *pan;
		} rows[] = {{&m.vocalStem, "centre (vocal)"},
			    {&m.guitarStem, "left"},
			    {&m.keysStem, "right"},
			    {&m.padStem, "decorrelated stereo"},
			    {&m.bassStem, "centre (protected)"},
			    {&m.kickStem, "centre (protected)"}};
		for (const auto &r : rows)
			std::printf("    %-8s  %-18s  %+6.1f dB\n", r.stem->name.c_str(), r.pan,
				    testmix::stemChangeDb(*r.stem, m.mix, out, rate, start, len));
		std::printf("    tracked vocal pitch: %.1f Hz (true 220 Hz)\n\n", engine.getTrackedF0());
	}
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	std::string presetId = kDefaultPresetId;
	std::vector<std::string> sets;
	std::vector<std::string> positional;
	wav::SampleFormat format = wav::SampleFormat::Float32;
	uint32_t rate = 48000;
	std::string mode = "render";
	std::string generateKind;
	std::string outputMode;

	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto needValue = [&](const char *opt) -> const char * {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "error: %s needs a value\n", opt);
				std::exit(2);
			}
			return argv[++i];
		};
		if (a == "-h" || a == "--help") {
			printUsage();
			return 0;
		} else if (a == "--preset") {
			presetId = needValue("--preset");
		} else if (a == "--set") {
			sets.emplace_back(needValue("--set"));
		} else if (a == "--mode") {
			outputMode = needValue("--mode");
		} else if (a == "--format") {
			bool ok;
			format = parseFormat(needValue("--format"), ok);
			if (!ok) {
				std::fprintf(stderr, "error: unknown format (use f32, s16, s24 or s32)\n");
				return 2;
			}
		} else if (a == "--rate") {
			const long r = std::strtol(needValue("--rate"), nullptr, 10);
			if (r < 8000 || r > 384000) {
				std::fprintf(stderr, "error: --rate must be between 8000 and 384000\n");
				return 2;
			}
			rate = static_cast<uint32_t>(r);
		} else if (a == "--report") {
			mode = "report";
		} else if (a == "--generate") {
			mode = "generate";
			generateKind = needValue("--generate");
		} else if (a == "--list-presets") {
			mode = "list-presets";
		} else if (a == "--list-params") {
			mode = "list-params";
		} else if (!a.empty() && a[0] == '-') {
			std::fprintf(stderr, "error: unknown option '%s'\n\n", a.c_str());
			printUsage();
			return 2;
		} else {
			positional.push_back(a);
		}
	}

	if (mode == "list-presets") {
		size_t count = 0;
		const Preset *list = presets(count);
		for (size_t i = 0; i < count; ++i)
			std::printf("%-11s%s\n", list[i].id,
				    std::strcmp(list[i].id, kDefaultPresetId) == 0 ? "  (default)" : "");
		return 0;
	}
	if (mode == "list-params") {
		std::printf("%-20s %10s %10s %10s\n", "key", "min", "max", "default");
		for (const FloatField &f : kFloatFields)
			std::printf("%-20s %10g %10g %10g%s\n", f.key, f.range->min, f.range->max, f.range->def,
				    f.suffix);
		for (const BoolField &b : kBoolFields)
			std::printf("%-20s %10s %10s %10s\n", b.key, "off", "on",
				    IsolatorParams{}.*b.member ? "on" : "off");
		std::printf("%-20s %10s %10s %10s\n", kOutputModeKey, "", "", "instrumental|vocals");
		return 0;
	}

	const Preset *preset = findPreset(presetId.c_str());
	if (!preset) {
		std::fprintf(stderr, "error: unknown preset '%s' (see --list-presets)\n", presetId.c_str());
		return 2;
	}
	IsolatorParams params = preset->params;
	if (!outputMode.empty() && !applySetting(params, std::string(kOutputModeKey) + "=" + outputMode))
		return 2;
	for (const std::string &s : sets)
		if (!applySetting(params, s))
			return 2;

	if (mode == "report")
		return report(params, presetId, rate);

	if (mode == "generate") {
		if (positional.size() != 1 || (generateKind != "testmix" && generateKind != "testmix-vibrato")) {
			std::fprintf(stderr, "error: use --generate testmix|testmix-vibrato <out.wav>\n");
			return 2;
		}
		const testmix::Mix m = testmix::make(rate, 10.0, generateKind == "testmix-vibrato", 1.5);
		wav::Audio a;
		a.sampleRate = rate;
		a.channels = {m.mix.left, m.mix.right};
		std::string err;
		if (!wav::write(positional[0], a, format, err)) {
			std::fprintf(stderr, "error: %s\n", err.c_str());
			return 1;
		}
		// Ground-truth instrumental, for A/B listening against the result.
		const testmix::Stereo inst = m.instrumental();
		a.channels = {inst.left, inst.right};
		std::string refPath = positional[0];
		const size_t dot = refPath.rfind(".wav");
		refPath = (dot == std::string::npos ? refPath : refPath.substr(0, dot)) + ".reference-instrumental.wav";
		if (!wav::write(refPath, a, format, err)) {
			std::fprintf(stderr, "error: %s\n", err.c_str());
			return 1;
		}
		std::printf("wrote %s and %s\n", positional[0].c_str(), refPath.c_str());
		return 0;
	}

	if (positional.size() != 2) {
		printUsage();
		return 2;
	}
	wav::Audio audio;
	std::string err;
	if (!wav::read(positional[0], audio, err)) {
		std::fprintf(stderr, "error: %s\n", err.c_str());
		return 1;
	}
	IsolatorEngine engine;
	const int centre = audio.numChannels() >= 6 ? 2 : -1;
	engine.prepare(audio.sampleRate, audio.numChannels(), centre);
	engine.setParams(params);
	engine.snapToTargets();
	processPlanar(engine, audio.channels);
	if (!wav::write(positional[1], audio, format, err)) {
		std::fprintf(stderr, "error: %s\n", err.c_str());
		return 1;
	}
	std::printf("Rendered '%s' -> '%s' (%d ch, %u Hz) with preset '%s'%s. Tracked vocal pitch at end: %.1f Hz\n",
		    positional[0].c_str(), positional[1].c_str(), audio.numChannels(), audio.sampleRate,
		    presetId.c_str(), audio.numChannels() == 1 ? " [mono: pitch remover only]" : "",
		    engine.getTrackedF0());
	return 0;
}
