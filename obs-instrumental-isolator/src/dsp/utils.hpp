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

// Small real-time helpers: parameter smoothing, envelope following and
// denormal protection.

#pragma once

#include <cmath>
#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define ISOLATOR_HAS_SSE_CSR 1
#endif

namespace isolator {
namespace dsp {

// Coefficient for a one-pole filter that reaches ~63% of a step after
// `timeMs` milliseconds.
inline double onePoleCoeff(double sampleRate, double timeMs)
{
	if (timeMs <= 0.0)
		return 0.0;
	return std::exp(-1.0 / (timeMs * 0.001 * sampleRate));
}

// Exponential (one-pole) parameter smoother. Removes "zipper" noise when a
// slider moves while audio is running.
class Smoother {
public:
	void setTimeConstant(double sampleRate, double timeMs) { coeff = onePoleCoeff(sampleRate, timeMs); }

	void setTarget(double t) { target = t; }
	void snap(double v)
	{
		target = v;
		current = v;
	}

	inline double next()
	{
		current = target + (current - target) * coeff;
		if (std::fabs(current - target) < 1e-9)
			current = target;
		return current;
	}

	// Advance `n` samples at once (used for block-rate filter parameters).
	inline double advance(int n)
	{
		current = target + (current - target) * std::pow(coeff, n);
		if (std::fabs(current - target) < 1e-6 * (1.0 + std::fabs(target)))
			current = target;
		return current;
	}

	double value() const { return current; }
	double getTarget() const { return target; }
	bool settled() const { return current == target; }

private:
	double coeff = 0.0;
	double current = 0.0;
	double target = 0.0;
};

// Linear ramp between 0 and 1, used for the click-free bypass crossfade.
class LinearRamp {
public:
	void setRampTime(double sampleRate, double timeMs)
	{
		const double samples = timeMs * 0.001 * sampleRate;
		step = samples > 1.0 ? 1.0 / samples : 1.0;
	}

	void setTarget(double t) { target = t; }
	void snap(double v)
	{
		target = v;
		current = v;
	}

	inline double next()
	{
		if (current < target) {
			current += step;
			if (current > target)
				current = target;
		} else if (current > target) {
			current -= step;
			if (current < target)
				current = target;
		}
		return current;
	}

	double value() const { return current; }
	double getTarget() const { return target; }
	bool settled() const { return current == target; }

private:
	double step = 1.0;
	double current = 0.0;
	double target = 0.0;
};

// Peak envelope follower with independent attack and release times.
class EnvelopeFollower {
public:
	void setup(double sampleRate, double attackMs, double releaseMs)
	{
		attack = onePoleCoeff(sampleRate, attackMs);
		release = onePoleCoeff(sampleRate, releaseMs);
	}

	void reset() { env = 0.0; }

	inline double process(double x)
	{
		const double a = std::fabs(x);
		const double c = a > env ? attack : release;
		env = a + (env - a) * c;
		if (env < 1e-30)
			env = 0.0;
		return env;
	}

	double value() const { return env; }

private:
	double attack = 0.0;
	double release = 0.0;
	double env = 0.0;
};

// RAII guard enabling flush-to-zero / denormals-are-zero for the current
// thread while audio is processed, restoring the previous mode afterwards.
class ScopedFlushDenormals {
public:
	ScopedFlushDenormals()
	{
#if defined(ISOLATOR_HAS_SSE_CSR)
		saved = _mm_getcsr();
		_mm_setcsr(saved | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
		uint64_t fpcr;
		__asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
		saved = fpcr;
		fpcr |= (1ull << 24); // FZ
		__asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
	}

	~ScopedFlushDenormals()
	{
#if defined(ISOLATOR_HAS_SSE_CSR)
		_mm_setcsr(saved);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
		uint64_t fpcr = saved;
		__asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
	}

	ScopedFlushDenormals(const ScopedFlushDenormals &) = delete;
	ScopedFlushDenormals &operator=(const ScopedFlushDenormals &) = delete;

private:
#if defined(ISOLATOR_HAS_SSE_CSR)
	unsigned int saved = 0;
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
	uint64_t saved = 0;
#endif
};

} // namespace dsp
} // namespace isolator
