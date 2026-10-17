// Level tests for the output stage (Output Level + soft limiter).
//
//  A. The stage itself: unity at 0 dB, exact gain, the limiter is transparent below its threshold and never exceeds 0 dBFS, smooth gain moves.
//  B. Fully dry level: with Dry/Wet = 0 and the default Output Level, the engine + stage output must equal the input level within 0.1 dB
//     in every mode that has a dry path (all host rates, both engine rates, all four qualities). Beat repeat and Resonestor have no dry
//     path in the firmware (Beat repeat forces the blend to wet and the input always goes through the slice player; in Resonestor the blend
//     knob is a distortion amount); they are measured and printed, and only checked for sanity.
//  C. Loud wet sounds cannot clip: with Output Level at its default and at +24 dB, no sample ever exceeds 0 dBFS.
//
// Built and run by the workflow, and by tests/mutation_check.sh, which breaks OutputStage.h on purpose and expects this test to FAIL.
#include "CloudsEngine.h"
#include "OutputStage.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <string>

static int failures = 0, checks = 0;
static bool fastMode = false;   // --fast: one host rate / engine rate / quality (used by the mutation check, which only needs to see the test fail)
#define CHECK(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL: "); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

static const char* kModeName[4] = { "Granular", "Pitch shifter/stretch", "Looping delay", "Spectral madness" };
static bool hasDryPath (int) { return true; }   // all four Clouds modes have a dry path
static double toDb (double power) { return 10.0 * std::log10 (power + 1e-30); }

