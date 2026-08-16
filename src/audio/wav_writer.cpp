#include "wav_writer.h"

#include <algorithm>
#include <array>
#include <vector>

namespace dst {
namespace {

constexpr std::uint16_t kBitsPerSample = 16;
constexpr std::streamoff kRiffSizeOffset = 4;
constexpr std::streamoff kDataSizeOffset = 40;
constexpr std::size_t kHeaderBytes = 44;

// RIFF is little-endian by definition, so these are written byte by byte rather than
// memcpy'd from host integers.
void putU32(std::ostream& out, std::uint32_t v) {
    const std::array<char, 4> bytes{
        static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
        static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    out.write(bytes.data(), bytes.size());
}

void putU16(std::ostream& out, std::uint16_t v) {
    const std::array<char, 2> bytes{static_cast<char>(v & 0xFF),
                                    static_cast<char>((v >> 8) & 0xFF)};
    out.write(bytes.data(), bytes.size());
}

void putTag(std::ostream& out, const char (&tag)[5]) { out.write(tag, 4); }

}  // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::filesystem::path& path, std::uint32_t sampleRate,
                     std::uint16_t channels) {
    close();

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_) {
        return false;
    }

    path_ = path;
    sampleRate_ = sampleRate;
    channels_ = channels;
    samplesWritten_ = 0;

    const std::uint32_t byteRate =
        sampleRate * channels * (kBitsPerSample / 8);
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * (kBitsPerSample / 8));

    putTag(file_, "RIFF");
    putU32(file_, 0);  // patched by close()
    putTag(file_, "WAVE");
    putTag(file_, "fmt ");
    putU32(file_, 16);  // PCM fmt chunk size
    putU16(file_, 1);   // format 1 = PCM
    putU16(file_, channels);
    putU32(file_, sampleRate);
    putU32(file_, byteRate);
    putU16(file_, blockAlign);
    putU16(file_, kBitsPerSample);
    putTag(file_, "data");
    putU32(file_, 0);  // patched by close()

    return static_cast<bool>(file_);
}

void WavWriter::write(std::span<const std::int16_t> samples) {
    if (!file_ || samples.empty()) {
        return;
    }
    for (const std::int16_t s : samples) {
        putU16(file_, static_cast<std::uint16_t>(s));
    }
    samplesWritten_ += samples.size();
}

void WavWriter::writeSilence(std::size_t samples) {
    if (!file_ || samples == 0) {
        return;
    }
    // Chunked so a long gap cannot allocate an unbounded buffer.
    constexpr std::size_t kChunk = 4096;
    const std::vector<char> zeros(kChunk * 2, 0);
    std::size_t remaining = samples;
    while (remaining > 0) {
        const std::size_t n = std::min(remaining, kChunk);
        file_.write(zeros.data(), static_cast<std::streamsize>(n * 2));
        remaining -= n;
    }
    samplesWritten_ += samples;
}

bool WavWriter::close() {
    if (!file_.is_open()) {
        return true;
    }

    const std::uint64_t dataBytes = samplesWritten_ * 2;

    file_.seekp(kDataSizeOffset, std::ios::beg);
    putU32(file_, static_cast<std::uint32_t>(dataBytes));

    file_.seekp(kRiffSizeOffset, std::ios::beg);
    putU32(file_, static_cast<std::uint32_t>(kHeaderBytes - 8 + dataBytes));

    const bool ok = static_cast<bool>(file_);
    file_.close();
    return ok;
}

}  // namespace dst
