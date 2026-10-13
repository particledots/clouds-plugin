// Offline sanity test of CloudsEngine at several host sample rates, modes and qualities.
// It checks that output is finite, bounded and not silent, that freeze keeps sound going after the input stops,
// and reports how fast it runs. It does NOT check that it sounds like the module.
#include "CloudsEngine.h"
#include <chrono>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static const char* kModeNames[] = { "granular", "stretch", "looping-delay", "spectral" };

struct Result { double energy = 0, maxAbs = 0; bool bad = false; double realtimeFactor = 0; };

// Runs `seconds` of a 110 Hz saw (silent when `inputOn` is false after `inputSeconds`), returns energy of the last half.
static Result run (double sr, CloudsEngine::Params p, double seconds, double inputSeconds, double freezeAtSeconds = -1.0)
{
    CloudsEngine e; e.prepare (sr);
    const int N = 512;
    std::vector<float> in (N), outL (N), outR (N);
    double phase = 0;
    Result r;
    const int blocks = (int) (sr * seconds / N);
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        const double t = (double) b * N / sr;
        if (freezeAtSeconds >= 0 && t >= freezeAtSeconds) p.freeze = true;
        const bool on = t < inputSeconds;
        for (int i = 0; i < N; ++i)
        {
            phase += 110.0 / sr; if (phase >= 1.0) phase -= 1.0;
            in[i] = on ? 0.4f * (float) (2.0 * phase - 1.0) : 0.0f;
        }
        e.process (in.data(), in.data(), outL.data(), outR.data(), N, p);
        for (int i = 0; i < N; ++i)
        {
            if (! std::isfinite (outL[i]) || ! std::isfinite (outR[i])) r.bad = true;
            r.maxAbs = std::max (r.maxAbs, (double) std::fabs (outL[i]));
            if (b > blocks / 2) r.energy += outL[i] * outL[i] + outR[i] * outR[i];
        }
    }
    const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    r.realtimeFactor = seconds / elapsed;
    return r;
}

int main()
{
    int fails = 0;
    const double rates[] = { 44100.0, 48000.0, 96000.0 };

    for (double sr : rates)
    {
        // 1) every mode x every quality, wet signal only
        for (int mode = 0; mode < 4; ++mode)
            for (int quality = 0; quality < 4; ++quality)
            {
                CloudsEngine::Params p; p.mode = mode; p.quality = quality; p.dryWet = 1.0f; p.density = 0.8f;
                p.texture = 0.4f; p.size = 0.5f; p.position = 0.4f;
                const Result r = run (sr, p, 6.0, 6.0);
                const bool ok = ! r.bad && r.energy > 0.5 && r.maxAbs <= 1.5;
                printf ("sr=%6.0f %-13s quality=%d  energy=%10.2f max=%.2f  %5.1fx realtime  %s\n",
                        sr, kModeNames[mode], quality, r.energy, r.maxAbs, r.realtimeFactor, ok ? "OK" : "FAIL");
                fails += ! ok;
            }

        // 2) freeze: stop feeding input, the frozen buffer should keep sounding (granular and looping delay)
        for (int mode : { 0, 2 })
        {
            CloudsEngine::Params p; p.mode = mode; p.quality = 0; p.dryWet = 1.0f; p.density = 0.8f; p.position = 0.5f; p.size = 0.6f;
            const Result r = run (sr, p, 8.0, 2.5, 2.4);
            const bool ok = ! r.bad && r.energy > 0.05 && r.maxAbs <= 1.5;
            printf ("sr=%6.0f %-13s freeze, input stopped  energy=%10.2f max=%.2f  %s\n", sr, kModeNames[mode], r.energy, r.maxAbs, ok ? "OK" : "FAIL");
            fails += ! ok;
        }

        // 3) switching mode and quality while running must not crash or produce non-finite output
        {
            CloudsEngine e; e.prepare (sr);
            CloudsEngine::Params p; p.dryWet = 0.7f;
            const int N = 256; std::vector<float> in (N), oL (N), oR (N);
            double phase = 0; bool bad = false;
            const int blocks = (int) (sr * 8 / N);
            for (int b = 0; b < blocks; ++b)
            {
                if (b % 40 == 0) { p.mode = (b / 40) % 4; p.quality = (b / 160) % 4; e.trigger(); }
                for (int i = 0; i < N; ++i) { phase += 220.0 / sr; if (phase >= 1.0) phase -= 1.0; in[i] = 0.4f * (float) (2.0 * phase - 1.0); }
                e.process (in.data(), in.data(), oL.data(), oR.data(), N, p);
                for (int i = 0; i < N; ++i) if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i]) || std::fabs (oL[i]) > 2.0f) bad = true;
            }
            printf ("sr=%6.0f mode/quality switching while running  %s\n", sr, bad ? "FAIL" : "OK");
            fails += bad;
        }

        CloudsEngine e; e.prepare (sr);
        printf ("sr=%6.0f processor rate %.0f, reported latency %d samples\n", sr, e.processorRate(), e.latencyInHostSamples());
    }
    printf (fails ? "\n%d FAILURES\n" : "\nall checks passed\n", fails);
    return fails;
}