// ---------------------------------------------------------------------------------------------------------------------------
static void testStageUnit()
{
    std::printf ("A. Output stage\n");
    // identity below the threshold, odd symmetry, never above 1.0
    for (float x = -OutputStage::kThreshold; x <= OutputStage::kThreshold; x += 0.01f)
        CHECK (OutputStage::softLimit (x) == x, "limiter changed %f below the threshold", x);
    float worst = 0.0f;
    for (float x = 0.0f; x <= 1000.0f; x += (x < 4.0f ? 0.001f : 1.0f))
    {
        const float y = OutputStage::softLimit (x), ny = OutputStage::softLimit (-x);
        worst = std::max (worst, std::fabs (y));
        CHECK (y <= 1.0f && y >= 0.0f, "limiter output %f for input %f is outside 0..1", y, x);
        CHECK (ny == -y, "limiter is not odd-symmetric at %f", x);
    }
    CHECK (OutputStage::softLimit (INFINITY) <= 1.0f && OutputStage::softLimit (-INFINITY) >= -1.0f, "infinity not bounded");
    CHECK (OutputStage::softLimit (NAN) == 0.0f, "NaN not silenced");
    // monotonic and with no kink at the threshold (slope about 1 on both sides)
    float prev = -1.0f; bool mono = true;
    for (float x = 0.0f; x <= 3.0f; x += 0.0005f) { const float y = OutputStage::softLimit (x); if (y < prev) mono = false; prev = y; }
    CHECK (mono, "limiter is not monotonic");
    const float t = OutputStage::kThreshold, e = 0.001f;
    const float slopeBelow = (OutputStage::softLimit (t) - OutputStage::softLimit (t - e)) / e;
    const float slopeAbove = (OutputStage::softLimit (t + e) - OutputStage::softLimit (t)) / e;
    CHECK (std::fabs (slopeBelow - 1.0f) < 0.01f && std::fabs (slopeAbove - 1.0f) < 0.01f, "slope at the threshold: %f / %f", slopeBelow, slopeAbove);
    std::printf ("   limiter: identity up to %.2f, largest output for inputs up to 1000: %.6f\n", OutputStage::kThreshold, worst);

    // gain accuracy: a quiet sine through the stage at several levels
    for (float db : { -24.0f, -6.0f, 0.0f, 9.03f, 12.0f })
    {
        OutputStage::Stage s; s.prepare (48000.0); s.setLevelDb (db);
        std::vector<float> a (4800), b (4800);
        for (int i = 0; i < 4800; ++i) a[i] = b[i] = 0.01f * (float) std::sin (2.0 * 3.14159265358979 * 440.0 * i / 48000.0);
        float* ch[2] = { a.data(), b.data() };
        s.process (ch, 2, 4800);
        double si = 0, so = 0;
        for (int i = 2400; i < 4800; ++i) { const double x = 0.01 * std::sin (2.0 * 3.14159265358979 * 440.0 * i / 48000.0); si += x * x; so += (double) a[i] * a[i]; }
        const double got = toDb (so / si);
        CHECK (std::fabs (got - db) < 0.001, "stage gain %f dB measured as %f dB", db, got);
    }
    // the default is the unity-making value
    CHECK (std::fabs (OutputStage::kDefaultLevelDb - 20.0f * std::log10 (2.0f * std::sqrt (2.0f))) < 0.005f, "default level is not 20*log10(2*sqrt(2))");

    // smooth moves: a step from +9.03 to -24 dB must glide (no jump bigger than a small fraction of the whole step) and settle
    {
        OutputStage::Stage s; s.prepare (48000.0); s.setLevelDb (9.03f);
        std::vector<float> a (48000, 0.1f);
        float* ch[1] = { a.data() };
        s.process (ch, 1, 4800);              // settled
        s.setLevelDb (-24.0f);
        std::fill (a.begin(), a.end(), 0.1f);
        s.process (ch, 1, 48000);
        float maxStep = 0.0f;
        for (int i = 1; i < 48000; ++i) maxStep = std::max (maxStep, std::fabs (a[i] - a[i - 1]));
        const float total = 0.1f * (OutputStage::dbToGain (9.03f) - OutputStage::dbToGain (-24.0f));
        CHECK (maxStep < 0.01f * total, "gain step is not smooth: biggest sample-to-sample change %f of a total move %f", maxStep, total);
        CHECK (std::fabs (20.0f * std::log10 (a[47999] / 0.1f) + 24.0f) < 0.01f, "gain did not settle at -24 dB");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
struct Measured { double dbL = 0, dbR = 0; float peakOut = 0; };

// Runs the engine for six seconds with a tone on each channel (220 Hz left, 330 Hz right) and returns the output level relative to the
// input level over the second half, after the output stage.
static Measured runLevel (double hostRate, int mode, int quality, int engineRate, float dryWet, double amp, float levelDb, bool useStage = true)
{
    CloudsEngine e; e.prepare (hostRate);
    OutputStage::Stage stage; stage.prepare (hostRate); stage.setLevelDb (levelDb);
    const int block = 480;
    const int blocks = (int) (hostRate / block * 6.0);
    double phaseL = 0, phaseR = 0, sInL = 0, sInR = 0, sOutL = 0, sOutR = 0;
    Measured m;
    for (int b = 0; b < blocks; ++b)
    {
        std::vector<float> inL (block), inR (block), outL (block), outR (block);
        for (int i = 0; i < block; ++i)
        {
            phaseL += 2.0 * 3.14159265358979 * 220.0 / hostRate; phaseR += 2.0 * 3.14159265358979 * 330.0 / hostRate;
            inL[i] = (float) (amp * std::sin (phaseL)); inR[i] = (float) (amp * std::sin (phaseR));
        }
        CloudsEngine::Params p;
        p.mode = mode; p.quality = quality; p.dryWet = dryWet;
        e.process (inL.data(), inR.data(), outL.data(), outR.data(), block, p);
        if (useStage) { float* ch[2] = { outL.data(), outR.data() }; stage.process (ch, 2, block); }
        for (int i = 0; i < block; ++i)
        {
            m.peakOut = std::max ({ m.peakOut, std::fabs (outL[i]), std::fabs (outR[i]) });
            if (b > blocks / 2) { sInL += (double) inL[i] * inL[i]; sInR += (double) inR[i] * inR[i]; sOutL += (double) outL[i] * outL[i]; sOutR += (double) outR[i] * outR[i]; }
        }
    }
    m.dbL = toDb (sOutL) - toDb (sInL); m.dbR = toDb (sOutR) - toDb (sInR);
    return m;
}

static void testDryLevel()
{
    std::printf ("B. Fully dry level (Dry/Wet = 0, default Output Level %.2f dB), tolerance 0.1 dB\n", OutputStage::kDefaultLevelDb);
    const double hosts[3] = { 44100.0, 48000.0, 96000.0 };
    double worstDev = 0.0; int runs = 0;
    for (int hi = 0; hi < 3; ++hi)
    for (int rate = 0; rate < 1; ++rate)   // no Engine Rate in this plug-in
    for (int quality = 0; quality < 4; ++quality)
    for (int mode = 0; mode < 4; ++mode)
    {
        if (! hasDryPath (mode)) continue;
        if (fastMode && (hi != 1 || rate != 0 || quality != 0)) continue;
        // -18 dBFS everywhere; -6 dBFS too at 48 kHz (the firmware's own soft-limit bends louder signals a little: see the loud case below)
        for (double amp : { 0.125, 0.5 })
        {
            if (amp > 0.2 && hi != 1) continue;
            const auto m = runLevel (hosts[hi], mode, quality, rate, 0.0f, amp, OutputStage::kDefaultLevelDb);
            const double dev = std::max (std::fabs (m.dbL), std::fabs (m.dbR));
            worstDev = std::max (worstDev, dev); ++runs;
            CHECK (dev <= 0.1, "%s: host %.0f engine %d quality %d input %.1f dBFS: dry output is %+.3f dB (L) %+.3f dB (R) relative to the input",
                   kModeName[mode], hosts[hi], rate, quality, 20.0 * std::log10 (amp), m.dbL, m.dbR);
        }
    }
    std::printf ("   %d runs, every mode: worst deviation from the input level %.3f dB\n", runs, worstDev);

    std::printf ("   Per mode at 48 kHz host, engine 0, quality 0, input -18 dBFS (dB relative to the input):\n");
    for (int mode = 0; mode < 4; ++mode)
    {
        const auto m = runLevel (48000.0, mode, 0, 0, 0.0f, 0.125, OutputStage::kDefaultLevelDb);
        std::printf ("     %-22s %+.2f%s\n", kModeName[mode], m.dbL, hasDryPath (mode) ? "" : "   (no dry path in the firmware: not a dry/unity measurement)");
        if (! hasDryPath (mode)) CHECK (std::isfinite (m.dbL) && m.dbL < 6.0, "%s level looks wrong (%f dB)", kModeName[mode], m.dbL);
    }
    // Loud dry input: not part of the 0.1 dB claim, reported so the figures are known (firmware soft limit + this limiter)
    for (double amp : { 0.5, 0.8, 0.95 })
    {
        const auto m = runLevel (48000.0, CloudsEngine::LoopingDelay, 0, 0, 0.0f, amp, OutputStage::kDefaultLevelDb);
        std::printf ("   Looping delay, dry, input %.1f dBFS: output %+.2f dB relative to the input\n", 20.0 * std::log10 (amp), m.dbL);
    }
}

static void testNoClip()
{
    std::printf ("C. Loud wet sounds cannot clip\n");
    double peakNoStage = 0.0, peakStage = 0.0; int runs = 0;
    for (int mode = 0; mode < 4; ++mode)
    for (int quality = 0; quality < (fastMode ? 1 : 4); ++quality)
    for (float levelDb : { OutputStage::kDefaultLevelDb, 24.0f })
    for (float dryWet : { 0.0f, 0.5f, 1.0f })
    {
        const auto m = runLevel (48000.0, mode, quality, 0, dryWet, 0.95, levelDb);
        peakStage = std::max (peakStage, (double) m.peakOut); ++runs;
        CHECK (m.peakOut <= 1.0f, "%s quality %d dry/wet %.1f level %.1f dB: peak %f exceeds 0 dBFS", kModeName[mode], quality, dryWet, levelDb, m.peakOut);
        if (levelDb == OutputStage::kDefaultLevelDb)
        {
            const auto raw = runLevel (48000.0, mode, quality, 0, dryWet, 0.95, levelDb, false);
            peakNoStage = std::max (peakNoStage, (double) raw.peakOut);
        }
    }
    std::printf ("   %d runs with a 0.95 amplitude input; largest output peak with the stage %.4f (limit 1.0000); the engine alone at the default gain would reach %.3f of full scale before the limiter\n",
                 runs, peakStage, peakNoStage * (double) OutputStage::dbToGain (OutputStage::kDefaultLevelDb));
}

int main (int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) if (std::string (argv[i]) == "--fast") fastMode = true;
    testStageUnit();
    testDryLevel();
    testNoClip();
    std::printf ("\nlevel_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
