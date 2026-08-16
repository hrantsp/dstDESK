// Parsing and validation of dst binary audio frames.
//
// Deliberately free of Qt: this is pure data handling, unit-testable without an event
// loop or a display. See rec/PROTOCOL.md §5.

#ifndef DST_DESK_CORE_FRAME_HPP
#define DST_DESK_CORE_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "Protocol.hpp"

namespace DST { namespace DESK { namespace Core {

struct FrameHeader
{
  std::uint8_t  version      = 0;
  Stream::Value stream       = Stream::Mic;
  std::uint16_t frameSamples = 0;
  std::uint32_t sampleIndex  = 0;
  std::uint32_t flags        = 0;
};

struct ParseError { enum Value : std::uint8_t
{
  None           = 0,
  TooShort       = 1, // fewer bytes than a header
  BadVersion     = 2, // version byte is not the one we speak
  LengthMismatch = 3, // message length != headerBytes + frameSamples * 2
  UnknownStream  = 4, // stream id outside the set defined by the protocol
};

  static constexpr auto what(ParseError::Value val) noexcept
  {
    switch (val) { case None           : return "ok";
                   case TooShort       : return "message shorter than the frame header";
                   case BadVersion     : return "unsupported protocol version";
                   case LengthMismatch : return "declared sample count does not match message length";
                   case UnknownStream  : return "unknown stream id";
                   default             : return "unknown error"; }
  }
};

struct ParsedFrame
{
  ParseError::Value error  = ParseError::None;
  FrameHeader       header = {};

  // Payload bytes inside the caller's buffer. Not aligned for int16 access in
  // general, so read it with samplesInto() rather than casting.
  std::span<const std::byte> payload = {};

  explicit operator bool() const { return error == ParseError::None; }
};

// Parses and fully validates one WebSocket binary message.
// The returned payload points into `message`; it must outlive the result.
ParsedFrame parseFrame(std::span<const std::byte> message);

// Copies the payload out as host-order int16 samples. Handles the byte-order
// conversion, so callers never depend on the host being little-endian.
void samplesInto(const ParsedFrame& frame, std::vector<std::int16_t>& out);

} } } // namespace DST::DESK::Core

#endif // DST_DESK_CORE_FRAME_HPP
