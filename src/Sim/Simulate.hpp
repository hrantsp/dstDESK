// A synthetic dstORCH, exposed as `dstdesk --simulate`.
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
};

int runSimulation(const SimulateConfig& cfg);

} } } // namespace DST::DESK::Sim

#endif // DST_DESK_SIM_SIMULATE_HPP
