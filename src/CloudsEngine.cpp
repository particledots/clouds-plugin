#include "CloudsEngine.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include "clouds/dsp/granular_processor.h"

namespace
{
    constexpr int kPrepareCallsPerBlock = 16;   // the firmware calls Prepare() in a tight main loop between blocks

    inline short toShort (float x)
    {
        return (short) std::lrintf (std::clamp (x, -1.0f, 1.0f) * 32767.0f);
    }
    inline void onePole (float& state, float target, float coefficient) { state += coefficient * (target - state); }

    // The firmware's processor lives in global memory, which the chip zeroes at start-up, and parts of Clouds
    // (for example the silence flag) are never initialised by the code itself. So hand it zeroed memory too.
    template <class T> T* newZeroed()
    {
        void* m = ::operator new (sizeof (T));
        std::memset (m, 0, sizeof (T));
        return new (m) T();
    }
}

CloudsEngine::CloudsEngine()
    : largeBuf (118784 / 8, 0), smallBuf ((65536 - 128) / 8, 0)
{
}

CloudsEngine::~CloudsEngine() = default;

void CloudsEngine::prepare (double sr)
{
    hostRate = sr;
    procRate = 32000.0;
    std::fill (largeBuf.begin(), largeBuf.end(), 0);
    std::fill (smallBuf.begin(), smallBuf.end(), 0);
    proc.reset (newZeroed<clouds::GranularProcessor>());   // fresh, zeroed state every time we (re)prepare
    proc->Init (largeBuf.data(), largeBuf.size() * 8, smallBuf.data(), smallBuf.size() * 8);
    *proc->mutable_parameters() = clouds::Parameters();
    proc->Prepare();   // allocate the working buffers (the module does this in its main loop)

    rsInL.init (hostRate, procRate);  rsInR.init (hostRate, procRate);
    rsOutL.init (procRate, hostRate); rsOutR.init (procRate, hostRate);
    inQueue.clear(); inQueueHead = 0;
    hostQL.clear(); hostQR.clear(); hostQHead = 0;
    const size_t prime = kBlock * 2 + SincResampler::kTaps;   // keeps the output resampler from starving
    procOutL.assign (prime, 0.0f); procOutR.assign (prime, 0.0f);
    smoothInit = false; lastFreeze = false; triggerPending = false;
}

int CloudsEngine::latencyInHostSamples() const
{
    const double procFrames = (double) kBlock * 2 + SincResampler::kTaps;
    if (std::abs (hostRate - procRate) < 1e-6) return (int) procFrames;
    return (int) std::ceil (procFrames * hostRate / procRate);
}

float CloudsEngine::snapPitch (float semitones, int mode)
{
    if (mode == SnapSemitones) return std::round (semitones);
    if (mode != SnapSteps) return semitones;
    static const float kSteps[] = { 0.f, 7.f, 12.f, 19.f, 24.f, 31.f, 36.f, 43.f, 48.f };   // mirrored below zero
    const float a = std::fabs (semitones);
    float best = kSteps[0];
    for (float d : kSteps)
        if (std::fabs (a - d) < std::fabs (a - best)) best = d;
    return semitones < 0.f ? -best : best;
}

