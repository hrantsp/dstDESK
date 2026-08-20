// A synthetic dstORCH, exposed as `kobayashi --simulate`.
//
// Speaks the real protocol over a real socket and sends generated tones, so the whole
// desktop half can be exercised end to end before the browser extension exists — and
// afterwards, to tell a server-side fault from an extension-side one.
//
// The two streams carry different pitches, which makes the result verifiable by ear:
// if mic.wav and meeting.wav each hold one clean tone, capture and transport are
// correct. If they are crossed or mixed, routing is not.

#ifndef DST_DESK_SIM_SIMULATE_HPP
#define DST_DESK_SIM_SIMULATE_HPP

#include <QString>
#include <cstdint>

namespace DST { namespace DESK { namespace Sim {

struct SimulateConfig
{
  std::uint16_t port    = 0;
  QString       token;
  double        seconds = 5.0;

  // Drops a run of frames from the meeting stream midway, so gap padding can be
  // observed rather than assumed. PROTOCOL.md §5.4.
  bool          injectGap = false;

  // Real audio instead of tones. Each must be 16 kHz mono 16-bit — the format the
  // wire carries — so no conversion happens here either. A stream with no file falls
  // back to its tone, which keeps the two distinguishable while testing one of them.
  QString micFile;
  QString tabFile;

  // Seconds to delay the meeting stream, so the two overlap the way a conversation
  // does rather than starting together.
  double tabOffset = 0.0;

  // Opens the meeting stream and then sends nothing on it at all — a tab capture that
  // yields no audio, or one whose track has already ended. The stream is open, so it
  // counts towards the merge watermark, and it never speaks, so it can never advance
  // it. Exists because that combination froze the whole transcript for the length of a
  // call while the microphone was being transcribed perfectly.
  bool quietMeeting = false;
};

int runSimulation(const SimulateConfig& cfg);

} } } // namespace DST::DESK::Sim

#endif // DST_DESK_SIM_SIMULATE_HPP
