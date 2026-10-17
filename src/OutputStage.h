// Output stage of the plug-in wrapper: one Output Level (in dB) followed by a soft limiter. JUCE-free, so the test programs use the very same code.
//
// Why it exists: the firmware's dry/wet crossfade table starts at 0.707 (-3 dB) and its output conversion halves the signal (x * 0.5,
// -6 dB) before the 16-bit output. The module's analogue output stage presumably makes that up; a plug-in has none. Measured with
// Dry/Wet = 0: -9.04 dB in the six modes that have a dry path (see tests/level_test.cpp). The default Output Level below is the
// +9.03 dB (= 20 * log10 (2 * sqrt (2))) that puts such a dry signal back at the input level.
//
// The limiter is a smooth, instantaneous curve (no look-ahead, no delay): signals below the threshold pass unchanged and louder ones
// are bent towards 0 dBFS, which they never exceed. It is a safety net against clipping, not a mastering limiter.
// It has an on/off switch (version 0.3.0, default ON). Switched off, the output is the Output Level gain alone and can go past 0 dBFS
// (only NaN is silenced and the value is kept inside +/-16 so a runaway can never reach the host as infinity). Switching glides over
// about 10 ms, so there is no click.
#pragma once
#include <algorithm>
#include <cmath>

namespace OutputStage
{
    constexpr float kDefaultLevelDb = 9.03f;
    constexpr float kMinLevelDb = -24.0f;
    constexpr float kMaxLevelDb = 24.0f;
    constexpr float kThreshold  = 0.9f;        // linear; about -0.9 dBFS. Everything below passes unchanged.
    constexpr bool  kDefaultLimiterOn = true;
    constexpr float kOffGuard = 16.0f;         // limiter off: values are kept inside +/-16 (about +24 dBFS); NaN becomes 0
    constexpr float kSmoothingSeconds = 0.01f; // time constant of the gain smoothing (no zipper noise when the knob moves or is automated)

    inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }

    // Memoryless soft limiter: identity up to kThreshold, then a tanh knee that approaches (but never reaches) 1.0.
    // The slope is 1 at the threshold, so there is no kink. NaN becomes 0.
    inline float softLimit (float x)
    {
        if (x != x) return 0.0f;
        const float a = std::fabs (x);
        if (a <= kThreshold) return x;
        const float y = kThreshold + (1.0f - kThreshold) * std::tanh ((a - kThreshold) / (1.0f - kThreshold));
        return x < 0.0f ? -y : y;
    }

    // Limiter off: no shaping, only the NaN / runaway guard.
    inline float guard (float x)
    {
        if (x != x) return 0.0f;
        return std::clamp (x, -kOffGuard, kOffGuard);
    }

    class Stage
    {
    public:
        void prepare (double sampleRate)
        {
            coef = 1.0f - std::exp (-1.0f / (kSmoothingSeconds * (float) std::max (sampleRate, 1000.0)));
            snap = true;
            limSnap = true;
        }

        // Sets the target level; the gain glides to it. The first call after prepare() jumps straight there.
        void setLevelDb (float db)
        {
            target = dbToGain (std::clamp (db, kMinLevelDb, kMaxLevelDb));
            if (snap) { gain = target; snap = false; }
        }

        // Limiter on / off. The first call after prepare() jumps straight there; later changes glide.
        void setLimiterOn (bool on)
        {
            limTarget = on ? 1.0f : 0.0f;
            if (limSnap) { limMix = limTarget; limSnap = false; }
        }

        // In place, any number of channels (the gain is shared, so the channels stay matched).
        void process (float* const* channels, int numChannels, int numSamples)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                gain += (target - gain) * coef;
                limMix += (limTarget - limMix) * coef;
                if (std::fabs (limTarget - limMix) < 1.0e-4f) limMix = limTarget;     // settle exactly: fully on is the plain limiter
                for (int c = 0; c < numChannels; ++c)
                {
                    const float y = channels[c][i] * gain;
                    if (limMix >= 1.0f)      channels[c][i] = softLimit (y);
                    else if (limMix <= 0.0f) channels[c][i] = guard (y);
                    else { const float g = guard (y); channels[c][i] = g + limMix * (softLimit (g) - g); }
                }
            }
        }

    private:
        float gain = 1.0f, target = 1.0f, coef = 0.001f;
        float limMix = 1.0f, limTarget = 1.0f;     // 1 = limiter fully on, 0 = off
        bool snap = true, limSnap = true;
    };
}
