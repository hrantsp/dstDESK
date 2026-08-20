#include <algorithm>
#include <array>
#include <bit>
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
  failed_         = false;

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

  if constexpr (std::endian::native == std::endian::little)
  {
    // One write, not one per sample. The payload is already in the byte order RIFF
    // wants, so this is the same bytes either way — but a frame is 512 samples arriving
    // 31 times a second on each of two streams, and the previous version made about
    // 32,000 ostream::write calls a second to move 64 kB.
    file_.write(reinterpret_cast<const char*>(samples.data()),
                static_cast<std::streamsize>(samples.size() * 2));
  }
  else
  {
    // Big-endian hosts byte-swap into a fixed buffer and write in blocks, so this stays
    // allocation-free and still ends up as a handful of writes rather than thousands.
    auto block = std::array<std::byte, 1024 * 2>{};

    for (std::size_t at = 0; at < samples.size(); at += 1024)
    {
      const std::size_t nn = std::min<std::size_t>(1024, samples.size() - at);
      for (std::size_t ii = 0; ii < nn; ++ii)
        writeU16(block.data() + ii * 2, static_cast<std::uint16_t>(samples[at + ii]));

      file_.write(reinterpret_cast<const char*>(block.data()),
                  static_cast<std::streamsize>(nn * 2));
      if (!file_) break;
    }
  }

  // Counted only if it landed. samplesWritten_ is what patchSizes() declares in the
  // header, so counting a failed write would produce a file claiming more audio than it
  // holds — which is a corrupt recording rather than a short one.
  if (!file_) { failed_ = true; return; }
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
    if (!file_) { failed_ = true; return; }
    remaining -= nn;
  }
  samplesWritten_ += samples;
}

void WavWriter::patchSizes()
{
  // HP:TODO: RIFF sizes are u32, so this truncates past 4 GB — about 37 hours of 16 kHz
  // mono, and the point at which the header stops describing the file. Not guarded
  // because no meeting reaches it and the fix is a different container (RF64 or WAV64),
  // not a check. Listed under "Known limits" in dstOMNI/README.md.
  const std::uint64_t dataBytes = samplesWritten_ * 2;

  file_.seekp(kDataSizeOffset, std::ios::beg);
  writeU32(file_, static_cast<std::uint32_t>(dataBytes));

  file_.seekp(kRiffSizeOffset, std::ios::beg);
  writeU32(file_, static_cast<std::uint32_t>(kWavHeaderBytes - 8 + dataBytes));
}

bool WavWriter::flush()
{
  if (!file_.is_open()) return true;

  // Remember where the audio ends, patch the header, then carry on from where we
  // left off — seeking to the header and forgetting to return would overwrite the
  // recording with itself.
  const auto resumeAt = file_.tellp();
  patchSizes();
  file_.seekp(resumeAt, std::ios::beg);
  file_.flush();

  if (!file_) failed_ = true;
  return static_cast<bool>(file_);
}

bool WavWriter::close()
{
  if (!file_.is_open()) return true;

  patchSizes();

  const bool ok = static_cast<bool>(file_);
  if (!ok) failed_ = true;
  file_.close();
  return ok;
}

} } } // namespace DST::DESK::Core
