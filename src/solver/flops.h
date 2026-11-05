#pragma once
// Flops up to suit isomorphism (version 2). Relabeling the four suits maps the 22100 flops onto
// 1755 canonical ones; a canonical flop stands for `count` flops, so averaging over canonical
// flops weighted by count is the same as averaging over every flop.
#include <array>
#include <cstdint>
#include <vector>

namespace pf {

using SuitPerm = std::array<uint8_t, 4>;  // perm[suit] = suit after relabeling

struct CanonFlop {
    std::array<int, 3> cards;  // cardIndex() values, ascending
    int count;                 // flops that map here; counts sum to 22100
};

// The 1755 canonical flops, ordered by their cards.
const std::vector<CanonFlop>& canonicalFlops();

// A sample of n canonical flops: every k-th of the list, k = 1755 / n rounded down; all of them for
// n <= 0. Weight each by its count.
// ponytail: a systematic sample over the card-ordered list; a stratified sample if budgets stay tight.
std::vector<CanonFlop> sampleCanonicalFlops(int n);

// Canonical flop of any three distinct cards (any order), and a suit relabeling `perm` that maps
// them onto it. Apply `perm` to hands with permuteCombo() to read them in the canonical frame.
struct FlopClass {
    int canon;  // index into canonicalFlops()
    SuitPerm perm;
};
FlopClass canonicalFlop(int a, int b, int c);

inline int permuteCard(int card, const SuitPerm& p) { return card / 4 * 4 + p[card % 4]; }
int permuteCombo(int combo, const SuitPerm& p);
// Inverse of comboIndex(): the two cardIndex() values, lower first.
std::array<int, 2> comboCards(int combo);

}  // namespace pf
