// Pitch Snap (our addition): the PITCH knob's value snaps to octaves and fifths (Steps) or whole semitones (Semitones).
//  1. the snapping function: exact over the knob's whole +-48 range
//  2. through the engine (Pitch shifter mode): the output pitch follows the snapped value, measured on a sine
//  3. every mode stays finite and bounded with each snap setting and a sweeping knob
#include "CloudsEngine.h"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

using E = CloudsEngine;

static double outputRatio (double sr, float knob, int snap)
{
    // 220 Hz sine through the Pitch shifter at full wet; the pitch of the last two seconds from upward zero crossings
    E e; e.prepare (sr);
    E::Params p; p.mode = E::PitchShifterStretch; p.pitchSemitones = knob; p.pitchSnap = snap; p.dryWet = 1.0f; p.reverb = 0.0f; p.feedback = 0.0f;
    const int N = 512; std::vector<float> in (N), oL (N), oR (N), all;
    double phase = 0;
    const int blocks = (int) (sr * 4.0 / N);
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < N; ++i) in[(size_t) i] = 0.5f * (float) std::sin (phase += 2.0 * M_PI * 220.0 / sr);
        e.process (in.data(), in.data(), oL.data(), oR.data(), N, p);
        if (b >= blocks / 2) all.insert (all.end(), oL.begin(), oL.end());
    }
    int c = 0; for (size_t i = 1; i < all.size(); ++i) c += (all[i - 1] < 0.f && all[i] >= 0.f);
    return (c * sr / (double) all.size()) / 220.0;
}

int main()
{
    int fails = 0;

    // 1. the function
    {
        static const float steps[] = { 0, 7, 12, 19, 24, 31, 36, 43, 48 };
        bool ok = true; float prev = -100.f; int distinct = 0; float last = 1e9f;
        for (int i = -4800; i <= 4800; ++i)
        {
            const float k = (float) i * 0.01f;
            const float st = E::snapPitch (k, E::SnapSteps);
            float bestDist = 1e9f; for (float s : steps) bestDist = std::min (bestDist, std::fabs (std::fabs (k) - s));
            bool onStep = false; for (float s : steps) onStep = onStep || std::fabs (std::fabs (st) - s) < 1e-6f;
            ok = ok && onStep && st >= prev - 1e-6f && std::fabs (std::fabs (st) - std::fabs (k)) <= bestDist + 1e-4f && (st == 0.f || (st < 0.f) == (k < 0.f))
                    && std::fabs (E::snapPitch (k, E::SnapSemitones) - std::round (k)) < 1e-6f
                    && E::snapPitch (k, E::SnapOff) == k;
            prev = st; if (st != last) { ++distinct; last = st; }
        }
        ok = ok && distinct == 17;
        printf ("snap function over -48..+48: Steps visit exactly 17 values (%d seen), the nearest one, never backwards; Semitones round; Off unchanged  %s\n", distinct, ok ? "OK" : "FAIL");
        fails += ! ok;
    }

    // 2. through the engine
    {
        struct C { float knob; int snap; };
        const C cs[] = { { 5.4f, E::SnapOff }, { 5.4f, E::SnapSteps }, { 5.4f, E::SnapSemitones },
                         { -9.3f, E::SnapSteps }, { -9.3f, E::SnapSemitones }, { 14.6f, E::SnapSteps }, { 14.6f, E::SnapSemitones } };
        bool ok = true;
        for (const auto& c : cs)
        {
            const double expected = E::snapPitch (c.knob, c.snap);
            const double got = 12.0 * std::log2 (outputRatio (48000.0, c.knob, c.snap));
            const bool good = std::fabs (got - expected) < 0.25;
            ok = ok && good;
            printf ("  engine: knob %+.2f st, snap %d -> expected %+.2f st, measured %+.2f st %s\n", c.knob, c.snap, expected, got, good ? "" : " <-- off");
        }
        printf ("pitch shifter output follows the snapped value (sine, 48000 Hz host)  %s\n", ok ? "OK" : "FAIL");
        fails += ! ok;
    }

    // 3. finite and bounded in every mode with each snap setting, a sweeping knob and a MIDI offset
    {
        bool ok = true;
        for (int mode = 0; mode < 4 && ok; ++mode)
            for (int snap = 0; snap < 3 && ok; ++snap)
            {
                E e; e.prepare (48000.0);
                E::Params p; p.mode = mode; p.pitchSnap = snap; p.dryWet = 0.8f; p.pitchOffsetSemitones = 3.0f;
                const int N = 480; std::vector<float> in (N), oL (N), oR (N); double phase = 0;
                for (int b = 0; b < 300 && ok; ++b)
                {
                    p.pitchSemitones = 48.0f * (float) std::sin (b * 0.05);
                    if (b % 50 == 0) e.trigger();
                    for (int i = 0; i < N; ++i) in[(size_t) i] = 0.4f * (float) std::sin (phase += 2.0 * M_PI * 220.0 / 48000.0);
                    e.process (in.data(), in.data(), oL.data(), oR.data(), N, p);
                    for (int i = 0; i < N; ++i) if (! std::isfinite (oL[(size_t) i]) || ! std::isfinite (oR[(size_t) i]) || std::fabs (oL[(size_t) i]) > 2.0f) ok = false;
                }
            }
        printf ("all modes x 3 snap settings, sweeping knob + MIDI offset: finite and bounded  %s\n", ok ? "OK" : "FAIL");
        fails += ! ok;
    }

    printf (fails ? "\n%d FAILURES\n" : "\nall pitch snap checks passed\n", fails);
    return fails;
}
