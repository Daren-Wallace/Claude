/*
Bass Booster for OBS - offline render / analysis tool
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later

Runs the exact DSP engine used inside OBS on a WAV file, so settings can be
auditioned and compared offline, and prints a before/after band report.
*/

#include "dsp/bass_engine.hpp"
#include "dsp/biquad.hpp"
#include "dsp/param_fields.hpp"
#include "dsp/params.hpp"
#include "wav.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace bassbooster;

namespace {

void printUsage()
{
	std::printf(
		"bass-booster-render: run the Bass Booster DSP on WAV files.\n"
		"\n"
		"Usage:\n"
		"  bass-booster-render [options] <input.wav> <output.wav>\n"
		"  bass-booster-render --analyze <file.wav>\n"
		"  bass-booster-render --response [options]           (CSV: freq_hz,gain_db)\n"
		"  bass-booster-render --generate <kind> <out.wav>    (kind: phone-voice | full-voice | bass-sweep)\n"
		"  bass-booster-render --list-presets | --list-params\n"
		"\n"
		"Options:\n"
		"  --preset <id>        start from a factory preset (default: %s)\n"
		"  --set key=value      override a parameter, e.g. --set shelf_gain_db=10 (repeatable)\n"
		"  --format <f>         output format: f32 (default) | s16 | s24 | s32\n"
		"  --rate <hz>          sample rate for --response / --generate (default 48000)\n"
		"  --quiet              do not print the band report\n",
		kDefaultPresetId);
}

bool applySetting(BassParams &p, const std::string &assignment)
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
		if (value == "1" || value == "true" || value == "on" || value == "yes") {
			p.*b->member = true;
		} else if (value == "0" || value == "false" || value == "off" || value == "no") {
			p.*b->member = false;
		} else {
			std::fprintf(stderr, "error: '%s' is not a boolean for %s\n", value.c_str(), key.c_str());
			return false;
		}
		return true;
	}
	std::fprintf(stderr, "error: unknown parameter '%s' (see --list-params)\n", key.c_str());
	return false;
}

// RMS level per frequency band, in dBFS, of the mono sum.
struct BandReport {
	static constexpr int kBands = 6;
	static constexpr const char *kNames[kBands] = {"   < 60 Hz", " 60-120 Hz", "120-250 Hz",
						       "250-500 Hz", "500-2k  Hz", "    > 2 kHz"};
	double db[kBands] = {};
	double peakDb = -200.0;
	double totalDb = -200.0;
};

BandReport analyze(const wav::Audio &a)
{
	using dsp::Biquad;
	using dsp::BiquadCoeffs;
	const double fs = a.sampleRate;
	const double edges[BandReport::kBands + 1] = {0.0, 60.0, 120.0, 250.0, 500.0, 2000.0, fs / 2.0};

	BandReport r;
	std::vector<double> mono(a.frames(), 0.0);
	double peak = 0.0, total = 0.0;
	for (size_t i = 0; i < a.frames(); ++i) {
		for (const auto &ch : a.channels) {
			mono[i] += ch[i];
			peak = std::fmax(peak, std::fabs(static_cast<double>(ch[i])));
		}
		mono[i] /= a.numChannels();
		total += mono[i] * mono[i];
	}
	const double n = a.frames() ? static_cast<double>(a.frames()) : 1.0;
	r.peakDb = 20.0 * std::log10(peak > 1e-10 ? peak : 1e-10);
	r.totalDb = 10.0 * std::log10(total / n > 1e-20 ? total / n : 1e-20);

	for (int b = 0; b < BandReport::kBands; ++b) {
		// 4th-order band split: cascaded Butterworth high-pass and low-pass.
		Biquad hp1, hp2, lp1, lp2;
		const bool useHp = edges[b] > 0.0;
		const bool useLp = edges[b + 1] < fs / 2.0;
		if (useHp) {
			hp1.setCoeffs(BiquadCoeffs::highPass(fs, edges[b], dsp::kButterworth4Q1));
			hp2.setCoeffs(BiquadCoeffs::highPass(fs, edges[b], dsp::kButterworth4Q2));
		}
		if (useLp) {
			lp1.setCoeffs(BiquadCoeffs::lowPass(fs, edges[b + 1], dsp::kButterworth4Q1));
			lp2.setCoeffs(BiquadCoeffs::lowPass(fs, edges[b + 1], dsp::kButterworth4Q2));
		}
		double acc = 0.0;
		for (double x : mono) {
			double y = x;
			if (useHp)
				y = hp2.process(hp1.process(y));
			if (useLp)
				y = lp2.process(lp1.process(y));
			acc += y * y;
		}
		r.db[b] = 10.0 * std::log10(acc / n > 1e-20 ? acc / n : 1e-20);
	}
	return r;
}

