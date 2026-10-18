#pragma once
// Range notation for reference charts: comma-separated hand classes with optional frequencies.
//   AA  AKs  AKo  AK (= AKs + AKo)  QQ-22  AKs-A2s  K9o-K5o  22+  ATs+  KTo+  AKs:0.5  QQ-JJ:0.25
// Unlisted classes are 0; a class listed twice keeps the last frequency.
#include <array>
#include <string>

#include "solver/cards.h"

namespace pf {

using RangeFreqs = std::array<float, NBUCKETS>;  // indexed by bucket (solver/cards.h)

// false (and `err` set) on a malformed token or a frequency outside [0, 1].
bool parseRange(const std::string& text, RangeFreqs& out, std::string* err = nullptr);
// Canonical text, one token per non-zero class in grid order ("AA,AKs,AQs:0.5,...").
std::string formatRange(const RangeFreqs& r);

// One reference chart file (docs/validation.md "Reference charts"):
//   # comment              (full-line comments are kept in `comments`; a trailing "# ..." is ignored)
//   spot: FFR              (action history from UTG, F/C/R; empty = UTG's open)
//   raise: AA,KK,A5s:0.5   (fold is the remainder)
//   call: QQ-22
//   category: vs_3bet      (optional, default "other")
//   scope: listed          (optional: only the classes named in raise/call are given)
struct Chart {
    std::string spot, category = "other", comments;
    RangeFreqs act[3] = {};  // per Act: fold (derived), call, raise
    bool given[3] = {};      // raise/call line present
    bool listedOnly = false;
    bool listed(int b) const { return act[1][b] > 0 || act[2][b] > 0; }
};
// false (and `err` set) on an unreadable file, a malformed line, or raise + call > 1.
bool readChart(const std::string& path, Chart& c, std::string& err);

}  // namespace pf
