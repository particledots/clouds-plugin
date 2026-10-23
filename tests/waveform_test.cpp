// Tests of the waveform display (headless, offline).
//
// What this checks:
//   A. the picture's column mapping (where a place in the buffer lands on the display), including wrap-around
//   B. the picture the engine publishes: a burst put a known time back lands on the right column (16-bit and low-fi, stereo and
//      mono, 32 and 48 kHz, short and long buffers)
//   C. Freeze: the picture holds still when the recording is held
//   D. hidden modes (Spectral madness, Spectral clouds, Resonestor, Oliverb)
//   E. the sound is bit-for-bit identical with the display open or closed, across all modes, qualities, rates
//   F. the marker is where the sound comes from: a gliding tone is recorded (its pitch says how old each place on the tape is),
//      and the pitch of what the plug-in plays is compared with the pitch at the place the marker and band show
//   G. a reader thread hammering the picture while the audio runs (also meant to be run under ThreadSanitizer)
// What it does NOT check: how the editor draws it, or anything inside Logic.
#include "CloudsEngine.h"
#include "stmlib/utils/random.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using Engine = CloudsEngine;

// This test is shared by the plug-in repos: the older engines (Extended) have no Engine Rate setting and always run at 32 kHz.
template <class T> constexpr auto hasEngineRate (int) -> decltype ((void) std::declval<T>().engineRate, bool()) { return true; }
template <class T> constexpr bool hasEngineRate (long) { return false; }
template <class T> constexpr auto hasBufferLength (int) -> decltype ((void) std::declval<T>().bufferLength, bool()) { return true; }
template <class T> constexpr bool hasBufferLength (long) { return false; }
static constexpr bool kHasLength = hasBufferLength<Engine::Params> (0);
template <class P> static void setBufferLength (P& p, int length) { if constexpr (hasBufferLength<P> (0)) p.bufferLength = length; (void) p; (void) length; }
static constexpr bool kHasRate = hasEngineRate<Engine::Params> (0);
static constexpr int kRates = kHasRate ? 2 : 1;
static double rateHzOf (int engineRate) { return kHasRate && engineRate != 0 ? 48000.0 : 32000.0; }
static const char* rateName (int engineRate) { return kHasRate && engineRate != 0 ? "48k" : "32k"; }
template <class E> static auto blockOf (const E& e, int) -> decltype ((long) e.blockSize()) { return (long) e.blockSize(); }
template <class E> static long blockOf (const E&, long) { return 32; }      // (the older engines always run 32-frame blocks)
template <class P> static void setEngineRate (P& p, int engineRate) { if constexpr (hasEngineRate<P> (0)) p.engineRate = engineRate; (void) p; (void) engineRate; }
using Pic = WaveformPicture;

static int gChecks = 0, gFailed = 0;
static void check (bool ok, const char* what, double a = 0.0, double b = 0.0)
{
    ++gChecks;
    if (! ok) { ++gFailed; std::printf ("  FAIL: %s  (%g, %g)\n", what, a, b); }
}

