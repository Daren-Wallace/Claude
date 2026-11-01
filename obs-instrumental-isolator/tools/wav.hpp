/*
Instrumental Isolator for OBS - minimal, dependency-free WAV reader/writer
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wav {

enum class SampleFormat {
	Pcm16,
	Pcm24,
	Pcm32,
	Float32,
};

struct Audio {
	uint32_t sampleRate = 48000;
	// Planar samples in [-1, 1]: channels[c][frame].
	std::vector<std::vector<float>> channels;

	size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
	int numChannels() const { return static_cast<int>(channels.size()); }
};

// Reads 8/16/24/32-bit PCM and 32/64-bit float files, including
// WAVE_FORMAT_EXTENSIBLE. Returns false and fills `error` on failure.
bool read(const std::string &path, Audio &out, std::string &error);

// Writes the audio. Integer formats are clipped to full scale.
bool write(const std::string &path, const Audio &in, SampleFormat format, std::string &error);

// In-memory variants (used by the tests).
bool decode(const std::vector<uint8_t> &bytes, Audio &out, std::string &error);
std::vector<uint8_t> encode(const Audio &in, SampleFormat format);

} // namespace wav
