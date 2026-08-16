#include "frame.h"

#include <bit>
#include <cstring>

namespace dst {
namespace {

// The wire is little-endian regardless of host. Reading byte by byte keeps this
// correct on a big-endian host without a compile-time branch at every call site.
std::uint16_t readU16(const std::byte* p) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(p[0]) |
                                      (std::to_integer<std::uint16_t>(p[1]) << 8));
}

std::uint32_t readU32(const std::byte* p) {
    return std::to_integer<std::uint32_t>(p[0]) |
           (std::to_integer<std::uint32_t>(p[1]) << 8) |
           (std::to_integer<std::uint32_t>(p[2]) << 16) |
           (std::to_integer<std::uint32_t>(p[3]) << 24);
}

bool knownStream(std::uint8_t value) {
    return value == static_cast<std::uint8_t>(protocol::Stream::Mic) ||
           value == static_cast<std::uint8_t>(protocol::Stream::Tab);
}

}  // namespace

const char* describe(ParseError error) {
    switch (error) {
        case ParseError::None:           return "ok";
        case ParseError::TooShort:       return "message shorter than the frame header";
        case ParseError::BadVersion:     return "unsupported protocol version";
        case ParseError::LengthMismatch: return "declared sample count does not match message length";
        case ParseError::UnknownStream:  return "unknown stream id";
    }
    return "unknown error";
}

ParsedFrame parseFrame(std::span<const std::byte> message) {
    ParsedFrame result;

    if (message.size() < protocol::kHeaderBytes) {
        result.error = ParseError::TooShort;
        return result;
    }

    const std::byte* p = message.data();

    result.header.version = std::to_integer<std::uint8_t>(p[protocol::kOffsetVersion]);
    if (result.header.version != protocol::kVersion) {
        // Checked before anything else is trusted: a different version means the
        // remaining bytes carry no guarantee of meaning.
        result.error = ParseError::BadVersion;
        return result;
    }

    const auto streamId = std::to_integer<std::uint8_t>(p[protocol::kOffsetStream]);
    if (!knownStream(streamId)) {
        result.error = ParseError::UnknownStream;
        return result;
    }
    result.header.stream = static_cast<protocol::Stream>(streamId);

    result.header.frameSamples = readU16(p + protocol::kOffsetFrameSamples);
    result.header.sampleIndex = readU32(p + protocol::kOffsetSampleIndex);
    result.header.flags = readU32(p + protocol::kOffsetFlags);

    const std::size_t expected =
        protocol::kHeaderBytes + static_cast<std::size_t>(result.header.frameSamples) * 2;
    if (message.size() != expected) {
        result.error = ParseError::LengthMismatch;
        return result;
    }

    result.payload = message.subspan(protocol::kHeaderBytes);
    return result;
}

void samplesInto(const ParsedFrame& frame, std::vector<std::int16_t>& out) {
    const std::size_t count = frame.payload.size() / 2;
    out.resize(count);
    if (count == 0) {
        return;
    }

    if constexpr (std::endian::native == std::endian::little) {
        // The common case on every target platform: the payload is already in host
        // order, so this is one memcpy per frame.
        std::memcpy(out.data(), frame.payload.data(), count * 2);
    } else {
        const std::byte* p = frame.payload.data();
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = static_cast<std::int16_t>(readU16(p + i * 2));
        }
    }
}

}  // namespace dst
