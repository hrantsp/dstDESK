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

TEST_CASE("a recording is playable before it is closed", "[recorder]")
{
  // The property that matters when a process is killed: a RIFF header still claiming
  // zero bytes makes the whole file unreadable however much audio sits behind it. The
  // sizes are therefore patched periodically, not only on close.
  const auto path = tempWav("midflight.wav");
  auto rec = StreamRecorder{};
  REQUIRE(rec.open(path, kSampleRate));

  const auto samples = std::vector<std::int16_t>(kFrameSamples, 7);
  for (std::uint32_t ii = 0; ii < 40; ++ii)
  {
    REQUIRE(rec.accept(header(ii * kFrameSamples, kFrameSamples), samples));
  }

  // Deliberately not closed.
  const auto bytes = readAll(path);
  REQUIRE(bytes.size() >= 44);

  const std::uint32_t riffSize = readU32(bytes, 4);
  const std::uint32_t dataSize = readU32(bytes, 40);

  CHECK(dataSize > 0);
  CHECK(riffSize == dataSize + 36);
  CHECK(dataSize <= 40u * kFrameSamples * 2);   // never claims more than was written
  CHECK(dataSize + 44 <= bytes.size());         // and never more than the file holds
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

TEST_CASE("a healthy recording does not report a write failure", "[recorder]")
{
  // The guard on the guard. A write-failure flag that fires spuriously is worse than
  // none: it puts "recording stopped" in front of a user whose recording is fine, and
  // teaches them to ignore the next one that is real. The failing direction needs a
  // filesystem that refuses writes and is exercised outside this suite; this is the
  // half that must hold on every platform.
  const auto path = tempWav("healthy.wav");

  auto recorder = StreamRecorder{};
  REQUIRE(recorder.open(path, kSampleRate));

  const auto samples = std::vector<std::int16_t>(kFrameSamples, 1234);
  for (std::uint32_t ii = 0; ii < 40; ++ii)
  {
    REQUIRE(recorder.accept(header(ii * kFrameSamples, kFrameSamples), samples));
    CHECK_FALSE(recorder.stats().writeFailed);
  }

  recorder.close();
  CHECK_FALSE(recorder.stats().writeFailed);
  std::filesystem::remove(path);
}

TEST_CASE("a gap too large to be real is not padded", "[recorder]")
{
  // sampleIndex is a u32 that arrives from the client, and it used to be handed
  // straight to the writer as a length. One frame claiming a position four billion
  // samples ahead made the recorder write eight gigabytes of silence, synchronously,
  // with the event loop blocked throughout — measured at 6.3 GB in two seconds.
  const auto path = tempWav("huge-gap.wav");

  auto recorder = StreamRecorder{};
  REQUIRE(recorder.open(path, kSampleRate));

  const auto samples = std::vector<std::int16_t>(kFrameSamples, 0);
  REQUIRE(recorder.accept(header(0, kFrameSamples), samples));

  // Well past the bound: four billion samples is about 69 hours of audio.
  REQUIRE(recorder.accept(header(4000000000u, kFrameSamples), samples));

  CHECK(recorder.stats().resyncs       == 1);
  CHECK(recorder.stats().gaps          == 0);
  CHECK(recorder.stats().paddedSamples == 0);

  // Two frames of audio and nothing else — the header, and no silence at all.
  recorder.close();
  CHECK(readAll(path).size() == 44 + std::size_t(kFrameSamples) * 2 * 2);
  std::filesystem::remove(path);
}

TEST_CASE("a gap a client could really produce is still padded", "[recorder]")
{
  // The other side of the bound. The sender caps its outbound buffer at roughly
  // sixteen seconds of audio, so a drop of a few seconds is an ordinary event and must
  // still be filled — otherwise everything after it sits earlier in the file than its
  // sampleIndex says, which is exactly the drift padding exists to prevent.
  const auto path = tempWav("real-gap.wav");

  auto recorder = StreamRecorder{};
  REQUIRE(recorder.open(path, kSampleRate));

  const auto samples = std::vector<std::int16_t>(kFrameSamples, 0);
  REQUIRE(recorder.accept(header(0, kFrameSamples), samples));

  // Five seconds later: a plausible backpressure drop.
  const std::uint32_t next = kSampleRate * 5;
  REQUIRE(recorder.accept(header(next, kFrameSamples), samples));

  CHECK(recorder.stats().resyncs       == 0);
  CHECK(recorder.stats().gaps          == 1);
  CHECK(recorder.stats().paddedSamples == next - kFrameSamples);

  recorder.close();
  std::filesystem::remove(path);
}
