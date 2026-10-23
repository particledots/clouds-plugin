// Unofficial SuperParasites Layers: a read-only description of where the POSITION knob points on the recording buffer, for the
// plug-in's waveform display. Nothing in the DSP reads this; the players only fill it in from their own state (Describe()).
//
// All distances are in frames (samples at the rate the buffer is stored at: the engine rate, or 16 kHz in the low-fi qualities),
// counted BACK from the write head, so 0 = "now" and ring_frames = the oldest audio the buffer still holds.
#ifndef CLOUDS_DSP_POSITION_VIEW_H_
#define CLOUDS_DSP_POSITION_VIEW_H_

#include "stmlib/stmlib.h"

namespace clouds {

enum PositionViewKind {
  VIEW_NONE = 0,
  VIEW_GRANULAR,      // grains start at the marker and read on toward now (the band is one grain's stretch of tape)
  VIEW_STRETCH,       // pitch shifter / time stretch: the same, with the WSOLA window as the band
  VIEW_DELAY,         // looping delay, not frozen: the marker is the delay time (no band)
  VIEW_LOOP,          // looping delay, frozen: the band is the loop, the marker its end nearest to now
  VIEW_BEAT_REPEAT    // beat repeat: the band is the loop inside the slice (outlined), the marker is where the loop starts
};

enum PositionViewHidden {
  VIEW_SHOWN = 0,
  VIEW_REBUILDING,    // the memory is being rebuilt (mode / quality / buffer change): nothing to show yet
  VIEW_NO_BUFFER,     // Spectral madness, Spectral clouds, Resonestor: the recording buffer is not played
  VIEW_NOT_A_TIME     // Oliverb: Position is a reverb pre-delay, not a place on the tape
};

struct PositionView {
  int32_t hidden = VIEW_REBUILDING;
  int32_t kind = VIEW_NONE;
  int32_t ring_frames = 0;       // length of the recording buffer
  int32_t head = 0;              // write head (channel 0)
  int32_t channels = 0;
  bool low_fi = false;           // 8-bit mu-law samples
  bool writing = false;          // false: the recording is being held (Freeze, a playing layer), so the picture does not change
  bool has_marker = false;
  bool has_band = false;
  bool has_slice = false;
  float marker_back = 0.0f;
  float band_far = 0.0f;         // the band runs from band_far (older) toward now for band_len frames (wrapping round the buffer)
  float band_len = 0.0f;
  float slice_far = 0.0f;        // beat repeat: the slice the loop lives in, same convention
  float slice_len = 0.0f;
};

}  // namespace clouds

#endif  // CLOUDS_DSP_POSITION_VIEW_H_