static const double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------------------------------------------
// A. the column mapping
// ---------------------------------------------------------------------------------------------------------------------------
static void testMapping()
{
    std::printf ("[A] column mapping\n");
    const int rings[] = { 1000, 32696, 65528, 960000 };
    const int widths[] = { 100, 512, 700, 1500 };
    uint32_t rng = 12345;
    auto next = [&] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    int bad = 0, trials = 0;
    for (int ring : rings)
        for (int width : widths)
            for (int t = 0; t < 200; ++t)
            {
                Pic::Snapshot s;
                s.ringFrames = ring; s.headFrames = (int) (next() % (uint32_t) ring);
                const int place = (int) (next() % (uint32_t) ring);              // a place in the ring that holds a burst
                s.peak[(int64_t) place * Pic::kBins / ring] = 20000;
                std::vector<float> col ((size_t) width);
                s.columns (width, col.data());
                int back = (s.headFrames - place) % ring; if (back <= 0) back += ring;   // frames behind the head (the slot at the head itself is the oldest)
                const float expect = s.columnOfBack ((float) back, width);
                int arg = (int) (std::max_element (col.begin(), col.end()) - col.begin());
                // the burst's bin is ring/1024 frames wide, which can be a few columns on a wide display of a short buffer
                const float binCols = (float) width / (float) Pic::kBins;
                const float tol = std::max (1.5f, binCols + 1.5f);
                float nearest = 1e9f;
                for (int x = 0; x < width; ++x) if (col[(size_t) x] > 0.5f) nearest = std::min (nearest, std::fabs ((float) x + 0.5f - expect));
                ++trials;
                if (! (col[(size_t) arg] > 0.5f && nearest <= tol)) { if (bad++ < 5) std::printf ("  ring %d width %d head %d place %d: expected column %.1f, got %d\n", ring, width, s.headFrames, place, expect, arg); }
            }
    check (bad == 0, "a burst anywhere in the ring lands on the column that matches its time back, for any head position", bad, trials);

    // the oldest audio is on the left, the newest on the right
    Pic::Snapshot s; s.ringFrames = 1000; s.headFrames = 400;
    s.peak[(int64_t) 399 * Pic::kBins / 1000] = 30000;   // just written
    s.peak[(int64_t) 401 * Pic::kBins / 1000] = 15000;   // about to be overwritten: the oldest
    std::vector<float> col (200);
    s.columns (200, col.data());
    check (col[199] > 0.8f && col[0] > 0.4f && col[0] < 0.6f, "newest audio at the right edge, oldest at the left edge", col[199], col[0]);
    check (std::fabs (s.columnOfBack (0.0f, 200) - 200.0f) < 1e-3f && std::fabs (s.columnOfBack (1000.0f, 200)) < 1e-3f, "back 0 = right edge, back = ring = left edge");
}

// ---------------------------------------------------------------------------------------------------------------------------
// A rig: an engine fed at its own rate (so one frame in = one frame stored), with the display open
// ---------------------------------------------------------------------------------------------------------------------------
struct Rig
{
    Engine e;
    Engine::Params p;
    double sr;
    long fed = 0;                          // frames fed since the start
    std::vector<float> in, oL, oR;
    static constexpr int N = 256;

    // `setup` can set the knobs before the first block, so the firmware's knob smoothing starts from their final values.
    Rig (int engineRate, int quality, int bufferLength, int mode, bool viewer = true, void (*setup) (Engine::Params&) = nullptr)
        : sr (rateHzOf (engineRate)), in (N), oL (N), oR (N)
    {
        if (viewer) e.waveformViewerOpened();
        e.prepare (sr);
        setEngineRate (p, engineRate); p.quality = quality; setBufferLength (p, bufferLength); p.mode = mode;
        p.dryWet = 1.0f; p.size = 0.5f; p.position = 0.5f; p.density = 0.5f; p.texture = 0.5f; p.reverb = 0.0f; p.feedback = 0.0f;
        if (setup) setup (p);
        run (0.3, [] (long) { return 0.0f; });      // the first rebuild
    }

    // Feeds `seconds` of gen(absolute frame index); returns the left output.
    template <class G> std::vector<float> run (double seconds, G gen, std::vector<float>* outR = nullptr)
    {
        std::vector<float> result;
        const long total = (long) (seconds * sr);
        long done = 0;
        result.reserve ((size_t) total);
        while (done < total)
        {
            const int n = (int) std::min<long> (N, total - done);
            for (int i = 0; i < n; ++i) in[(size_t) i] = gen (fed + i);
            e.process (in.data(), in.data(), oL.data(), oR.data(), n, p);
            result.insert (result.end(), oL.begin(), oL.begin() + n);
            if (outR) outR->insert (outR->end(), oR.begin(), oR.begin() + n);
            fed += n; done += n;
        }
        return result;
    }
    void silence (double seconds) { run (seconds, [] (long) { return 0.0f; }); }

    Pic::Snapshot snap() const { Pic::Snapshot s; e.waveform().read (s); return s; }

