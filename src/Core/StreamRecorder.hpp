// Protocol-aware sink for one stream: turns a sequence of frames into a continuous
// WAV, inserting silence where frames were dropped.
//
// Gap padding is required by rec/PROTOCOL.md §5.4 and is not cosmetic. sampleIndex is
// the authoritative position, so writing frames back to back after a drop would shift
// everything afterwards permanently — and the same shift would corrupt the word
// timings the transcript ordering depends on.

#ifndef DST_DESK_CORE_STREAMRECORDER_HPP
#define DST_DESK_CORE_STREAMRECORDER_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include "Frame.hpp"
#include "WavWriter.hpp"

namespace DST { namespace DESK { namespace Core {

// HP:TODO: recordings are written in the clear. They are meeting audio, kept for as
// long as the user leaves them there, and nothing here encrypts them or expires them.
// Out of scope for an interview task; it would be the first thing to add for real use,
// along with a retention policy that a user can actually see.
class StreamRecorder
{
public:
  struct Stats
  {
    bool          started          = false;
    std::uint64_t frames           = 0;
    std::uint64_t samples          = 0; // samples actually received
    std::uint64_t paddedSamples    = 0; // silence inserted to cover gaps
    std::uint32_t gaps             = 0;
    std::uint32_t rejected         = 0; // frames dropped as out of order
    bool          writeFailed      = false; // the file stopped accepting audio
    std::uint32_t resyncs          = 0;     // gaps too large to be real, not padded
    std::uint32_t firstSampleIndex = 0;
    std::uint32_t lastSampleIndex  = 0;
  };

  bool open(const std::filesystem::path& path, std::uint32_t sampleRate)
  {
    sampleRate_   = sampleRate;
    stats_        = Stats{};
    nextExpected_ = 0;
    sinceFlush_   = 0;
    return wav_.open(path, sampleRate, 1);
  }

  // Accounting without a file, for a session the user asked not to record.
  //
  // Frame counting is not a side effect of writing audio; it is what shows the pipeline
  // is intact — gaps, rejects and duration are how a dropped or reordered frame becomes
  // visible at all. Turning recording off should stop audio reaching the disk, not stop
  // the application knowing what it received.
  bool openCounting(std::uint32_t sampleRate)
  {
    sampleRate_   = sampleRate;
    stats_        = Stats{};
    nextExpected_ = 0;
    sinceFlush_   = 0;
    counting_     = true;
    return true;
  }

  bool isOpen() const { return wav_.isOpen() || counting_; }
  void close()        { counting_ = false; wav_.close(); }

  // Returns false if the frame was rejected as out of order, per PROTOCOL.md §5.3.
  bool accept(const FrameHeader& header, std::span<const std::int16_t> samples)
  {
    if (!wav_.isOpen() && !counting_) return false;

    if (!stats_.started)
    {
      // The shared capture clock has usually been running before a given stream
      // opens, so the first sampleIndex is arbitrary. The file begins here.
      stats_.started          = true;
      stats_.firstSampleIndex = header.sampleIndex;
      nextExpected_           = header.sampleIndex;
    }
    else if (header.sampleIndex <= stats_.lastSampleIndex)
    {
      // WebSocket runs over TCP, so this should not happen; if it does, something
      // upstream is wrong and replaying it would corrupt the timeline.
      ++stats_.rejected;
      return false;
    }

    if (header.sampleIndex > nextExpected_)
    {
      const std::uint32_t missing = header.sampleIndex - nextExpected_;

      if (missing > maxPadSamples())
      {
        // sampleIndex is a u32 supplied by the client, and until this bound existed it
        // was used directly as a write length: one 1036-byte frame declaring a position
        // four billion samples ahead made the recorder write eight gigabytes of silence
        // to disk, synchronously, with the event loop blocked for all of it. Measured,
        // not imagined — 6.3 GB in two seconds before it was killed.
        //
        // No real gap looks like this. The client bounds its own buffer at about
        // sixteen seconds of audio, so anything past thirty is a fault rather than a
        // drop. Recording continues from the new position instead of stopping: the
        // timeline has a step in it either way, and a recording that keeps going is
        // worth more than one that ends at the first bad frame.
        ++stats_.resyncs;
        nextExpected_ = header.sampleIndex;
      }
      else
      {
        if (wav_.isOpen()) wav_.writeSilence(missing);
        stats_.paddedSamples += missing;
        ++stats_.gaps;
      }
    }

    if (wav_.isOpen()) wav_.write(samples);
    stats_.writeFailed      = wav_.failed();
    stats_.samples         += samples.size();
    stats_.lastSampleIndex  = header.sampleIndex;
    nextExpected_           = header.sampleIndex + header.frameSamples;
    ++stats_.frames;

    // Keep the file playable as it grows. Without this, a process killed mid-session
    // leaves a header claiming zero bytes, and the whole recording is unreadable
    // however much audio is actually on disk.
    if (++sinceFlush_ >= kFlushEveryFrames)
    {
      sinceFlush_ = 0;
      if (wav_.isOpen()) wav_.flush();
    }

    return true;
  }

  const Stats&                 stats() const { return stats_; }
  const std::filesystem::path& path()  const { return wav_.path(); }

  // Seconds of audio in the file, padding included.
  double durationSeconds() const
  {
    if (sampleRate_ == 0) return 0.0;
    const auto written = wav_.isOpen() ? wav_.samplesWritten()
                                       : stats_.samples + stats_.paddedSamples;
    return static_cast<double>(written) / sampleRate_;
  }

private:
  // The largest gap that will be padded rather than treated as a client fault. The
  // sender bounds its own outbound buffer at roughly sixteen seconds of audio, so a
  // genuine drop cannot exceed that by much; this leaves room and still refuses to turn
  // one bad integer into an unbounded write. PROTOCOL.md §5.4.
  static constexpr std::uint32_t kMaxPadSeconds = 30;

  std::uint32_t maxPadSamples() const { return sampleRate_ * kMaxPadSeconds; }

  // Roughly one second of audio at the protocol's frame rate.
  static constexpr std::uint32_t kFlushEveryFrames = 32;

  WavWriter     wav_;
  bool          counting_     = false;
  std::uint32_t sampleRate_   = 0;
  std::uint32_t nextExpected_ = 0;
  std::uint32_t sinceFlush_   = 0;
  Stats         stats_        = {};
};

} } } // namespace DST::DESK::Core

#endif // DST_DESK_CORE_STREAMRECORDER_HPP
