/*
Instrumental Isolator for OBS - minimal, dependency-free WAV reader/writer
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "wav.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace wav {

namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

uint16_t le16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t *p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
	       (static_cast<uint32_t>(p[3]) << 24);
}

void put16(std::vector<uint8_t> &v, uint16_t x)
{
	v.push_back(static_cast<uint8_t>(x & 0xFF));
	v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
	for (int i = 0; i < 4; ++i)
		v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

void putTag(std::vector<uint8_t> &v, const char *tag)
{
	v.insert(v.end(), tag, tag + 4);
}

int32_t clampToInt(double x, double scale, int32_t lo, int32_t hi)
{
	const double s = std::round(x * scale);
	if (s < lo)
		return lo;
	if (s > hi)
		return hi;
	return static_cast<int32_t>(s);
}

} // namespace

bool decode(const std::vector<uint8_t> &bytes, Audio &out, std::string &error)
{
	if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
	    std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
		error = "not a RIFF/WAVE file";
		return false;
	}

	uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
	uint32_t rate = 0;
	const uint8_t *data = nullptr;
	size_t dataSize = 0;

	size_t pos = 12;
	while (pos + 8 <= bytes.size()) {
		const uint8_t *chunk = bytes.data() + pos;
		const uint32_t size = le32(chunk + 4);
		const size_t bodyStart = pos + 8;
		const size_t available = bytes.size() - bodyStart;
		const size_t bodySize = size > available ? available : size; // tolerate truncated files

		if (std::memcmp(chunk, "fmt ", 4) == 0) {
			if (bodySize < 16) {
				error = "fmt chunk too small";
				return false;
			}
			const uint8_t *f = chunk + 8;
			format = le16(f);
			channels = le16(f + 2);
			rate = le32(f + 4);
			blockAlign = le16(f + 12);
			bits = le16(f + 14);
			if (format == kFormatExtensible && bodySize >= 26)
				format = le16(f + 24); // first two bytes of the SubFormat GUID
		} else if (std::memcmp(chunk, "data", 4) == 0) {
			data = chunk + 8;
			dataSize = bodySize;
		}
		pos = bodyStart + size + (size & 1u); // chunks are word aligned
	}

	if (!channels || !rate || !data) {
		error = "missing fmt or data chunk";
		return false;
	}
	const bool isPcm = format == kFormatPcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
	const bool isFloat = format == kFormatFloat && (bits == 32 || bits == 64);
	if (!isPcm && !isFloat) {
		error = "unsupported sample format (format " + std::to_string(format) + ", " + std::to_string(bits) +
			" bits)";
		return false;
	}
	const size_t bytesPerSample = bits / 8u;
	if (blockAlign < channels * bytesPerSample)
		blockAlign = static_cast<uint16_t>(channels * bytesPerSample);

	const size_t frames = dataSize / blockAlign;
	out.sampleRate = rate;
	out.channels.assign(channels, std::vector<float>(frames));

	for (size_t i = 0; i < frames; ++i) {
		for (size_t c = 0; c < channels; ++c) {
			const uint8_t *s = data + i * blockAlign + c * bytesPerSample;
			double v = 0.0;
			if (isFloat && bits == 32) {
				float f;
				uint32_t u = le32(s);
				std::memcpy(&f, &u, 4);
				v = f;
			} else if (isFloat) {
				double d;
				uint64_t u = static_cast<uint64_t>(le32(s)) |
					     (static_cast<uint64_t>(le32(s + 4)) << 32);
				std::memcpy(&d, &u, 8);
				v = d;
			} else if (bits == 8) {
				v = (static_cast<int>(s[0]) - 128) / 128.0;
			} else if (bits == 16) {
				v = static_cast<int16_t>(le16(s)) / 32768.0;
			} else if (bits == 24) {
				int32_t x = static_cast<int32_t>(static_cast<uint32_t>(s[0]) << 8 |
								 static_cast<uint32_t>(s[1]) << 16 |
								 static_cast<uint32_t>(s[2]) << 24) >>
					    8;
				v = x / 8388608.0;
			} else {
				v = static_cast<int32_t>(le32(s)) / 2147483648.0;
			}
			out.channels[c][i] = static_cast<float>(v);
		}
	}
	return true;
}

std::vector<uint8_t> encode(const Audio &in, SampleFormat format)
{
	const uint16_t channels = static_cast<uint16_t>(in.numChannels());
	uint16_t bits = 32;
	uint16_t tag = kFormatPcm;
	switch (format) {
	case SampleFormat::Pcm16:
		bits = 16;
		break;
	case SampleFormat::Pcm24:
		bits = 24;
		break;
	case SampleFormat::Pcm32:
		bits = 32;
		break;
	case SampleFormat::Float32:
		bits = 32;
		tag = kFormatFloat;
		break;
	}
	const uint16_t blockAlign = static_cast<uint16_t>(channels * (bits / 8));
	const uint32_t dataSize = static_cast<uint32_t>(in.frames() * blockAlign);

	std::vector<uint8_t> v;
	v.reserve(44 + dataSize);
	putTag(v, "RIFF");
	put32(v, 36 + dataSize + (dataSize & 1u));
	putTag(v, "WAVE");
	putTag(v, "fmt ");
	put32(v, 16);
	put16(v, tag);
	put16(v, channels);
	put32(v, in.sampleRate);
	put32(v, in.sampleRate * blockAlign);
	put16(v, blockAlign);
	put16(v, bits);
	putTag(v, "data");
	put32(v, dataSize);

	for (size_t i = 0; i < in.frames(); ++i) {
		for (size_t c = 0; c < channels; ++c) {
			const double x = in.channels[c][i];
			switch (format) {
			case SampleFormat::Pcm16:
				put16(v, static_cast<uint16_t>(clampToInt(x, 32768.0, -32768, 32767)));
				break;
			case SampleFormat::Pcm24: {
				const uint32_t s = static_cast<uint32_t>(clampToInt(x, 8388608.0, -8388608, 8388607));
				v.push_back(static_cast<uint8_t>(s & 0xFF));
				v.push_back(static_cast<uint8_t>((s >> 8) & 0xFF));
				v.push_back(static_cast<uint8_t>((s >> 16) & 0xFF));
				break;
			}
			case SampleFormat::Pcm32:
				put32(v, static_cast<uint32_t>(clampToInt(x, 2147483648.0, INT32_MIN, INT32_MAX)));
				break;
			case SampleFormat::Float32: {
				const float f = static_cast<float>(x);
				uint32_t u;
				std::memcpy(&u, &f, 4);
				put32(v, u);
				break;
			}
			}
		}
	}
	if (dataSize & 1u)
		v.push_back(0);
	return v;
}

bool read(const std::string &path, Audio &out, std::string &error)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "cannot open '" + path + "'";
		return false;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return decode(bytes, out, error);
}

bool write(const std::string &path, const Audio &in, SampleFormat format, std::string &error)
{
	if (in.numChannels() == 0) {
		error = "no audio channels to write";
		return false;
	}
	const std::vector<uint8_t> bytes = encode(in, format);
	std::ofstream f(path, std::ios::binary);
	if (!f) {
		error = "cannot create '" + path + "'";
		return false;
	}
	f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!f) {
		error = "write failed for '" + path + "'";
		return false;
	}
	return true;
}

} // namespace wav
