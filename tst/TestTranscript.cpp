#include <catch2/catch_test_macros.hpp>

#include "Core/Transcript.hpp"

using namespace DST::DESK::Core;

namespace {

std::vector<std::string> textsOf(const std::vector<Utterance>& uu)
{
  auto out = std::vector<std::string>{};
  for (const auto& one : uu) out.push_back(one.text);
  return out;
}

} // namespace

TEST_CASE("nothing is committed until both streams have finalised past it", "[transcript]")
{
  auto merger = TranscriptMerger{};
  merger.openStream(Stream::Mic);
  merger.openStream(Stream::Tab);

  merger.addFinal(Stream::Mic, 0.0, 2.0, "first");

  // The meeting stream has reported nothing, so it may still produce something that
  // belongs before this. Committing now would risk printing them out of order.
  CHECK(merger.watermark() == 0.0);
  CHECK(merger.takeCommitted().empty());

  merger.addFinal(Stream::Tab, 0.0, 2.5, ""); // silence still advances the watermark
  CHECK(merger.watermark() == 2.0);
  CHECK(textsOf(merger.takeCommitted()) == std::vector<std::string>{ "first" });
}

TEST_CASE("an utterance arriving late still lands in the right place", "[transcript]")
{
  // The measured failure case: one stream's final for earlier audio arrives after the
  // other's final for later audio. Appending in arrival order would reverse them.
  auto merger = TranscriptMerger{};
  merger.openStream(Stream::Mic);
  merger.openStream(Stream::Tab);

  merger.addFinal(Stream::Mic, 4.19, 6.10, "The first mouse");
  merger.addFinal(Stream::Tab, 4.00, 6.90, "So you're the Jew Hunter.");

  CHECK(textsOf(merger.takeCommitted()) ==
        std::vector<std::string>{ "So you're the Jew Hunter.", "The first mouse" });
}

TEST_CASE("empty finals advance the watermark without appearing", "[transcript]")
{
  auto merger = TranscriptMerger{};
  merger.openStream(Stream::Mic);
  merger.openStream(Stream::Tab);

  merger.addFinal(Stream::Mic, 0.0, 5.0, "");
  merger.addFinal(Stream::Tab, 0.0, 5.0, "");

  CHECK(merger.watermark() == 5.0);
  CHECK(merger.takeCommitted().empty()); // silence is not text
}

TEST_CASE("a closed stream stops holding the watermark back", "[transcript]")
{
  auto merger = TranscriptMerger{};
  merger.openStream(Stream::Mic);
  merger.openStream(Stream::Tab);

  merger.addFinal(Stream::Mic, 0.0, 3.0, "alone");
  CHECK(merger.takeCommitted().empty()); // the meeting stream is still holding it

  // Without removing it from the minimum, nothing would ever be committed again.
  merger.closeStream(Stream::Tab);
  CHECK(merger.watermark() == 3.0);
  CHECK(textsOf(merger.takeCommitted()) == std::vector<std::string>{ "alone" });
}

TEST_CASE("interim text is shown but never committed", "[transcript]")
{
  auto merger = TranscriptMerger{};
  merger.openStream(Stream::Mic);
  merger.openStream(Stream::Tab);

  merger.setInterim(Stream::Mic, 4.84, "Finding people is my specialty. So, naturally, I work for the");
  CHECK(merger.live().at(Stream::Mic).text.starts_with("Finding people"));
  CHECK(merger.takeCommitted().empty());

  // The real final was shorter than the interim, and "for the" moved to the next
  // segment. Only the final may reach the transcript.
  merger.addFinal(Stream::Mic, 4.84, 8.58, "Finding people is my specialty, so, naturally, I work");
  merger.addFinal(Stream::Tab, 0.0, 9.0, "");

  CHECK(merger.live().find(Stream::Mic) == merger.live().end());
  CHECK(textsOf(merger.takeCommitted()) ==
        std::vector<std::string>{ "Finding people is my specialty, so, naturally, I work" });
}

TEST_CASE("each utterance is committed exactly once", "[transcript]")
{
  auto merger = TranscriptMerger{};
  merger.openStream(Stream::Mic);
  merger.openStream(Stream::Tab);

  merger.addFinal(Stream::Mic, 0.0, 1.0, "one");
  merger.addFinal(Stream::Tab, 0.0, 1.0, "two");

  CHECK(merger.takeCommitted().size() == 2);
  CHECK(merger.takeCommitted().empty());
  CHECK(merger.pendingCount() == 0);
}

TEST_CASE("ending a session flushes what the watermark never reached", "[transcript]")
{
  // A session can end with one stream ahead of the other. Waiting for a watermark
  // that will never advance would silently drop the last thing anyone said.
  auto merger = TranscriptMerger{};
  merger.openStream(Stream::Mic);
  merger.openStream(Stream::Tab);

  merger.addFinal(Stream::Mic, 10.0, 12.0, "last words");
  REQUIRE(merger.takeCommitted().empty());

  CHECK(textsOf(merger.flush()) == std::vector<std::string>{ "last words" });
  CHECK(merger.pendingCount() == 0);
}

TEST_CASE("the merged order matches the measured two-stream session", "[transcript]")
{
  // Replays the arrival order actually observed from two concurrent connections and
  // checks the transcript reads as the conversation did.
  auto merger = TranscriptMerger{};
  merger.openStream(Stream::Mic);
  merger.openStream(Stream::Tab);

  struct Arrival { Stream::Value stream; double start, end; const char* text; };
  const Arrival arrivals[] = {
    { Stream::Mic, 0.00,  3.60, "Two little mice fell in a bucket of cream." },
    { Stream::Mic, 4.19,  6.10, "The first mouse" },
    { Stream::Tab, 4.00,  7.11, "So you're the Jew Hunter. I'm a detective." },
    { Stream::Mic, 6.14,  8.71, "quickly gave up and drowned" },
    { Stream::Tab, 7.11,  8.84, "A damn good detective." },
    { Stream::Mic, 8.71, 10.79, "The second mouse" },
    { Stream::Tab, 8.84, 12.58, "Finding people is my specialty, so, naturally, I work" },
  };
  for (const auto& aa : arrivals) merger.addFinal(aa.stream, aa.start, aa.end, aa.text);

  // The microphone stream has finalised to 10.79 and the meeting stream to 12.58, so
  // the watermark is 10.79 and everything starting before it can be committed — in
  // audio order, not the order it arrived.
  const auto committed = textsOf(merger.takeCommitted());
  CHECK(committed == std::vector<std::string>{
    "Two little mice fell in a bucket of cream.",
    "So you're the Jew Hunter. I'm a detective.",
    "The first mouse",
    "quickly gave up and drowned",
    "A damn good detective.",
    "The second mouse",
    "Finding people is my specialty, so, naturally, I work",
  });
  CHECK(merger.pendingCount() == 0);
}
