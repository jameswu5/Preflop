#pragma once
// Card and hand-class indexing shared by the solver, trainer and GUI.
#include <string>

#include "trainer/game.h"

namespace pf {

constexpr int NCOMBOS = 1326, NBUCKETS = 169;

int cardIndex(Card c);             // 0..51 = rank * 4 + suit
Card cardFromIndex(int i);
int comboIndex(Card a, Card b);    // 0..1325, order of a and b doesn't matter

// 0..168 = row * 13 + col of the standard 13x13 range grid: row/col 0 = A ... 12 = 2,
// pairs on the diagonal, suited above it (row < col), offsuit below.
int bucket(Card a, Card b);
std::string bucketName(int b);     // "AA", "AKs", "T9o"
int bucketFromName(const std::string& name);  // -1 if not a valid name

}  // namespace pf
