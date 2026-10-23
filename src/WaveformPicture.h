// A coarse picture of the recording buffer, for the editor's waveform display.
//
// The audio thread is the only writer: a little at a time, each millisecond, it re-reads a few segments of the recording memory,
// stores each segment's peak, and publishes where the write head, Position and Size are. The editor is the only reader. Every
// value is a relaxed std::atomic, so neither side ever waits for the other (no locks, no allocation, no waiting of any kind). A
// picture read while it is being refreshed can mix two refreshes; for a display that is harmless.
//
// The peaks are indexed by place in the buffer's ring (not by time), so the picture stays still while the head moves on and only
// the segments the head passes change. The reader turns ring places into "time back from now" using the published head.
#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

class WaveformPicture
{
public:
    static constexpr int kBins = 1024;

    // What the audio side publishes besides the peaks (see supercell/dsp/position_view.h for the meaning of each).
    struct Meta
    {
        int hidden = 1;            // 0 shown, 1 rebuilding, 2 no recording buffer in this mode, 3 Position is not a time here
        int kind = 0;              // 0 none, 1 granular, 2 stretch, 3 delay, 4 loop, 5 beat repeat
        int ringFrames = 0;
        int headFrames = 0;
        int channels = 0;
        float rateHz = 32000.0f;   // the rate the buffer is stored at
        bool lowFi = false;
        bool writing = false;      // false: the recording is held still (Freeze, a playing layer)
        bool hasMarker = false, hasBand = false, hasSlice = false;
        float markerBack = 0.0f, bandFar = 0.0f, bandLen = 0.0f, sliceFar = 0.0f, sliceLen = 0.0f;
    };

    struct Snapshot : Meta
    {
        uint16_t peak[kBins];      // 0..32767, by place in the ring
        Snapshot() { std::memset (peak, 0, sizeof (peak)); }

        float seconds (float frames) const { return rateHz > 0.0f ? frames / rateHz : 0.0f; }

        // How far back (in frames, 0 = now, ringFrames = oldest) the left edge of display column x of `width` is.
        float backAtColumnEdge (int x, int width) const { return (float) ringFrames * (1.0f - (float) x / (float) std::max (1, width)); }

        // The column (as a float, 0 = left edge = oldest) that `back` frames behind the head falls on.
        float columnOfBack (float back, int width) const
        {
            return ringFrames > 0 ? (1.0f - back / (float) ringFrames) * (float) width : 0.0f;
        }

        // The loudest peak (0..1) under each of `width` display columns, left = oldest audio, right = what the head just wrote.
        void columns (int width, float* out) const
        {
            if (ringFrames <= 0 || width <= 0) { for (int x = 0; x < width; ++x) out[x] = 0.0f; return; }
            const int64_t ring = ringFrames;
            for (int x = 0; x < width; ++x)
            {
                // ring places under this column: from head + ring*x/width to head + ring*(x+1)/width (mod ring)
                const int64_t a = (headFrames + ring * x / width) % ring;
                int64_t b = (headFrames + ring * (x + 1) / width - 1) % ring;
                if (b < 0) b += ring;
                const int b0 = (int) (a * kBins / ring), b1 = (int) (b * kBins / ring);
                int n = b1 - b0; if (n < 0) n += kBins;
                if (n > kBins - 1) n = kBins - 1;
                uint16_t m = 0;
                for (int i = 0, bin = b0; i <= n; ++i, bin = (bin + 1) % kBins) m = std::max (m, peak[bin]);
                out[x] = (float) m / 32768.0f;
            }
        }
    };

    WaveformPicture() { for (auto& p : peak) p.store (0, std::memory_order_relaxed); }

    // ---- audio thread -------------------------------------------------------------------------------------------------
    void setPeak (int bin, uint16_t value) { peak[(size_t) bin].store (value, std::memory_order_relaxed); }
    void clearPeaks() { for (auto& p : peak) p.store (0, std::memory_order_relaxed); }
    void publish (const Meta& m)
    {
        const auto r = std::memory_order_relaxed;
        hidden.store (m.hidden, r); kind.store (m.kind, r); ringFrames.store (m.ringFrames, r); headFrames.store (m.headFrames, r);
        channels.store (m.channels, r); rateHz.store (m.rateHz, r); lowFi.store (m.lowFi, r); writing.store (m.writing, r);
        hasMarker.store (m.hasMarker, r); hasBand.store (m.hasBand, r); hasSlice.store (m.hasSlice, r);
        markerBack.store (m.markerBack, r); bandFar.store (m.bandFar, r); bandLen.store (m.bandLen, r);
        sliceFar.store (m.sliceFar, r); sliceLen.store (m.sliceLen, r);
    }

    // ---- editor (any thread) ------------------------------------------------------------------------------------------
    void read (Snapshot& s) const
    {
        const auto r = std::memory_order_relaxed;
        s.hidden = hidden.load (r); s.kind = kind.load (r); s.ringFrames = ringFrames.load (r); s.headFrames = headFrames.load (r);
        s.channels = channels.load (r); s.rateHz = rateHz.load (r); s.lowFi = lowFi.load (r); s.writing = writing.load (r);
        s.hasMarker = hasMarker.load (r); s.hasBand = hasBand.load (r); s.hasSlice = hasSlice.load (r);
        s.markerBack = markerBack.load (r); s.bandFar = bandFar.load (r); s.bandLen = bandLen.load (r);
        s.sliceFar = sliceFar.load (r); s.sliceLen = sliceLen.load (r);
        for (int i = 0; i < kBins; ++i) s.peak[i] = peak[(size_t) i].load (r);
        if (s.ringFrames > 0) s.headFrames = std::clamp (s.headFrames, 0, s.ringFrames - 1);   // (a torn read must not index outside the ring)
    }

private:
    std::atomic<uint16_t> peak[kBins];
    std::atomic<int> hidden { 1 }, kind { 0 }, ringFrames { 0 }, headFrames { 0 }, channels { 0 };
    std::atomic<float> rateHz { 32000.0f };
    std::atomic<bool> lowFi { false }, writing { false }, hasMarker { false }, hasBand { false }, hasSlice { false };
    std::atomic<float> markerBack { 0.0f }, bandFar { 0.0f }, bandLen { 0.0f }, sliceFar { 0.0f }, sliceLen { 0.0f };

    static_assert (std::atomic<uint16_t>::is_always_lock_free && std::atomic<int>::is_always_lock_free
                   && std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
                   "the waveform picture must never need a lock");
};
