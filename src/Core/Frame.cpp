#include <bit>
#include <cstring>
#include "Endian.hpp"
#include "Frame.hpp"

namespace DST { namespace DESK { namespace Core {

ParsedFrame parseFrame(std::span<const std::byte> message)
{
  auto result = ParsedFrame{};

  if (message.size() < kHeaderBytes)
  {
    result.error = ParseError::TooShort;
    return result;
  }

  const std::byte* pp = message.data();

  result.header.version = std::to_integer<std::uint8_t>(pp[kOffsetVersion]);
  if (result.header.version != kVersion)
  {
    // Checked before anything else is trusted: a different version means the
    // remaining bytes carry no guarantee of meaning.
    result.error = ParseError::BadVersion;
    return result;
  }

  const auto streamId = std::to_integer<std::uint8_t>(pp[kOffsetStream]);
  if (!Stream::isKnown(streamId))
  {
    result.error = ParseError::UnknownStream;
    return result;
  }

  result.header.stream       = static_cast<Stream::Value>(streamId);
  result.header.frameSamples = readU16(pp + kOffsetFrameSamples);
  result.header.sampleIndex  = readU32(pp + kOffsetSampleIndex);
  result.header.flags        = readU32(pp + kOffsetFlags);

  const std::size_t expected = kHeaderBytes + static_cast<std::size_t>(result.header.frameSamples) * 2;
  if (message.size() != expected)
  {
    result.error = ParseError::LengthMismatch;
    return result;
  }

  result.payload = message.subspan(kHeaderBytes);
  return result;
}

void samplesInto(const ParsedFrame& frame, std::vector<std::int16_t>& out)
{
  const std::size_t count = frame.payload.size() / 2;
  out.resize(count);
  if (count == 0) return;

  if constexpr (std::endian::native == std::endian::little)
  {
    // The common case on every target platform: the payload is already in host
    // order, so this is one memcpy per frame.
    std::memcpy(out.data(), frame.payload.data(), count * 2);
  }
  else
  {
    const std::byte* pp = frame.payload.data();
    for (std::size_t ii = 0; ii < count; ++ii)
      out[ii] = static_cast<std::int16_t>(readU16(pp + ii * 2));
  }
}

} } } // namespace DST::DESK::Core