void printReport(const BandReport *before, const BandReport &after)
{
	if (before) {
		std::printf("\n  Band          Before     After    Change\n");
		std::printf("  ----------  --------  --------  --------\n");
		for (int b = 0; b < BandReport::kBands; ++b)
			std::printf("  %s  %6.1f dB %6.1f dB  %+6.1f dB\n", BandReport::kNames[b], before->db[b],
				    after.db[b], after.db[b] - before->db[b]);
		std::printf("  Overall     %6.1f dB %6.1f dB  %+6.1f dB\n", before->totalDb, after.totalDb,
			    after.totalDb - before->totalDb);
		std::printf("  Peak        %6.1f dB %6.1f dB\n\n", before->peakDb, after.peakDb);
	} else {
		std::printf("\n  Band          Level (RMS dBFS)\n");
		for (int b = 0; b < BandReport::kBands; ++b)
			std::printf("  %s  %6.1f dB\n", BandReport::kNames[b], after.db[b]);
		std::printf("  Overall     %6.1f dB\n  Peak        %6.1f dB\n\n", after.totalDb, after.peakDb);
	}
}

// Test material. "phone-voice" is a sung vowel at a male speaking pitch with
// everything below 300 Hz removed, the classic "no bass at all" input.
wav::Audio generate(const std::string &kind, uint32_t rate, bool &ok)
{
	wav::Audio a;
	a.sampleRate = rate;
	const double fs = rate;
	const size_t frames = static_cast<size_t>(fs * 6.0);
	a.channels.assign(2, std::vector<float>(frames, 0.0f));
	ok = true;

	if (kind == "phone-voice" || kind == "full-voice") {
		const bool phone = kind == "phone-voice";
		dsp::Butterworth4 hp;
		hp.setup(dsp::Butterworth4::Type::HighPass, fs, 300.0);
		double phase = 0.0;
		for (size_t i = 0; i < frames; ++i) {
			const double t = static_cast<double>(i) / fs;
			// Male speaking pitch with gentle vibrato and a syllable-rate envelope.
			const double f0 = 115.0 * (1.0 + 0.01 * std::sin(2.0 * dsp::kPi * 5.0 * t));
			const double syllable = 0.55 + 0.45 * std::sin(2.0 * dsp::kPi * 2.0 * t - 1.2);
			phase += 2.0 * dsp::kPi * f0 / fs;
			if (phase > 2.0 * dsp::kPi)
				phase -= 2.0 * dsp::kPi;
			double s = 0.0;
			for (int h = 1; h <= 30; ++h) {
				// Vowel-like spectral envelope: chest resonance plus formants
				// near 700 Hz and 1200 Hz.
				const double fh = f0 * h;
				const double formant = 1.0 / (1.0 + std::pow((fh - 700.0) / 250.0, 2)) +
						       0.6 / (1.0 + std::pow((fh - 1200.0) / 300.0, 2)) +
						       0.8 / (1.0 + std::pow(fh / 250.0, 2));
				s += formant * std::sin(h * phase + 0.7 * h);
			}
			s *= 0.08 * syllable;
			const double y = phone ? hp.process(s) : s;
			a.channels[0][i] = static_cast<float>(y);
			a.channels[1][i] = static_cast<float>(y);
		}
	} else if (kind == "bass-sweep") {
		// Log sine sweep 20 Hz -> 20 kHz at -12 dBFS.
		const double f1 = 20.0, f2 = 20000.0, T = 6.0;
		const double k = std::log(f2 / f1);
		for (size_t i = 0; i < frames; ++i) {
			const double t = static_cast<double>(i) / fs;
			const double phase = 2.0 * dsp::kPi * f1 * T / k * (std::exp(t / T * k) - 1.0);
			const float v = static_cast<float>(0.25 * std::sin(phase));
			a.channels[0][i] = v;
			a.channels[1][i] = v;
		}
	} else {
		ok = false;
	}
	return a;
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

} // namespace

