// What the transcript view costs as a meeting gets long, and a guard on the one
// property that matters.
//
// This exists because the widget-per-utterance version it replaced was written for a
// demo and verified in sessions of one to two minutes, where nothing it did badly was
// visible. It degraded with the number of lines already shown — 2.7 ms per append at
// 200 lines, 21.6 ms at 1200, past 53 ms at 3400 — so an hour-long meeting slowed until
// it stopped keeping up. Decision 24 records the measurements.
//
// The assertion is therefore about **shape, not speed**. Absolute milliseconds depend on
// the machine, the platform plugin and what else is running, and a threshold in
// milliseconds would be a flaky test that eventually gets deleted. What must stay true is
// that appending line n does not cost more because n-1 lines came before it. The
// tolerance is wide on purpose: the implementation this replaced would fail it by a
// factor of ten, and no correct implementation should come close to it.
//
//   kobayashi-bench            report only, no assertion
//   kobayashi-bench --check    also fail if the cost grows with history
//
// Run under an offscreen platform, since it needs no display:
//   QT_QPA_PLATFORM=offscreen ./bin/Release/kobayashi-bench

#include <QApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <string>
#include "App/TranscriptView.hpp"

using namespace DST::DESK;

namespace {

// Utterance-shaped filler: varying lengths, so rows wrap to different heights and the
// measurement is not of one cached answer repeated.
std::string lineOf(int ii)
{
  static const char* words[] = {
    "the", "meeting", "is", "running", "long", "and", "we", "should", "probably",
    "wrap", "up", "before", "everyone", "loses", "the", "thread", "entirely", "now" };

  auto out = std::string{};
  const int count = 6 + (ii % 14);
  for (int ww = 0; ww < count; ++ww) { out += words[(ii * 7 + ww) % 18]; out += ' '; }
  return out;
}

/// Milliseconds per append over `count` lines, appended onto a view that already holds
/// `existing` of them.
double appendCost(App::TranscriptView& view, QApplication& app, int existing, int count)
{
  auto timer = QElapsedTimer{};
  timer.start();

  for (int ii = 0; ii < count; ++ii)
  {
    auto utterance = Core::Utterance{};
    utterance.stream     = (ii % 3 == 0) ? Core::Stream::Mic : Core::Stream::Tab;
    utterance.start      = (existing + ii) * 3.0;
    utterance.end        = utterance.start + 2.5;
    utterance.text       = lineOf(existing + ii);
    utterance.confidence = 0.9;
    view.append(utterance);

    // A view that is never painted never lays its items out, and a layout is only
    // solved when a LayoutRequest is delivered. Without this the benchmark times the
    // scheduling of work rather than the work, and reports that everything is free.
    if ((ii % 10) == 0) { app.processEvents(); view.grab(); }
  }

  app.processEvents();
  view.grab();
  return double(timer.elapsed()) / count;
}

} // namespace

int main(int argc, char** argv)
{
  auto app = QApplication(argc, argv);
  auto out = QTextStream(stdout);

  bool check = false;
  for (int ii = 1; ii < argc; ++ii)
    if (QString::fromLocal8Bit(argv[ii]) == QStringLiteral("--check")) check = true;

  auto view = App::TranscriptView{};
  view.resize(900, 600);
  view.show();
  app.processEvents();

  // Blocks of the same size at growing depths. Comparing like with like is what makes
  // the ratio meaningful; a running average would hide exactly the growth being looked
  // for.
  constexpr int kBlock  = 200;
  constexpr int kBlocks = 6;

  auto costs = std::vector<double>{};
  out << "  lines    ms/append" << Qt::endl;

  for (int block = 0; block < kBlocks; ++block)
  {
    const int existing = block * kBlock;
    const double cost = appendCost(view, app, existing, kBlock);
    costs.push_back(cost);
    out << QStringLiteral("  %1    %2").arg(existing + kBlock, 5).arg(cost, 9, 'f', 3) << Qt::endl;
  }

  // A search over the whole history, and clearing it again. Clearing was the worst
  // moment in the version this replaced — every hidden row became visible at once and
  // the layout solved all of them — so it is measured explicitly rather than left to be
  // noticed by a user.
  auto timer = QElapsedTimer{};
  timer.start();
  view.setSearch(QStringLiteral("meeting"));
  app.processEvents();
  view.grab();
  const auto searchMs = timer.elapsed();

  timer.restart();
  view.setSearch(QString());
  app.processEvents();
  view.grab();
  const auto clearMs = timer.elapsed();

  out << Qt::endl
      << QStringLiteral("  search over %1 lines : %2 ms").arg(kBlock * kBlocks).arg(searchMs) << Qt::endl
      << QStringLiteral("  clearing the search  : %1 ms").arg(clearMs) << Qt::endl;

  if (!check) return 0;

  // First block against last. Anything that walks the history per append shows up here
  // as a ratio near the number of blocks; anything that does not stays near one.
  const double first = std::max(costs.front(), 0.001);
  const double ratio = costs.back() / first;

  constexpr double kMaxGrowth = 3.0;

  out << Qt::endl
      << QStringLiteral("  growth from first block to last: %1x (limit %2x)")
             .arg(ratio, 0, 'f', 2).arg(kMaxGrowth, 0, 'f', 1)
      << Qt::endl;

  if (ratio > kMaxGrowth)
  {
    out << Qt::endl
        << "  FAIL  appending got slower as the transcript grew. Something is walking\n"
           "        the whole history per line — see decision 24 in dstOMNI/DESIGN.md."
        << Qt::endl;
    return 1;
  }

  out << "  ok    append cost does not grow with the transcript" << Qt::endl;
  return 0;
}