void CloudsEngine::runBlocks (const Params& p)
{
    using namespace clouds;
    const size_t B = kBlock;

    // Mode and quality: set through the same calls the module's UI uses. The processor mutes itself while it rebuilds.
    proc->set_playback_mode ((PlaybackMode) std::clamp (p.mode, 0, 3));
    proc->set_quality (std::clamp (p.quality, 0, 3));

    std::vector<ShortFrame> in (B), out (B);
    while (inQueue.size() - inQueueHead >= B)
    {
        for (size_t i = 0; i < B; ++i)
        {
            const Frame& f = inQueue[inQueueHead + i];
            in[i].l = toShort (f.l);
            in[i].r = toShort (f.r);
        }
        inQueueHead += B;

        // Per-block knob smoothing with the firmware's own coefficients (cv_scaler.cc).
        if (! smoothInit)
        {
            sPosition = p.position; sSize = p.size; sDensity = p.density; sTexture = p.texture; sPitch = snapPitch (p.pitchSemitones, p.pitchSnap) + p.pitchOffsetSemitones;
            sDryWet = p.dryWet; sSpread = p.stereoSpread; sFeedback = p.feedback; sReverb = p.reverb;
            smoothInit = true;
        }
        onePole (sPosition, p.position, 0.05f);
        onePole (sDensity,  p.density,  0.01f);
        onePole (sSize,     p.size,     0.01f);
        onePole (sTexture,  p.texture,  0.01f);
        // Pitch Snap: the knob's note is snapped, then the MIDI offset is added. With a snap on, the pitch glides over a few ms
        // (so a step is not a click); Off keeps the module's slow knob smoothing exactly as it was.
        onePole (sPitch, snapPitch (p.pitchSemitones, p.pitchSnap) + p.pitchOffsetSemitones, p.pitchSnap == SnapOff ? 0.01f : 0.2f);
        onePole (sDryWet,   p.dryWet,   0.05f);
        onePole (sSpread,   p.stereoSpread, 0.05f);
        onePole (sFeedback, p.feedback, 0.05f);
        onePole (sReverb,   p.reverb,   0.05f);

        Parameters* prm = proc->mutable_parameters();
        prm->position = std::clamp (sPosition, 0.0f, 1.0f);
        prm->texture  = std::clamp (sTexture, 0.0f, 1.0f);
        prm->density  = std::clamp (sDensity, 0.0f, 1.0f);
        prm->size     = std::clamp (sSize, 0.0f, 1.0f);
        prm->dry_wet  = std::clamp (sDryWet * 1.05f - 0.025f, 0.0f, 1.0f);   // the module's end-stop trim
        prm->reverb   = std::clamp (sReverb, 0.0f, 1.0f);
        prm->feedback = std::clamp (sFeedback, 0.0f, 1.0f);
        prm->stereo_spread = std::clamp (sSpread, 0.0f, 1.0f);
        prm->pitch    = std::clamp (sPitch, -48.0f, 48.0f);

        // Freeze: only written when the plug-in's switch changes, like the module's gate edges. (The processor itself
        // clears freeze when it rebuilds its buffers after a mode/quality change, as on the module.)
        if (p.freeze != lastFreeze) { prm->freeze = p.freeze; lastFreeze = p.freeze; }

        prm->trigger = triggerPending;
        triggerPending = false;
        prm->gate = false;

        proc->Process (in.data(), out.data(), B);

        for (int k = 0; k < kPrepareCallsPerBlock; ++k)
            proc->Prepare();

        for (size_t i = 0; i < B; ++i)
        {
            procOutL.push_back ((float) out[i].l / 32768.0f);
            procOutR.push_back ((float) out[i].r / 32768.0f);
        }
    }
    if (inQueueHead > 4096)
    {
        inQueue.erase (inQueue.begin(), inQueue.begin() + (long) inQueueHead);
        inQueueHead = 0;
    }
}

void CloudsEngine::process (const float* inL, const float* inR, float* outL, float* outR, int n, const Params& p)
{
    // 1. host rate -> 32 kHz
    tmpL.clear(); tmpR.clear();
    rsInL.process (inL, (size_t) n, tmpL);
    rsInR.process (inR, (size_t) n, tmpR);
    const size_t produced = std::min (tmpL.size(), tmpR.size());
    for (size_t i = 0; i < produced; ++i)
        inQueue.push_back ({ tmpL[i], tmpR[i] });

    // 2. run the module DSP
    runBlocks (p);

    // 3. 32 kHz -> host rate
    rsOutL.process (procOutL.data(), procOutL.size(), hostQL);
    rsOutR.process (procOutR.data(), procOutR.size(), hostQR);
    procOutL.clear(); procOutR.clear();

    const size_t avail = std::min (hostQL.size(), hostQR.size()) - hostQHead;
    const size_t take = std::min (avail, (size_t) n);
    for (size_t i = 0; i < take; ++i) { outL[i] = hostQL[hostQHead + i]; outR[i] = hostQR[hostQHead + i]; }
    for (size_t i = take; i < (size_t) n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }   // underrun guard
    hostQHead += take;
    if (hostQHead > 8192)
    {
        hostQL.erase (hostQL.begin(), hostQL.begin() + (long) hostQHead);
        hostQR.erase (hostQR.begin(), hostQR.begin() + (long) hostQHead);
        hostQHead = 0;
    }
}
