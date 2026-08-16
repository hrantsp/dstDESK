// Protocol-aware sink for one stream: turns a sequence of frames into a continuous
// WAV, inserting silence where frames were dropped.
//
// Gap padding is required by PROTOCOL.md §5.4 and is not cosmetic. sampleIndex is the
// authoritative position, so writing frames back to back after a drop would shift
// everything afterwards permanently — and the same shift would corrupt the word
// timings the transcript ordering depends on.

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include "../protocol/frame.h"
#include "wav_writer.h"

namespace dst {

class StreamRecorder {
public:
    struct Stats {
        bool started = false;
        std::uint64_t frames = 0;
        std::uint64_t samples = 0;        // samples actually received
        std::uint64_t paddedSamples = 0;  // silence inserted to cover gaps
        std::uint32_t gaps = 0;
        std::uint32_t rejected = 0;       // frames dropped as out of order
        std::uint32_t firstSampleIndex = 0;
        std::uint32_t lastSampleIndex = 0;
    };

    bool open(const std::filesystem::path& path, std::uint32_t sampleRate);
    bool isOpen() const { return wav_.isOpen(); }
    void close() { wav_.close(); }

    // Returns false if the frame was rejected as out of order, per PROTOCOL.md §5.3.
    bool accept(const FrameHeader& header, std::span<const std::int16_t> samples);

    const Stats& stats() const { return stats_; }
    const std::filesystem::path& path() const { return wav_.path(); }

    // Seconds of audio in the file, padding included.
    double durationSeconds() const;

private:
    WavWriter wav_;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t nextExpected_ = 0;
    Stats stats_{};
};

}  // namespace dst
