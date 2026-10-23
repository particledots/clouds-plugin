#include "CloudsEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include "clouds/dsp/granular_processor.h"
#include "clouds/dsp/mu_law.h"

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
        updateWaveform();

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

// Waveform display: once per 1 ms block, only while an editor has the display open. Publishes where the head, Position and Size
// are, and re-reads a few segments of the recording memory for their peaks (the whole buffer is swept every 500 ms or so, a
// 1 s buffer every 16 ms). It only reads the sample memory the processor has just finished with, on the audio thread itself,
// so it cannot disturb the sound and nothing in it can wait on the editor.
void CloudsEngine::updateWaveform()
{
    if (viewers.load (std::memory_order_relaxed) <= 0) return;

    clouds::PositionView v;
    proc->GetPositionView (&v);

    WaveformPicture::Meta m;
    m.hidden = v.hidden; m.kind = v.kind; m.ringFrames = v.ring_frames; m.headFrames = v.head; m.channels = v.channels;
    m.rateHz = proc->stored_sample_rate(); m.lowFi = v.low_fi; m.writing = v.writing;
    m.hasMarker = v.has_marker; m.hasBand = v.has_band; m.hasSlice = v.has_slice;
    m.markerBack = v.marker_back; m.bandFar = v.band_far; m.bandLen = v.band_len; m.sliceFar = v.slice_far; m.sliceLen = v.slice_len;

    const void* d[2] = { nullptr, nullptr };
    size_t bytes[2] = { 0, 0 };
    bool ok = v.hidden == clouds::VIEW_SHOWN && v.ring_frames > 0;
    for (int ch = 0; ch < 2 && ok; ++ch)
    {
        void* data = nullptr; size_t n = 0;
        if (proc->sample_region (ch, &data, &n)) { d[ch] = data; bytes[ch] = n; }
        else if (ch == 0) ok = false;
    }
    if (! ok) { scanRing = -1; picture.publish (m); return; }

    const int64_t ring = v.ring_frames;
    const size_t bytesPerFrame = v.low_fi ? 1u : 2u;
    for (int ch = 0; ch < 2; ++ch)
        if (d[ch] != nullptr && (size_t) ring * bytesPerFrame > bytes[ch]) { scanRing = -1; m.hidden = 1; picture.publish (m); return; }   // (never expected)

    // A new buffer (rebuilt, different length or format, moved): start the sweep again from silence.
    const int32_t gen = proc->buffers_generation();
    if (gen != scanGeneration || (int) ring != scanRing || v.low_fi != scanLowFi || d[0] != scanPtr)
    {
        scanGeneration = gen; scanRing = (int) ring; scanLowFi = v.low_fi; scanPtr = d[0]; scanCursor = 0; scanSinceHeld = 0;
        picture.clearPeaks();
    }

    constexpr int kBins = WaveformPicture::kBins;
    // A recording that is held still (Freeze, a playing layer) is swept once more after it stopped, then only re-checked now and then
    // (a layer being loaded changes the memory without the head moving).
    if (v.writing) scanSinceHeld = 0;
    else if (scanSinceHeld >= kBins && (++scanIdleTick & 31) != 0) { picture.publish (m); return; }
    const int perBlock = (int) std::clamp<int64_t> (2048LL * kBins / ring, 1, kBins);   // about 2048 frames re-read per block
    for (int k = 0; k < perBlock; ++k)
    {
        const int bin = scanCursor;
        scanCursor = (scanCursor + 1) % kBins;
        const int64_t f0 = ring * bin / kBins, f1 = ring * (bin + 1) / kBins;
        int peak = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            if (d[ch] == nullptr) continue;
            if (v.low_fi)
            {
                const uint8_t* s = static_cast<const uint8_t*> (d[ch]);
                for (int64_t f = f0; f < f1; ++f) { const int a = std::abs ((int) clouds::MuLaw2Lin (s[f])); if (a > peak) peak = a; }
            }
            else
            {
                const int16_t* s = static_cast<const int16_t*> (d[ch]);
                for (int64_t f = f0; f < f1; ++f) { const int a = std::abs ((int) s[f]); if (a > peak) peak = a; }
            }
        }
        picture.setPeak (bin, (uint16_t) std::min (peak, 32767));
    }
    if (! v.writing) scanSinceHeld += perBlock;
    picture.publish (m);
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
