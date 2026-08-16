#include <catch2/catch_test_macros.hpp>

#include "Core/Frame.hpp"

#include <cstring>
#include <vector>
#include "Core/Endian.hpp"

using namespace DST::DESK::Core;

namespace {

// Builds a wire frame the way dstORCH would, so the tests exercise the real layout
// rather than a mirror of the parser's assumptions.
std::vector<std::byte> makeFrame(std::uint8_t version, std::uint8_t stream,
                                 std::uint16_t frameSamples, std::uint32_t sampleIndex,
                                 const std::vector<std::int16_t>& samples)
{
  auto buf = std::vector<std::byte>(kHeaderBytes + samples.size() * 2);

  buf[kOffsetVersion] = static_cast<std::byte>(version);
  buf[kOffsetStream ] = static_cast<std::byte>(stream);
  writeU16(buf.data() + kOffsetFrameSamples, frameSamples);
  writeU32(buf.data() + kOffsetSampleIndex,  sampleIndex);
  writeU32(buf.data() + kOffsetFlags,        0);

  for (std::size_t ii = 0; ii < samples.size(); ++ii)
    writeU16(buf.data() + kHeaderBytes + ii * 2, static_cast<std::uint16_t>(samples[ii]));

  return buf;
}

const std::vector<std::int16_t> kSamples = { 0, 1, -1, 32767, -32768, 1234, -4321, 7 };

} // namespace

TEST_CASE("a well-formed frame parses", "[frame]")
{
  const auto buf = makeFrame(kVersion, 1,
                             static_cast<std::uint16_t>(kSamples.size()), 48000,
                             kSamples);
  const auto parsed = parseFrame(buf);

  REQUIRE(parsed);
  CHECK(parsed.header.version      == kVersion);
  CHECK(parsed.header.stream       == Stream::Tab);
  CHECK(parsed.header.frameSamples == kSamples.size());
  CHECK(parsed.header.sampleIndex  == 48000);
  CHECK(parsed.payload.size()      == kSamples.size() * 2);
}

TEST_CASE("payload round-trips through samplesInto", "[frame]")
{
  const auto buf = makeFrame(kVersion, 0,
                             static_cast<std::uint16_t>(kSamples.size()), 0, kSamples);
  const auto parsed = parseFrame(buf);
  REQUIRE(parsed);

  auto out = std::vector<std::int16_t>{};
  samplesInto(parsed, out);
  CHECK(out == kSamples);
}

TEST_CASE("a truncated message is rejected", "[frame]")
{
  auto buf = makeFrame(kVersion, 0, 4, 0, { 1, 2, 3, 4 });
  buf.resize(kHeaderBytes - 1);
  CHECK(parseFrame(buf).error == ParseError::TooShort);
}

TEST_CASE("a foreign protocol version is rejected before anything else is read",
          "[frame]")
{
  const auto buf = makeFrame(kVersion + 1, 99, 9999, 0, kSamples);
  // Stream 99 and a nonsense sample count would both be errors too; version must
  // win, because nothing after it can be trusted.
  CHECK(parseFrame(buf).error == ParseError::BadVersion);
}

TEST_CASE("an unknown stream id is rejected", "[frame]")
{
  const auto buf = makeFrame(kVersion, 7,
                             static_cast<std::uint16_t>(kSamples.size()), 0, kSamples);
  CHECK(parseFrame(buf).error == ParseError::UnknownStream);
}

TEST_CASE("a declared sample count that disagrees with the length is rejected",
          "[frame]")
{
  // Claims more samples than the payload carries — the shape a truncated or
  // corrupted frame takes.
  auto buf = makeFrame(kVersion, 0, 64, 0, kSamples);
  CHECK(parseFrame(buf).error == ParseError::LengthMismatch);

  // ...and fewer.
  buf = makeFrame(kVersion, 0, 2, 0, kSamples);
  CHECK(parseFrame(buf).error == ParseError::LengthMismatch);
}

TEST_CASE("a full-size production frame is exactly kFrameBytes", "[frame]")
{
  const auto full = std::vector<std::int16_t>(kFrameSamples, 0);
  const auto buf  = makeFrame(kVersion, 0, kFrameSamples, 0, full);
  CHECK(buf.size() == kFrameBytes);
  CHECK(parseFrame(buf));
}
