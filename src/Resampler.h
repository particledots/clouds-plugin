// Small windowed-sinc streaming resampler (stereo-agnostic: one instance per channel).
#pragma once
#include <vector>
#include <cmath>
#include <cstddef>

class SincResampler
{
public:
    static constexpr int kTaps = 32;       // kernel width in input samples
    static constexpr int kPhases = 1024;   // fractional-position resolution

    // ratio = inRate / outRate (input samples consumed per output sample)
    void init (double inRate, double outRate)
    {
        step = inRate / outRate;
        bypass = std::abs (inRate - outRate) < 1e-6;
        const double cutoff = std::min (1.0, outRate / inRate) * 0.97;   // anti-alias when downsampling
        table.assign ((size_t) kPhases * (size_t) kTaps, 0.0f);
        const double pi = 3.14159265358979323846;
        for (int p = 0; p < kPhases; ++p)
        {
            const double frac = (double) p / (double) kPhases;
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double d = (double) (k - (kTaps / 2 - 1)) - frac;   // distance from output position
                const double x = d * cutoff;
                const double s = std::abs (x) < 1e-9 ? 1.0 : std::sin (pi * x) / (pi * x);
                const double w = 0.42 + 0.5 * std::cos (pi * d / (kTaps / 2)) + 0.08 * std::cos (2.0 * pi * d / (kTaps / 2));
                const double h = s * w * cutoff;
                table[(size_t) p * (size_t) kTaps + (size_t) k] = (float) h;
                sum += h;
            }
            for (int k = 0; k < kTaps; ++k)    // normalise DC gain per phase
                table[(size_t) p * (size_t) kTaps + (size_t) k] /= (float) sum;
        }
        reset();
    }

    void reset()
    {
        hist.assign ((size_t) kTaps, 0.0f);
        pos = 0.0;
    }

    // Appends input to internal history, emits as many outputs as possible.
    // Returns number of outputs appended to `out`.
    size_t process (const float* in, size_t n, std::vector<float>& out)
    {
        if (bypass) { out.insert (out.end(), in, in + n); return n; }
        hist.insert (hist.end(), in, in + n);
        size_t produced = 0;
        // output sample needs hist[i .. i+kTaps-1] with i = floor(pos)
        while ((size_t) pos + (size_t) kTaps <= hist.size())
        {
            const size_t i = (size_t) pos;
            const double frac = pos - (double) i;
            int ph = (int) (frac * (double) kPhases);
            if (ph >= kPhases) ph = kPhases - 1;
            const float* h = &table[(size_t) ph * (size_t) kTaps];
            const float* x = &hist[i];
            float acc = 0.0f;
            for (int k = 0; k < kTaps; ++k) acc += h[k] * x[k];
            out.push_back (acc);
            pos += step;
            ++produced;
        }
        // drop consumed history
        const size_t drop = (size_t) pos;
        if (drop > 0)
        {
            hist.erase (hist.begin(), hist.begin() + (long) drop);
            pos -= (double) drop;
        }
        return produced;
    }

    int latencyInputSamples() const { return bypass ? 0 : kTaps / 2; }

private:
    std::vector<float> table, hist;
    double step = 1.0, pos = 0.0;
    bool bypass = true;
};
