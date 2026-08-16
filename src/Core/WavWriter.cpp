#include <algorithm>
#include <vector>
#include "Endian.hpp"
#include "WavWriter.hpp"

namespace DST { namespace DESK { namespace Core {
namespace {

constexpr std::uint16_t  kBitsPerSample  = 16;
constexpr std::streamoff kRiffSizeOffset =  4;
constexpr std::streamoff kDataSizeOffset = 40;
constexpr std::size_t    kWavHeaderBytes = 44;

// A RIFF four-character chunk tag: ASCII, never byte-swapped.
void writeTag(std::ostream& out, const char (&tag)[5]) { out.write(tag, 4); }

} // namespace

bool WavWriter::open(const std::filesystem::path& path, std::uint32_t sampleRate, std::uint16_t channels)
{
  close();

  auto ec = std::error_code{};
  std::filesystem::create_directories(path.parent_path(), ec);

  file_.open(path, std::ios::binary | std::ios::trunc);
  if (!file_) return false;

  path_           = path;
  sampleRate_     = sampleRate;
  channels_       = channels;
  samplesWritten_ = 0;

  const std::uint32_t byteRate   = sampleRate * channels * (kBitsPerSample / 8);
  const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (kBitsPerSample / 8));

  writeTag(file_, "RIFF");
  writeU32(file_, 0); // patched by close()
  writeTag(file_, "WAVE");
  writeTag(file_, "fmt ");
  writeU32(file_, 16); // PCM fmt chunk size
  writeU16(file_, 1);  // format 1 = PCM
  writeU16(file_, channels);
  writeU32(file_, sampleRate);
  writeU32(file_, byteRate);
  writeU16(file_, blockAlign);
  writeU16(file_, kBitsPerSample);
  writeTag(file_, "data");
  writeU32(file_, 0); // patched by close()

  return static_cast<bool>(file_);
}

void WavWriter::write(std::span<const std::int16_t> samples)
{
  if (!file_ || samples.empty()) return;
  for (const std::int16_t ss : samples) writeU16(file_, static_cast<std::uint16_t>(ss));
  samplesWritten_ += samples.size();
}

void WavWriter::writeSilence(std::size_t samples)
{
  if (!file_ || samples == 0) return;

  // Chunked so a long gap cannot allocate an unbounded buffer.
  constexpr std::size_t kChunk = 4096;
  const auto zeros = std::vector<char>(kChunk * 2, 0);

  std::size_t remaining = samples;
  while (remaining > 0)
  {
    const std::size_t nn = std::min(remaining, kChunk);
    file_.write(zeros.data(), static_cast<std::streamsize>(nn * 2));
    remaining -= nn;
  }
  samplesWritten_ += samples;
}

bool WavWriter::close()
{
  if (!file_.is_open()) return true;

  const std::uint64_t dataBytes = samplesWritten_ * 2;

  file_.seekp(kDataSizeOffset, std::ios::beg);
  writeU32(file_, static_cast<std::uint32_t>(dataBytes));

  file_.seekp(kRiffSizeOffset, std::ios::beg);
  writeU32(file_, static_cast<std::uint32_t>(kWavHeaderBytes - 8 + dataBytes));

  const bool ok = static_cast<bool>(file_);
  file_.close();
  return ok;
}

} } } // namespace DST::DESK::Core
