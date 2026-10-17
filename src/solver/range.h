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

}  // namespace pf
