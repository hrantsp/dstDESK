#include <catch2/catch_test_macros.hpp>

#include "Core/StreamRecorder.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

using namespace DST::DESK::Core;

namespace {

std::filesystem::path tempWav(const char* name)
{
  auto pp = std::filesystem::temp_directory_path() / "dst-tests" / name;
  std::filesystem::remove(pp);
  return pp;
}

std::vector<char> readAll(const std::filesystem::path& pp)
{
  auto in = std::ifstream(pp, std::ios::binary);
  return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::uint32_t readU32(const std::vector<char>& bb, std::size_t off)
{
  return  static_cast<std::uint8_t>(bb[off])            |
         (static_cast<std::uint8_t>(bb[off + 1]) <<  8) |
         (static_cast<std::uint8_t>(bb[off + 2]) << 16) |
         (static_cast<std::uint8_t>(bb[off + 3]) << 24);
}

FrameHeader header(std::uint32_t sampleIndex, std::uint16_t frameSamples)
{
  auto hh = FrameHeader{};
  hh.version      = kVersion;
  hh.stream       = Stream::Mic;
  hh.frameSamples = frameSamples;
  hh.sampleIndex  = sampleIndex;
  return hh;
}

} // namespace

TEST_CASE("a closed WAV has a valid RIFF header and correct sizes", "[recorder]")
{
  const auto path = tempWav("header.wav");
  auto rec = StreamRecorder{};
  REQUIRE(rec.open(path, kSampleRate));

  const auto samples = std::vector<std::int16_t>(kFrameSamples, 1000);
  REQUIRE(rec.accept(header(0, kFrameSamples), samples));
  rec.close();

  const auto bytes = readAll(path);
  REQUIRE(bytes.size() == 44 + kFrameSamples * 2);

  CHECK(std::string(bytes.data(),      4) == "RIFF");
  CHECK(std::string(bytes.data() +  8, 4) == "WAVE");
  CHECK(std::string(bytes.data() + 36, 4) == "data");
  CHECK(readU32(bytes,  4) == bytes.size() - 8);   // RIFF size
  CHECK(readU32(bytes, 24) == kSampleRate);        // sample rate
  CHECK(readU32(bytes, 40) == kFrameSamples * 2);  // data size
}

TEST_CASE("contiguous frames produce no padding", "[recorder]")
{
  const auto path = tempWav("contiguous.wav");
  auto rec = StreamRecorder{};
  REQUIRE(rec.open(path, kSampleRate));

  const auto samples = std::vector<std::int16_t>(kFrameSamples, 7);
  for (std::uint32_t ii = 0; ii < 10; ++ii)
  {
    REQUIRE(rec.accept(header(ii * kFrameSamples, kFrameSamples), samples));
  }
  rec.close();

  CHECK(rec.stats().frames        == 10);
  CHECK(rec.stats().gaps          == 0);
  CHECK(rec.stats().paddedSamples == 0);
  CHECK(rec.stats().samples       == 10u * kFrameSamples);
}

TEST_CASE("a gap is padded with exactly the missing number of samples", "[recorder]")
{
  const auto path = tempWav("gap.wav");
  auto rec = StreamRecorder{};
  REQUIRE(rec.open(path, kSampleRate));

  const auto samples = std::vector<std::int16_t>(kFrameSamples, 7);
  REQUIRE(rec.accept(header(0, kFrameSamples), samples));

  // Three frames go missing: next index jumps by 4 frames instead of 1.
  const std::uint32_t missing = 3 * kFrameSamples;
  REQUIRE(rec.accept(header(4 * kFrameSamples, kFrameSamples), samples));
  rec.close();

  CHECK(rec.stats().gaps          == 1);
  CHECK(rec.stats().paddedSamples == missing);

  // The file must be as long as if nothing had been lost: position is preserved,
  // which is the entire point of padding.
  const auto bytes = readAll(path);
  CHECK(bytes.size() == 44 + (5u * kFrameSamples) * 2);
}

TEST_CASE("out-of-order frames are rejected rather than replayed", "[recorder]")
{
  const auto path = tempWav("reorder.wav");
  auto rec = StreamRecorder{};
  REQUIRE(rec.open(path, kSampleRate));

  const auto samples = std::vector<std::int16_t>(kFrameSamples, 7);
  REQUIRE(rec.accept(header(kFrameSamples * 5, kFrameSamples), samples));

  CHECK_FALSE(rec.accept(header(kFrameSamples * 2, kFrameSamples), samples));
  CHECK_FALSE(rec.accept(header(kFrameSamples * 5, kFrameSamples), samples)); // duplicate

  CHECK(rec.stats().rejected == 2);
  CHECK(rec.stats().frames   == 1);
}

TEST_CASE("a stream starting late does not pad from zero", "[recorder]")
{
  // sampleIndex comes from a clock shared with the other stream, so a stream that
  // opens 30 s in starts at a large index. The file must begin at that point, not
  // contain 30 s of silence.
  const auto path = tempWav("late-start.wav");
  auto rec = StreamRecorder{};
  REQUIRE(rec.open(path, kSampleRate));

  const std::uint32_t lateStart = kSampleRate * 30;
  const auto samples = std::vector<std::int16_t>(kFrameSamples, 7);
  REQUIRE(rec.accept(header(lateStart, kFrameSamples), samples));
  rec.close();

  CHECK(rec.stats().paddedSamples    == 0);
  CHECK(rec.stats().firstSampleIndex == lateStart);
  CHECK(readAll(path).size()         == 44 + kFrameSamples * 2);
}
