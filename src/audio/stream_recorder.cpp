#include "stream_recorder.h"

namespace dst {

bool StreamRecorder::open(const std::filesystem::path& path, std::uint32_t sampleRate) {
    sampleRate_ = sampleRate;
    stats_ = Stats{};
    nextExpected_ = 0;
    return wav_.open(path, sampleRate, 1);
}

bool StreamRecorder::accept(const FrameHeader& header,
                            std::span<const std::int16_t> samples) {
    if (!wav_.isOpen()) {
        return false;
    }

    if (!stats_.started) {
        // The shared capture clock has usually been running before a given stream
        // opens, so the first sampleIndex is arbitrary. The file begins here.
        stats_.started = true;
        stats_.firstSampleIndex = header.sampleIndex;
        nextExpected_ = header.sampleIndex;
    } else if (header.sampleIndex <= stats_.lastSampleIndex) {
        // WebSocket runs over TCP, so this should not happen; if it does, something
        // upstream is wrong and replaying it would corrupt the timeline.
        ++stats_.rejected;
        return false;
    }

    if (header.sampleIndex > nextExpected_) {
        const std::uint32_t missing = header.sampleIndex - nextExpected_;
        wav_.writeSilence(missing);
        stats_.paddedSamples += missing;
        ++stats_.gaps;
    }

    wav_.write(samples);
    stats_.samples += samples.size();
    stats_.lastSampleIndex = header.sampleIndex;
    nextExpected_ = header.sampleIndex + header.frameSamples;
    ++stats_.frames;
    return true;
}

double StreamRecorder::durationSeconds() const {
    if (sampleRate_ == 0) {
        return 0.0;
    }
    return static_cast<double>(wav_.samplesWritten()) / sampleRate_;
}

}  // namespace dst
