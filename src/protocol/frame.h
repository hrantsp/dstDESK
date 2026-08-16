// Parsing and validation of dst binary audio frames.
//
// Deliberately free of Qt: this is pure data handling, unit-testable without an event
// loop or a display. See PROTOCOL.md §5.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "protocol_generated.h"

namespace dst {

enum class ParseError {
    None,
    TooShort,        // fewer bytes than a header
    BadVersion,      // version byte is not the one we speak
    LengthMismatch,  // message length != headerBytes + frameSamples * 2
    UnknownStream,   // stream id outside the set defined by the protocol
};

const char* describe(ParseError error);

struct FrameHeader {
    std::uint8_t version = 0;
    protocol::Stream stream = protocol::Stream::Mic;
    std::uint16_t frameSamples = 0;
    std::uint32_t sampleIndex = 0;
    std::uint32_t flags = 0;
};

struct ParsedFrame {
    ParseError error = ParseError::None;
    FrameHeader header{};
    // Payload bytes inside the caller's buffer. Not aligned for int16 access in
    // general, so read it with samplesInto() rather than casting.
    std::span<const std::byte> payload{};

    explicit operator bool() const { return error == ParseError::None; }
};

// Parses and fully validates one WebSocket binary message.
// The returned payload points into `message`; it must outlive the result.
ParsedFrame parseFrame(std::span<const std::byte> message);

// Copies the payload out as host-order int16 samples. Handles the byte-order
// conversion, so callers never depend on the host being little-endian.
void samplesInto(const ParsedFrame& frame, std::vector<std::int16_t>& out);

}  // namespace dst
