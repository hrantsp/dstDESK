// Minimal RIFF/WAVE writer for 16-bit PCM.
//
// Exists so the seam can be verified by ear: if the two files play back as clean,
// separate audio, the capture and transport path is correct end to end. Qt-free.

#ifndef DST_DESK_CORE_WAVWRITER_HPP
#define DST_DESK_CORE_WAVWRITER_HPP

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>

namespace DST { namespace DESK { namespace Core {

class WavWriter
{
public:
  WavWriter() = default;

  // A file that is never closed stays unplayable, because the RIFF sizes are only
  // patched by close(). Hence the destructor.
  ~WavWriter() { close(); }

  WavWriter             (const WavWriter&) = delete;
  WavWriter& operator = (const WavWriter&) = delete;

  // Writes a placeholder header; sizes are patched by close().
  bool open(const std::filesystem::path& path, std::uint32_t sampleRate, std::uint16_t channels);

  bool isOpen() const { return file_.is_open(); }

  void write       (std::span<const std::int16_t> samples);
  void writeSilence(std::size_t samples);

  bool close();

  std::uint64_t                samplesWritten() const { return samplesWritten_; }
  const std::filesystem::path& path()           const { return path_; }

private:
  std::filesystem::path path_;
  std::ofstream         file_;
  std::uint32_t         sampleRate_     = 0;
  std::uint16_t         channels_       = 0;
  std::uint64_t         samplesWritten_ = 0;
};

} } } // namespace DST::DESK::Core

#endif // DST_DESK_CORE_WAVWRITER_HPP
