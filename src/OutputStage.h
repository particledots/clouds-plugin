// Output stage of the plug-in wrapper: one Output Level (in dB) followed by a soft limiter. JUCE-free, so the test programs use the very same code.
//
// Why it exists: the firmware's dry/wet crossfade table starts at 0.707 (-3 dB) and its output conversion halves the signal (x * 0.5,
// -6 dB) before the 16-bit output. The module's analogue output stage presumably makes that up; a plug-in has none. Measured with
// Dry/Wet = 0: -9.04 dB in the six modes that have a dry path (see tests/level_test.cpp). The default Output Level below is the
// +9.03 dB (= 20 * log10 (2 * sqrt (2))) that puts such a dry signal back at the input level.
//
// The limiter is a smooth, instantaneous curve (no look-ahead, no delay): signals below the threshold pass unchanged and louder ones
// are bent towards 0 dBFS, which they never exceed. It is a safety net against clipping, not a mastering limiter.
#pragma once
#include <algorithm>
#include <cmath>

namespace OutputStage
{
    constexpr float kDefaultLevelDb = 9.03f;
    constexpr float kMinLevelDb = -24.0f;
    constexpr float kMaxLevelDb = 24.0f;
    constexpr float kThreshold  = 0.9f;        // linear; about -0.9 dBFS. Everything below passes unchanged.
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

    class Stage
    {
    public:
        void prepare (double sampleRate)
        {
            coef = 1.0f - std::exp (-1.0f / (kSmoothingSeconds * (float) std::max (sampleRate, 1000.0)));
            snap = true;
        }

        // Sets the target level; the gain glides to it. The first call after prepare() jumps straight there.
        void setLevelDb (float db)
        {
            target = dbToGain (std::clamp (db, kMinLevelDb, kMaxLevelDb));
            if (snap) { gain = target; snap = false; }
        }

        // In place, any number of channels (the gain is shared, so the channels stay matched).
        void process (float* const* channels, int numChannels, int numSamples)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                gain += (target - gain) * coef;
                for (int c = 0; c < numChannels; ++c)
                    channels[c][i] = softLimit (channels[c][i] * gain);
            }
        }

    private:
        float gain = 1.0f, target = 1.0f, coef = 0.001f;
        bool snap = true;
    };
}
