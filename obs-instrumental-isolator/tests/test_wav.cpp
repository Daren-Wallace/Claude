/*
Instrumental Isolator for OBS - WAV I/O and parameter-table tests
Copyright (C) 2026 Daren Wallace
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "wav.hpp"

#include <cmath>
#include <cstring>
#include <set>
#include <string>

TEST_CASE("WAV encode/decode round-trips every output format", "[wav]")
{
	const auto format = GENERATE(wav::SampleFormat::Pcm16, wav::SampleFormat::Pcm24, wav::SampleFormat::Pcm32,
				     wav::SampleFormat::Float32);
	const int channels = GENERATE(1, 2, 6);

	wav::Audio a;
	a.sampleRate = 44100;
	a.channels.assign(static_cast<size_t>(channels), std::vector<float>(1001));
	for (int c = 0; c < channels; ++c)
		for (size_t i = 0; i < 1001; ++i)
			a.channels[static_cast<size_t>(c)][i] =
				static_cast<float>(0.9 * std::sin(0.01 * static_cast<double>(i) * (c + 1)));

	const auto bytes = wav::encode(a, format);
	wav::Audio b;
	std::string err;
	REQUIRE(wav::decode(bytes, b, err));
	REQUIRE(b.sampleRate == 44100);
	REQUIRE(b.numChannels() == channels);
	REQUIRE(b.frames() == 1001);

	const double tolerance = format == wav::SampleFormat::Pcm16   ? 1.0 / 32768.0
				 : format == wav::SampleFormat::Pcm24 ? 1.0 / 8388608.0
								      : 1e-7;
	for (int c = 0; c < channels; ++c)
		for (size_t i = 0; i < 1001; ++i)
			REQUIRE(std::fabs(b.channels[static_cast<size_t>(c)][i] -
					  a.channels[static_cast<size_t>(c)][i]) <= tolerance);
}

TEST_CASE("WAV integer output clips instead of wrapping", "[wav]")
{
	wav::Audio a;
	a.channels = {{2.0f, -2.0f, 0.0f}};
	wav::Audio b;
	std::string err;
	REQUIRE(wav::decode(wav::encode(a, wav::SampleFormat::Pcm16), b, err));
	CHECK(b.channels[0][0] > 0.999f);
	CHECK(b.channels[0][1] == -1.0f);
	CHECK(b.channels[0][2] == 0.0f);
}

TEST_CASE("WAV decoder skips unknown chunks and rejects garbage", "[wav]")
{
	wav::Audio a;
	a.channels = {{0.5f, -0.25f}};
	auto bytes = wav::encode(a, wav::SampleFormat::Float32);
	// Insert an odd-sized LIST chunk (with pad byte) between fmt and data.
	const std::vector<uint8_t> list = {'L', 'I', 'S', 'T', 3, 0, 0, 0, 'a', 'b', 'c', 0};
	bytes.insert(bytes.begin() + 36, list.begin(), list.end());
	wav::Audio b;
	std::string err;
	REQUIRE(wav::decode(bytes, b, err));
	CHECK(b.channels[0][0] == 0.5f);
	CHECK(b.channels[0][1] == -0.25f);

	CHECK_FALSE(wav::decode({}, b, err));
	CHECK_FALSE(wav::decode(std::vector<uint8_t>(64, 0x42), b, err));
	CHECK_FALSE(err.empty());
}