int main(int argc, char **argv)
{
	std::string presetId = kDefaultPresetId;
	std::vector<std::string> sets;
	std::vector<std::string> positional;
	wav::SampleFormat format = wav::SampleFormat::Float32;
	uint32_t rate = 48000;
	bool quiet = false;
	std::string mode = "render";
	std::string generateKind;

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
		} else if (a == "--quiet") {
			quiet = true;
		} else if (a == "--analyze") {
			mode = "analyze";
		} else if (a == "--response") {
			mode = "response";
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
			std::printf("%-10s%s\n", list[i].id,
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
				    BassParams{}.*b.member ? "on" : "off");
		return 0;
	}

	const Preset *preset = findPreset(presetId.c_str());
	if (!preset) {
		std::fprintf(stderr, "error: unknown preset '%s' (see --list-presets)\n", presetId.c_str());
		return 2;
	}
	BassParams params = preset->params;
	for (const std::string &s : sets) {
		if (!applySetting(params, s))
			return 2;
	}

	if (mode == "generate") {
		if (positional.size() != 1) {
			std::fprintf(stderr, "error: --generate needs exactly one output file\n");
			return 2;
		}
		bool ok;
		const wav::Audio a = generate(generateKind, rate, ok);
		if (!ok) {
			std::fprintf(stderr, "error: unknown signal '%s'\n", generateKind.c_str());
			return 2;
		}
		std::string err;
		if (!wav::write(positional[0], a, format, err)) {
			std::fprintf(stderr, "error: %s\n", err.c_str());
			return 1;
		}
		return 0;
	}

	if (mode == "response") {
		BassEngine engine;
		engine.prepare(rate, 1);
		engine.setParams(params);
		std::printf("freq_hz,gain_db\n");
		for (double f = 10.0; f <= 20000.0; f *= std::pow(2.0, 1.0 / 24.0))
			std::printf("%.2f,%.3f\n", f, engine.linearResponseDb(f));
		return 0;
	}

	if (mode == "analyze") {
		if (positional.size() != 1) {
			std::fprintf(stderr, "error: --analyze needs exactly one file\n");
			return 2;
		}
		wav::Audio a;
		std::string err;
		if (!wav::read(positional[0], a, err)) {
			std::fprintf(stderr, "error: %s\n", err.c_str());
			return 1;
		}
		std::printf("%s: %d ch, %u Hz, %.2f s\n", positional[0].c_str(), a.numChannels(), a.sampleRate,
			    static_cast<double>(a.frames()) / a.sampleRate);
		printReport(nullptr, analyze(a));
		return 0;
	}

	// Render.
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
	const BandReport before = analyze(audio);

	BassEngine engine;
	engine.prepare(audio.sampleRate, audio.numChannels());
	engine.setParams(params);
	engine.snapToTargets();

	const int channels = audio.numChannels() < BassEngine::kMaxChannels ? audio.numChannels()
									    : BassEngine::kMaxChannels;
	std::vector<float *> ptrs(static_cast<size_t>(channels));
	const size_t block = 1024; // same buffer size OBS uses
	for (size_t off = 0; off < audio.frames(); off += block) {
		const size_t n = (audio.frames() - off) < block ? (audio.frames() - off) : block;
		for (int c = 0; c < channels; ++c)
			ptrs[static_cast<size_t>(c)] = audio.channels[static_cast<size_t>(c)].data() + off;
		engine.process(ptrs.data(), channels, static_cast<int>(n));
	}

	if (!wav::write(positional[1], audio, format, err)) {
		std::fprintf(stderr, "error: %s\n", err.c_str());
		return 1;
	}
	if (!quiet) {
		std::printf("Rendered '%s' -> '%s' with preset '%s'%s\n", positional[0].c_str(), positional[1].c_str(),
			    presetId.c_str(), sets.empty() ? "" : " + overrides");
		printReport(&before, analyze(audio));
	}
	return 0;
}
