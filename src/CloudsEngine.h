// JUCE-free wrapper around the Mutable Instruments Clouds DSP (MIT licensed, Emilie Gillet).
// Clouds runs at 32 kHz (16 kHz in the low-fidelity qualities) in 32-frame blocks with 16-bit samples;
// this wrapper converts to and from the host rate.
#pragma once
#include <memory>
#include <vector>
#include <cstdint>
#include "Resampler.h"

namespace clouds { class GranularProcessor; }

class CloudsEngine
{
public:
    enum Mode { Granular = 0, PitchShifterStretch = 1, LoopingDelay = 2, Spectral = 3 };

    struct Params
    {
        // Same meaning as the module's knobs (all 0..1 unless noted).
        float position = 0.5f, size = 0.5f, density = 0.7f, texture = 0.5f;
        float pitchSemitones = 0.0f;                         // the PITCH knob, -48..48
        float pitchOffsetSemitones = 0.0f;                   // added after Pitch Snap, like the V/OCT input (used for MIDI follow)
        int pitchSnap = 0;                                   // our addition: 0 Off, 1 Steps (octaves and fifths), 2 Semitones
        // The module's four BLEND roles, each as its own value:
        float dryWet = 0.5f, stereoSpread = 0.5f, feedback = 0.0f, reverb = 0.0f;
        bool freeze = false;
        int mode = Granular;
        int quality = 0;     // 0 stereo 16-bit, 1 mono 16-bit, 2 stereo 8-bit low-fi, 3 mono 8-bit low-fi (the module's order)
    };

    CloudsEngine();
    ~CloudsEngine();

    void prepare (double hostSampleRate);

    // Fire the module's TRIG input once (seeds a grain / restarts the loop). Safe to call before process().
    void trigger() { triggerPending = true; }

    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples, const Params& p);

    int latencyInHostSamples() const;
    double processorRate() const { return procRate; }

    // Our addition: Pitch Snap applied to the PITCH knob's semitone value (before the MIDI note is added).
    //   0 Off       - unchanged
    //   1 Steps     - octaves and fifths over the knob's +-48 range: 0, 7, 12, 19, 24, 31, 36, 43, 48 (and the negatives)
    //   2 Semitones - whole semitones
    enum PitchSnap { SnapOff = 0, SnapSteps = 1, SnapSemitones = 2 };
    static float snapPitch (float semitones, int mode);

private:
    struct Frame { float l, r; };

    void runBlocks (const Params& p);

    std::unique_ptr<clouds::GranularProcessor> proc;
    std::vector<uint64_t> largeBuf, smallBuf;   // the module's 118784-byte main block and 65408-byte CCM block

    double hostRate = 48000.0, procRate = 32000.0;
    static constexpr size_t kBlock = 32;        // the module's block size

    // smoothed copies of the knob values (the firmware low-passes each knob; same coefficients)
    float sPosition = 0.5f, sSize = 0.5f, sDensity = 0.7f, sTexture = 0.5f, sPitch = 0.0f;
    float sDryWet = 0.5f, sSpread = 0.5f, sFeedback = 0.0f, sReverb = 0.0f;
    bool lastFreeze = false, triggerPending = false, smoothInit = false;

    SincResampler rsInL, rsInR, rsOutL, rsOutR;
    std::vector<float> tmpL, tmpR;
    std::vector<float> hostQL, hostQR;
    size_t hostQHead = 0;
    std::vector<Frame> inQueue;
    size_t inQueueHead = 0;
    std::vector<float> procOutL, procOutR;
};
