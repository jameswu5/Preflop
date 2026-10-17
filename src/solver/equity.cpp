#include "solver/equity.h"

#include <phevaluator/phevaluator.h>

#include <algorithm>
#include <fstream>

#include "solver/cards.h"

namespace pf {

static int evalIds(const int* id) {
    return 7463 - evaluate_7cards(id[0], id[1], id[2], id[3], id[4], id[5], id[6]);  // PHE: 1 = best
}

int rank7(const Card* c) {
    int id[7];
    for (int i = 0; i < 7; ++i) id[i] = cardIndex(c[i]);
    return evalIds(id);
}

bool Equity::loadHuTable(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<float> t(size_t(NCOMBOS) * NCOMBOS);
    if (!in.read(reinterpret_cast<char*>(t.data()), t.size() * sizeof(float)) || in.peek() != EOF)
        return false;
    hu_ = std::move(t);
    return true;
}

void Equity::compute(const Holes& cards, const std::array<bool, NPOS>& live, std::mt19937& rng,
                     int boards, std::array<double, NPOS>& out) const {
    out.fill(0);
    int players[NPOS], n = 0;
    for (int p = 0; p < NPOS; ++p)
        if (live[p]) players[n++] = p;
    if (n == 1) {
        out[players[0]] = 1;
        return;
    }
    if (n == 2 && !hu_.empty()) {
        int a = players[0], b = players[1];
        double e = hu_[size_t(comboIndex(cards[a][0], cards[a][1])) * NCOMBOS +
                       comboIndex(cards[b][0], cards[b][1])];
        out[a] = e, out[b] = 1 - e;
        return;
    }
    // Deck minus all 12 hole cards; hand[i] = 2 hole ids + 5 board slots.
    bool used[52] = {};
    for (auto& h : cards) used[cardIndex(h[0])] = used[cardIndex(h[1])] = true;
    int deck[52], nd = 0;
    for (int i = 0; i < 52; ++i)
        if (!used[i]) deck[nd++] = i;
    int hand[NPOS][7];
    for (int i = 0; i < n; ++i)
        hand[i][0] = cardIndex(cards[players[i]][0]), hand[i][1] = cardIndex(cards[players[i]][1]);

    for (int k = 0; k < boards; ++k) {
        for (int j = 0; j < 5; ++j) {  // partial Fisher-Yates
            std::swap(deck[j], deck[j + std::uniform_int_distribution<int>(0, nd - 1 - j)(rng)]);
            for (int i = 0; i < n; ++i) hand[i][2 + j] = deck[j];
        }
        int best = 0, winners = 0, score[NPOS];
        for (int i = 0; i < n; ++i) {
            score[i] = evalIds(hand[i]);
            if (score[i] > best) best = score[i], winners = 0;
            winners += score[i] == best;
        }
        for (int i = 0; i < n; ++i)
            if (score[i] == best) out[players[i]] += 1.0 / winners;
    }
    for (int i = 0; i < n; ++i) out[players[i]] /= boards;
}

}  // namespace pf
