// Merging two independently transcribed streams into one conversation.
//
// Qt-free on purpose: this is the piece most likely to be subtly wrong, and it is far
// easier to test as plain data than through a live transcription connection.
//
// The ordering rule is a finalisation watermark rather than a fixed delay — see
// decision 13 in dstOMNI/DESIGN.md. Each stream reports how far it has been finalised;
// everything below the minimum across open streams can be committed in start order,
// because neither stream can still produce anything earlier.

#ifndef DST_DESK_CORE_TRANSCRIPT_HPP
#define DST_DESK_CORE_TRANSCRIPT_HPP

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "Protocol.hpp"

namespace DST { namespace DESK { namespace Core {

struct Utterance
{
  Stream::Value stream = Stream::Mic;
  double        start  = 0.0; // seconds on the clock shared by both streams
  double        end    = 0.0;
  std::string   text;

  // Mean word confidence as reported by the engine, 0 when unknown. Kept so the UI can
  // mark uncertain text rather than presenting every guess with equal weight.
  double confidence = 0.0;
};

class TranscriptMerger
{
public:
  void openStream(Stream::Value stream)
  {
    finalizedUpTo_[stream] = 0.0;
    stalled_.erase(stream);
    live_.erase(stream);
  }

  // A closed stream must stop holding the watermark back, or it freezes forever and
  // nothing is ever committed again.
  void closeStream(Stream::Value stream)
  {
    finalizedUpTo_.erase(stream);
    stalled_.erase(stream);
    live_.erase(stream);
  }

  /// A final result. Empty text still advances the watermark: the engine finalises
  /// silence too, and that is what proves how far this stream has progressed.
  void addFinal(Stream::Value stream, double start, double end, std::string text,
                double confidence = 0.0)
  {
    auto it = finalizedUpTo_.find(stream);
    if (it == finalizedUpTo_.end()) return; // stream not open

    // A final describing audio this stream has already finalised is a duplicate. The
    // engine delivers finals in order over a single connection, so the only way to see
    // one is for a second source to be speaking for this stream — a stale connection
    // finalising its buffered tail after being replaced, say. Accepting it would repeat
    // text the reader has already been shown, at a time that no longer matches.
    if (!text.empty() && end <= it->second) return;

    it->second = std::max(it->second, end);
    live_.erase(stream);

    if (!text.empty())
    {
      pending_.push_back(Utterance{ stream, start, end, std::move(text), confidence });
    }
  }

  /// An interim result: shown, never committed. Finals have been observed to be
  /// shorter than the interim before them and to revise words at the start, so interim
  /// text must never be promoted as-is.
  void setInterim(Stream::Value stream, double start, std::string text)
  {
    if (!finalizedUpTo_.contains(stream)) return;

    if (text.empty()) live_.erase(stream);
    else              live_[stream] = Utterance{ stream, start, start, std::move(text), 0.0 };
  }

  /// Marks a stream as producing nothing at all, so it stops holding the watermark
  /// back. A stalled stream is treated like a closed one until it speaks again.
  ///
  /// The caller decides this from the engine's silence over wall-clock time, not from
  /// how far behind the transcript is. Those are different things, and confusing them
  /// was a real bug: during a long sentence the finalised position legitimately trails
  /// the audio by the whole length of that sentence, so any rule based on that distance
  /// fires during ordinary speech and commits text out of order.
  void setStalled(Stream::Value stream, bool stalled)
  {
    if (!finalizedUpTo_.contains(stream)) return;
    if (stalled) stalled_.insert(stream);
    else         stalled_.erase(stream);
  }

  /// Seconds up to which every open, responding stream has finalised.
  ///
  /// A stream whose transcription has died would otherwise hold the minimum down
  /// forever and the transcript would stop advancing while the other stream kept
  /// producing text — the worst failure this class can have, because it looks exactly
  /// like the application being broken. Excluding a stalled stream avoids that without
  /// weakening the ordering guarantee for streams that are actually working.
  double watermark() const
  {
    auto lowest = std::optional<double>{};
    for (const auto& [stream, finalized] : finalizedUpTo_)
    {
      if (stalled_.contains(stream)) continue;
      lowest = lowest.has_value() ? std::min(*lowest, finalized) : finalized;
    }

    // Every stream stalled: nothing can be ordered against anything, so hold.
    return std::max(0.0, lowest.value_or(0.0));
  }

  /// Utterances that can no longer be reordered, in conversational order. Each is
  /// returned exactly once; the caller appends them to what it is already showing.
  std::vector<Utterance> takeCommitted()
  {
    const double upTo = watermark();

    // The test is on start, not end. Every stream has finalised everything before the
    // watermark, so any utterance still to come must start at or after it — which
    // means anything pending that starts earlier can never be preceded and is safe to
    // emit. Testing the end instead would commit a short late utterance ahead of a
    // longer one that began before it, producing exactly the scrambling this exists to
    // prevent. Strict less-than, so a tie at the watermark waits rather than gambling
    // on which side of it a future utterance lands.
    auto split = std::partition(pending_.begin(), pending_.end(),
                                [upTo](const Utterance& uu) { return uu.start < upTo; });

    auto ready = std::vector<Utterance>(pending_.begin(), split);
    pending_.erase(pending_.begin(), split);

    std::sort(ready.begin(), ready.end(), [](const Utterance& aa, const Utterance& bb)
    {
      if (aa.start != bb.start) return aa.start < bb.start;
      return aa.stream < bb.stream; // stable and deterministic on an exact tie
    });

    return ready;
  }

  /// Everything still held back, in order. Used when a session ends: waiting for a
  /// watermark that will never advance would silently discard the last utterances.
  std::vector<Utterance> flush()
  {
    auto ready = std::move(pending_);
    pending_.clear();
    std::sort(ready.begin(), ready.end(), [](const Utterance& aa, const Utterance& bb)
    {
      if (aa.start != bb.start) return aa.start < bb.start;
      return aa.stream < bb.stream;
    });
    return ready;
  }

  /// Current interim text per stream, for the live area beneath the committed text.
  const std::map<Stream::Value, Utterance>& live() const { return live_; }

  std::size_t pendingCount() const { return pending_.size(); }

private:
  std::map<Stream::Value, double>    finalizedUpTo_;
  std::set<Stream::Value>            stalled_;
  std::map<Stream::Value, Utterance> live_;
  std::vector<Utterance>             pending_;
};

} } } // namespace DST::DESK::Core

#endif // DST_DESK_CORE_TRANSCRIPT_HPP