    // Frames written into the buffer so far (counted at the rate the buffer is stored at), as far as the test can tell: whole
    // blocks, then corrected with the published head (the first block or two after a mode change can be skipped by the processor).
    long written() const
    {
        const long B = blockOf (e, 0);
        const auto s = snap();
        const long est = (long) std::llround ((double) (fed / B * B) * (double) s.rateHz / sr);
        if (s.ringFrames <= 0) return est;
        long d = (est - s.headFrames) % s.ringFrames;
        if (d < 0) d += s.ringFrames;
        if (d > s.ringFrames / 2) d -= s.ringFrames;
        return est - d;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------
// B. a burst a known time back lands on the right column
// ---------------------------------------------------------------------------------------------------------------------------
static void testBurstPlacement (int engineRate, int quality, int bufferLength)
{
    if (engineRate != 0 && ! kHasRate) return;      // (no Engine Rate setting in this plug-in)
    if (bufferLength != 0 && ! kHasLength) return;  // (no Buffer Length setting in this plug-in)
    const int width = 800;
    Rig r (engineRate, quality, bufferLength, Engine::Granular);
    auto s0 = r.snap();
    const double ringSec = s0.ringFrames / (double) s0.rateHz;
    // back-times as fractions of the buffer, away from the very ends
    const double fractions[] = { 0.15, 0.4, 0.62, 0.85 };
    for (double frac : fractions)
    {
        Rig q (engineRate, quality, bufferLength, Engine::Granular);
        const double beforeSec = ringSec * 0.97;                    // the buffer is about full before the burst
        const long burstLen = (long) (q.sr * ringSec * 0.004);      // 0.4 % of the buffer
        const long afterFrames = (long) (frac * s0.ringFrames * (q.sr / s0.rateHz));
        q.run (beforeSec * 0.5, [] (long) { return 0.0f; });
        const long burstAt = q.fed;
        q.run (burstLen / q.sr, [&] (long i) { return 0.7f * (float) std::sin (0.9 * (double) (i - burstAt)); });
        q.run (afterFrames / q.sr, [] (long) { return 0.0f; });
        // let the sweep refresh everything (the longest sweep is a little over 128 ms)
        q.p.freeze = true;                                           // hold the recording still while the sweep finishes
        q.silence (0.4);
        const auto s = q.snap();
        // where the burst's centre should be: frames behind the head, in stored frames
        const long wr = q.written();
        const double stored = s.rateHz;
        const double backCentre = (double) wr - ((double) burstAt + (double) burstLen / 2.0) * (stored / q.sr);
        std::vector<float> col ((size_t) width);
        s.columns (width, col.data());
        int arg = (int) (std::max_element (col.begin(), col.end()) - col.begin());
        const double expectCol = s.columnOfBack ((float) backCentre, width);
        char what[160];
        std::snprintf (what, sizeof what, "burst %.0f %% of the buffer back (engine %s, quality %d, buffer %d): column", frac * 100.0, rateName (engineRate), quality, bufferLength);
        const double tol = std::max (3.0, (double) width * 0.012);
        check (std::fabs ((double) arg + 0.5 - expectCol) <= tol, what, arg, expectCol);
        // and it is a burst, not smeared over the display: everything far from it is quiet
        int loud = 0; for (int x = 0; x < width; ++x) if (col[(size_t) x] > 0.2f) ++loud;
        check (loud <= (int) (width * 0.03) + 6, "the picture is mostly quiet away from the burst", loud, width);
        check (col[(size_t) arg] > 0.4f, "the burst shows at about its level", col[(size_t) arg]);
        check (s.ringFrames > 0 && s.headFrames >= 0 && s.headFrames < s.ringFrames, "head is inside the ring", s.headFrames, s.ringFrames);
    }
}

static void testPlacementAll()
{
    std::printf ("[B] the picture: a burst lands where it is in time\n");
    testBurstPlacement (0, 0, 0);   // 32 kHz, stereo 16-bit, 1 s
    testBurstPlacement (1, 0, 0);   // 48 kHz
    testBurstPlacement (0, 1, 0);   // mono
    testBurstPlacement (0, 2, 0);   // stereo low-fi (8-bit mu-law, 16 kHz)
    testBurstPlacement (1, 3, 0);   // mono low-fi at the 48 kHz engine
    testBurstPlacement (1, 0, 3);   // 20 s buffer at 48 kHz (the longest)
    testBurstPlacement (0, 2, 2);   // low-fi, 10 s
}

// ---------------------------------------------------------------------------------------------------------------------------
// C. freeze
// ---------------------------------------------------------------------------------------------------------------------------
static bool samePeaks (const Pic::Snapshot& a, const Pic::Snapshot& b) { return std::memcmp (a.peak, b.peak, sizeof a.peak) == 0 && a.headFrames == b.headFrames; }

static void testFreeze()
{
    std::printf ("[C] Freeze\n");
    auto tone = [] (double hz, float amp, double sr) { return [=] (long i) { return amp * (float) std::sin (2.0 * kPi * hz * (double) i / sr); }; };
    {
        Rig r (0, 0, 0, Engine::Granular);
        r.run (1.5, tone (440, 0.3f, r.sr));
        check (r.snap().writing, "recording and not frozen: the picture is live");
        auto a = r.snap();
        r.run (0.3, tone (440, 0.3f, r.sr));
        check (a.headFrames != r.snap().headFrames, "the head moves while recording");
        r.p.freeze = true;
        r.run (0.4, tone (440, 0.3f, r.sr));          // a full sweep passes
        const auto f1 = r.snap();
        check (! f1.writing, "Freeze on: the display says the recording is held");
        r.run (0.5, tone (880, 0.9f, r.sr));          // loud different input while frozen
        const auto f2 = r.snap();
        check (samePeaks (f1, f2), "Freeze on: the picture does not change, however loud the input");
        r.p.freeze = false;
        r.run (0.6, tone (880, 0.9f, r.sr));
        const auto f3 = r.snap();
        check (f3.writing && ! samePeaks (f1, f3), "Freeze off: the picture moves again");
        float mx1 = 0, mx3 = 0; for (auto v : f1.peak) mx1 = std::max<float> (mx1, v); for (auto v : f3.peak) mx3 = std::max<float> (mx3, v);
        check (mx3 > mx1 * 1.5f, "and shows the new, louder input", mx1, mx3);
    }
    for (int mode : { (int) Engine::PitchShifterStretch, (int) Engine::LoopingDelay })
    {
        Rig r (1, 0, 0, mode);
        r.run (1.2, tone (440, 0.3f, r.sr));
        r.p.freeze = true; r.run (0.4, tone (440, 0.3f, r.sr));
        const auto a = r.snap(); r.run (0.4, tone (990, 0.9f, r.sr));
        check (! a.writing && samePeaks (a, r.snap()), "frozen picture holds still in the other recording modes too");
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// D. hidden modes
// ---------------------------------------------------------------------------------------------------------------------------
static void testHidden()
{
    std::printf ("[D] modes without a Position on the tape\n");
    struct { int mode; int hidden; const char* name; } cases[] = {
        { Engine::Spectral, 2, "Spectral madness" } };
    for (int rate = 0; rate < kRates; ++rate)
        for (auto& c : cases)
        {
            Rig r (rate, 0, 0, c.mode);
            r.silence (0.5);
            char what[120]; std::snprintf (what, sizeof what, "%s is hidden with reason %d", c.name, c.hidden);
            check (r.snap().hidden == c.hidden, what, r.snap().hidden, c.hidden);
        }
    for (int mode : { (int) Engine::Granular, (int) Engine::PitchShifterStretch, (int) Engine::LoopingDelay })
    {
        Rig r (0, 0, 0, mode); r.silence (0.5);
        check (r.snap().hidden == 0 && r.snap().ringFrames > 0, "the four recording modes are shown", mode, r.snap().hidden);
    }
    {   // going into a hidden mode and back brings the picture back
        Rig r (0, 0, 0, Engine::Granular);
        r.run (1.0, [&] (long i) { return 0.4f * (float) std::sin (2 * kPi * 300.0 * (double) i / r.sr); });
        r.p.mode = Engine::Spectral; r.silence (0.5);
        check (r.snap().hidden == 2, "Spectral madness hides it");
        r.p.mode = Engine::Granular; r.silence (0.6);
        check (r.snap().hidden == 0, "back in Granular it is shown again", r.snap().hidden);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// E. the sound is the same with the display open or closed
// ---------------------------------------------------------------------------------------------------------------------------
static std::vector<float> scenario (bool viewer, int engineRate, int quality, int bufferLength, std::vector<uint8_t>* /*unused*/)
{
    stmlib::Random::Seed (4242);
    Engine e;
    if (viewer) e.waveformViewerOpened();
    const double sr = rateHzOf (engineRate);
    e.prepare (sr);
    Engine::Params p; setEngineRate (p, engineRate); p.quality = quality; setBufferLength (p, bufferLength);
    p.dryWet = 0.8f; p.reverb = 0.2f; p.feedback = 0.2f;
    std::vector<float> in (256), oL (256), oR (256), all;
    uint32_t rng = 99;
    double ph = 0;
    long t = 0;
    auto block = [&] (double seconds)
    {
        const long total = (long) (seconds * sr);
        for (long done = 0; done < total; done += 256)
        {
            for (int i = 0; i < 256; ++i)
            {
                rng = rng * 1664525u + 1013904223u;
                ph += 220.0 / sr;
                in[(size_t) i] = 0.4f * (float) std::sin (2 * kPi * ph) + 0.2f * ((float) (rng >> 9) / 4194304.0f - 1.0f);
            }
            if (t % 7 == 0 && done == 0) e.trigger();
            e.process (in.data(), in.data(), oL.data(), oR.data(), 256, p);
            all.insert (all.end(), oL.begin(), oL.end()); all.insert (all.end(), oR.begin(), oR.end());
        }
        ++t;
    };
    for (int mode = 0; mode < 4; ++mode)
    {
        p.mode = mode;
        block (0.5);
        p.position = 0.1f + 0.1f * (float) mode; p.size = 0.9f - 0.1f * (float) mode; p.density = 0.3f + 0.08f * (float) mode;
        block (0.4);
        p.freeze = true; block (0.3);
        p.position = 0.9f; p.pitchSemitones = 7.0f; p.pitchSnap = 1; block (0.3);
        p.freeze = false; block (0.3);
        p.pitchSnap = 0; p.pitchSemitones = 0.0f;
    }
    return all;
}

static void testBitExact()
{
    std::printf ("[E] the sound is bit-for-bit the same with the display open or closed\n");
    struct { int rate, quality, length; } cases[] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 2, 1 }, { 1, 3, 0 }, { 1, 0, 2 } };
    for (auto& c : cases)
    {
        if ((c.rate != 0 && ! kHasRate) || (c.length != 0 && ! kHasLength)) continue;
        std::vector<uint8_t> hA, hB;
        const auto a = scenario (false, c.rate, c.quality, c.length, &hA);
        const auto b = scenario (true, c.rate, c.quality, c.length, &hB);
        size_t diff = 0, first = 0;
        if (a.size() != b.size()) diff = 1;
        else for (size_t i = 0; i < a.size(); ++i) if (std::memcmp (&a[i], &b[i], sizeof (float)) != 0) { if (diff++ == 0) first = i; }
        char what[160];
        std::snprintf (what, sizeof what, "engine %s, quality %d, buffer %d: %zu samples, all modes: output identical", c.rate ? "48k" : "32k", c.quality, c.length, a.size());
        check (diff == 0, what, (double) diff, (double) first);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// F. the marker is where the sound comes from
// ---------------------------------------------------------------------------------------------------------------------------
// The test tone glides upward for ever: at frame n it has frequency f0 + K * n / ring (K Hz per buffer length). So the pitch of a
// place on the tape says when it was recorded, and the pitch of what is played says which place it came from.
static const double kF0 = 300.0;

static void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * kPi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0);
            for (size_t j = 0; j < len / 2; ++j)
            {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl;
            }
        }
    }
}

// Power-weighted mean frequency of a signal, and the 10 % / 90 % points of its power.
struct Spectrum { double centroid = 0, median = 0, p10 = 0, p90 = 0, power = 0; };
static Spectrum analyse (const std::vector<float>& x, double sr)
{
    size_t n = 1; while (n < x.size()) n <<= 1;
    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < x.size(); ++i) a[i] = (double) x[i] * (0.5 - 0.5 * std::cos (2 * kPi * (double) i / (double) x.size()));
    fft (a);
    Spectrum s;
    double sum = 0, wsum = 0;
    std::vector<double> pw (n / 2);
    for (size_t k = 1; k < n / 2; ++k)
    {
        const double f = (double) k * sr / (double) n;
        if (f < 80.0 || f > sr * 0.45) { pw[k] = 0; continue; }
        pw[k] = std::norm (a[k]);
        sum += pw[k]; wsum += pw[k] * f;
    }
    s.power = sum; s.centroid = sum > 0 ? wsum / sum : 0;
    double acc = 0;
    for (size_t k = 1; k < n / 2; ++k)
    {
        acc += pw[k];
        if (s.median == 0 && acc >= 0.5 * sum) s.median = (double) k * sr / (double) n;
        if (s.p10 == 0 && acc >= 0.1 * sum) s.p10 = (double) k * sr / (double) n;
        if (s.p90 == 0 && acc >= 0.9 * sum) s.p90 = (double) k * sr / (double) n;
    }
    return s;
}

struct Gliss
{
    double ring;      // frames per buffer length at the stored rate
    double sr;
    double K;         // Hz the glide climbs per buffer length
    float operator() (long i) const { const double n = (double) i; return 0.5f * (float) std::sin (2 * kPi / sr * (kF0 * n + K * n * n / (2.0 * ring))); }
    double freqAt (double n) const { return kF0 + K * n / ring; }
};

static double pitchPotFor (float semitones) { return (double) semitones; }      // (this engine takes the pitch in semitones directly)

struct Result { double errFrames = 0, ringFrames = 0, ms = 0, spreadMs = 0; bool ok = false; Pic::Snapshot snap; };

static long wrapSigned (long d, long ring) { d %= ring; if (d < 0) d += ring; return d > ring / 2 ? d - ring : d; }

// Which tape place does the sound come from?  Returns the error in time (+ = the sound comes from later than the marker says).
static Result chirpCase (int mode, int engineRate, float position, float size, bool frozen, float pitchSemis = 0.0f)
{
    Result res;
    // the knobs are set before the first block, so that none of the firmware's slow knob smoothing is still moving when we look
    static float sPos, sSize, sPot; static int sMode;
    sPos = position; sSize = size; sPot = (float) pitchPotFor (pitchSemis); sMode = mode;
    Rig r (engineRate, 0, 0, mode, true, [] (Engine::Params& p)
    {
        p.position = sPos; p.size = sSize; p.pitchSemitones = sPot; p.dryWet = 1.0f; p.texture = 0.5f; p.stereoSpread = 0.0f;
        p.density = sMode == Engine::Granular ? 0.9f : 0.2f;
    });
    const auto s0 = r.snap();
    const double K = frozen ? 3000.0 : 1200.0;          // (the running delay needs a gentler glide: it stays below half the sample rate for longer)
    Gliss g { (double) s0.ringFrames, r.sr, K };
    const double L = (double) r.e.latencyInHostSamples();
    r.run (1.6 * (double) s0.ringFrames / r.sr, g);       // the buffer is full of the glide
    long nFreeze = 0;
    if (frozen)
    {
        nFreeze = r.written();
        r.p.freeze = true; r.run (0.2, g);
        const auto h = r.snap();
        nFreeze += wrapSigned ((long) h.headFrames - nFreeze, h.ringFrames);   // where the head stopped, as an absolute frame count
    }
    r.run (frozen ? 1.0 : 3.5, g);   // (the running delay glides slowly, on the original Clouds a time constant of about 0.6 s)
    std::vector<float> out = r.run (frozen ? 3.0 : 0.4, g);
    const auto snap = r.snap();
    res.snap = snap; res.ringFrames = (double) s0.ringFrames;
    const auto sp = analyse (out, r.sr);
    if (sp.power <= 0) return res;
    const double centreBack = (snap.kind == 3) ? snap.markerBack : snap.markerBack + snap.bandLen * 0.5;
    const double ratio = std::pow (2.0, (double) pitchSemis / 12.0);
    // the frame, in the glide's own numbering, that the tape place the marker points at was recorded at
    double predictedFrame;
    if (frozen) predictedFrame = (double) nFreeze - centreBack;
    else predictedFrame = (double) r.written() - 0.2 * r.sr - L - centreBack;        // the middle of the 0.4 s of output; the output is L frames late
    const double measuredHz = sp.median / ratio;
    const double predictedHz = g.freqAt (predictedFrame);
    res.errFrames = (measuredHz - predictedHz) * g.ring / K;
    res.ms = res.errFrames / s0.rateHz * 1000.0;
    res.spreadMs = (sp.p90 - sp.p10) / ratio * g.ring / K / s0.rateHz * 1000.0;
    if (std::getenv ("WF_DEBUG")) std::printf ("    dbg: centroid %.0f p10 %.0f p90 %.0f ratio %.3f predictedHz %.0f pred frame %.0f ring %.0f centreBack %.0f nFreeze %ld head %d\n", sp.centroid, sp.p10, sp.p90, ratio, predictedHz, predictedFrame, g.ring, centreBack, nFreeze, snap.headFrames);
    res.ok = true;
    return res;
}

// `inBand`: grains have a lopsided envelope (the Texture knob tilts it), so the heard "centre" can sit anywhere inside the shaded band;
// then the test is only that the sound comes from inside the band (with a little slack).
static void report (const char* name, int engineRate, float pos, float size, const Result& x, double tolFrac, double tolMs, const char* extra = "", bool inBand = false)
{
    char what[220];
    std::snprintf (what, sizeof what, "%s %s position %.2f size %.2f%s: sound is %+.1f ms from where the marker says (marker %.3f s back, band %.3f s, heard spread %.0f ms)",
                   name, rateName (engineRate), pos, size, extra, x.ms, x.snap.markerBack / x.snap.rateHz, x.snap.bandLen / x.snap.rateHz, x.spreadMs);
    double tol = std::max (tolMs, tolFrac * x.ringFrames / 32000.0 * 1000.0);
    if (inBand) tol = x.snap.bandLen / x.snap.rateHz * 500.0 + tolMs;
    const bool ok = x.ok && std::fabs (x.ms) <= tol;
    ++gChecks;
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (! ok) ++gFailed;
}

static void testMarkerMeaning()
{
    std::printf ("[F] the marker is where the sound comes from (a gliding tone as a time stamp; centre of the shaded band)\n");
    const float positions[] = { 0.0f, 0.3f, 0.65f, 1.0f };
    const float sizes[] = { 0.2f, 0.6f };
    for (int rate = 0; rate < kRates; ++rate)
        for (float pos : positions)
            for (float size : sizes)
                report ("Granular", rate, pos, size, chirpCase (Engine::Granular, rate, pos, size, true), 0.03, 12.0, "", true);
    report ("Granular", 0, 0.5f, 0.5f, chirpCase (Engine::Granular, 0, 0.5f, 0.5f, true, 12.0f), 0.03, 12.0, " pitch +12", true);
    report ("Granular", 0, 0.5f, 0.5f, chirpCase (Engine::Granular, 0, 0.5f, 0.5f, true, -12.0f), 0.03, 12.0, " pitch -12", true);
    for (int rate = 0; rate < kRates; ++rate)
        for (float pos : positions)
            for (float size : sizes)
                report ("Pitch shifter", rate, pos, size, chirpCase (Engine::PitchShifterStretch, rate, pos, size, true), 0.06, 40.0);
    for (int rate = 0; rate < kRates; ++rate)
        for (float pos : { 0.0f, 0.3f, 0.65f, 1.0f })
            report ("Looping delay (running)", rate, pos, 0.5f, chirpCase (Engine::LoopingDelay, rate, pos, 0.5f, false), 0.02, 10.0);
    for (int rate = 0; rate < kRates; ++rate)
        for (float pos : { 0.0f, 0.3f, 0.65f })
            for (float size : { 0.2f, 0.5f })
                report ("Looping delay (frozen loop)", rate, pos, size, chirpCase (Engine::LoopingDelay, rate, pos, size, true), 0.03, 12.0);
}


// ---------------------------------------------------------------------------------------------------------------------------
// G. a reader thread and the audio at the same time
// ---------------------------------------------------------------------------------------------------------------------------
static void testThreads()
{
    std::printf ("[G] a reader hammering the picture while the audio runs\n");
    Rig r (1, 0, 3, Engine::Granular);
    std::atomic<bool> stop { false };
    std::atomic<long> reads { 0 };
    std::atomic<double> worstUs { 0.0 };
    std::atomic<bool> sane { true };
    std::thread reader ([&]
    {
        Pic::Snapshot s;
        std::vector<float> col (800);
        while (! stop.load())
        {
            const auto t0 = std::chrono::steady_clock::now();
            r.e.waveform().read (s);
            s.columns (800, col.data());
            const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
            double w = worstUs.load(); while (us > w && ! worstUs.compare_exchange_weak (w, us)) {}
            if (s.ringFrames > 0 && (s.headFrames < 0 || s.headFrames >= s.ringFrames)) sane.store (false);
            for (float v : col) if (! (v >= 0.0f && v <= 1.01f)) sane.store (false);
            ++reads;
        }
    });
    Gliss g { 20.0 * 48000.0, 48000.0 };
    double worstBlockMs = 0, total = 0; int blocks = 0;
    for (int k = 0; k < 400; ++k)
    {
        r.p.mode = (k / 50) % 4;           // modes change while it is being read
        r.p.position = (float) ((k * 37) % 100) / 100.0f; r.p.freeze = (k / 25) % 2 == 1;
        const auto t0 = std::chrono::steady_clock::now();
        r.run (0.05, g);
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        worstBlockMs = std::max (worstBlockMs, ms); total += ms; ++blocks;
    }
    stop.store (true); reader.join();
    std::printf ("  %ld reads, slowest %.0f us; audio: 50 ms of sound took %.2f ms on average, %.2f ms at worst\n", reads.load(), worstUs.load(), total / blocks, worstBlockMs);
    check (reads.load() > 100, "the reader ran freely while the audio ran", (double) reads.load());
    check (sane.load(), "every picture the reader got was sane (head inside the ring, columns between 0 and 1)");
}

// CPU cost of keeping the picture, 20 s buffer at 48 kHz (information, not a pass / fail)
static void costReport()
{
    double ms[2] = {};
    for (int viewer = 0; viewer < 2; ++viewer)
    {
        Rig r (1, 0, 3, Engine::Granular, viewer == 1);
        Gliss g { 20.0 * 48000.0, 48000.0 };
        r.run (3.0, g);
        const auto t0 = std::chrono::steady_clock::now();
        r.run (10.0, g);
        ms[viewer] = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    }
    std::printf ("[cost] 10 s of Granular at 48 kHz with a 20 s buffer: %.0f ms without the display, %.0f ms with it open (%.1f %% more)\n",
                 ms[0], ms[1], 100.0 * (ms[1] - ms[0]) / ms[0]);
}

int main (int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp (argv[1], "quick") == 0;
    testMapping();
    testPlacementAll();
    testFreeze();
    testHidden();
    testBitExact();
    if (! quick) { testMarkerMeaning(); }
    testThreads();
    if (! quick) costReport();
    std::printf ("\n%d checks, %d failed\n", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}
