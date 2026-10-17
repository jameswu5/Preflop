#include "solver/cards.h"

#include <algorithm>

namespace pf {

static const char RANKS[] = "AKQJT98765432";  // grid order: row/col 0 = A

int cardIndex(Card c) { return c.rank * 4 + c.suit; }
Card cardFromIndex(int i) { return {uint8_t(i / 4), uint8_t(i % 4)}; }

int comboIndex(Card a, Card b) {
    int i = cardIndex(a), j = cardIndex(b);
    if (i > j) std::swap(i, j);
    return j * (j - 1) / 2 + i;
}

int bucket(Card a, Card b) {
    int hi = 12 - std::max(a.rank, b.rank), lo = 12 - std::min(a.rank, b.rank);  // grid indices
    return a.suit == b.suit ? hi * 13 + lo : lo * 13 + hi;
}

std::string bucketName(int b) {
    int row = b / 13, col = b % 13;
    if (row == col) return {RANKS[row], RANKS[row]};
    if (row < col) return {RANKS[row], RANKS[col], 's'};
    return {RANKS[col], RANKS[row], 'o'};
}

int bucketFromName(const std::string& name) {
    for (int b = 0; b < NBUCKETS; ++b)
        if (bucketName(b) == name) return b;
    return -1;
}

}  // namespace pf
