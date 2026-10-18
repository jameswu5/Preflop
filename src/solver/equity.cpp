#include "solver/equity.h"

#include <phevaluator/phevaluator.h>

#include <algorithm>
#include <fstream>

#include "solver/cards.h"

namespace pf {

static int evalIds(const int* id) {
    return 7463 - evaluate_7cards(id[0], id[1], id[2], id[3], id[4], id[5], id[6]);  // PHE: 1 = best
}

// 7-card strength by additive key, faster than PHE's quinary hash. A card's key packs base-5 digits
// for its rank (ranks 2..8 in bits 0-16, 9..A in bits 17-30) and a 4-bit suit count (bits 32-47),
// so a hand's key is the sum of its cards' keys and a board's key is shared by every player. Without
// a flush, lo[]/hi[] map the two rank fields to offsets that sum to a dense index into val[], built
// from evaluate_7cards, so the result is exactly evalIds'.
struct Eval7 {
    static constexpr int SPLIT = 7, NLO = 78125, NHI = 15625;  // 5^7, 5^6
    static constexpr uint64_t FIVES = 0x3333ull << 32, FLUSH = 0x8888ull << 32;  // suit count + 3 >= 8
    uint64_t card[52];
    std::vector<uint16_t> lo = std::vector<uint16_t>(NLO), hi = std::vector<uint16_t>(NHI), val;
    Eval7() {
        for (int id = 0; id < 52; ++id) {
            uint64_t p = 1;
            for (int r = 0; r < (id >> 2) % SPLIT; ++r) p *= 5;
            card[id] = p << (id >> 2 < SPLIT ? 0 : 17) | 1ull << (32 + 4 * (id & 3));
        }
        auto size = [](int code) { int s = 0; for (; code; code /= 5) s += code % 5; return s; };
        int nlo[8] = {}, nhi[8] = {};
        std::vector<int> hiBySize[8];
        for (int c = 0; c < NHI; ++c)
            if (int t = size(c); t <= 7) hi[c] = nhi[t]++, hiBySize[t].push_back(c);
        int base[8] = {};
        for (int c = 0; c < NLO; ++c)
            if (size(c) <= 7) ++nlo[size(c)];
        for (int s = 1; s < 8; ++s) base[s] = base[s - 1] + nlo[s - 1] * nhi[8 - s];
        val.resize(base[7] + nlo[7] * nhi[0]);
        std::fill(nlo, nlo + 8, 0);
        for (int c = 0; c < NLO; ++c) {
            int s = size(c);
            if (s > 7) continue;
            lo[c] = uint16_t(base[s] + nlo[s]++ * nhi[7 - s]);
            for (int h : hiBySize[7 - s]) {
                int id[7], n = 0;  // ranks with these counts; round-robin suits, so no flush
                for (int r = 0, x = c; r < SPLIT; ++r, x /= 5)
                    for (int k = 0; k < x % 5; ++k, ++n) id[n] = r * 4 + n % 4;
                for (int r = SPLIT, x = h; r < 13; ++r, x /= 5)
                    for (int k = 0; k < x % 5; ++k, ++n) id[n] = r * 4 + n % 4;
                val[lo[c] + hi[h]] = uint16_t(evalIds(id));
            }
        }
    }
    static bool flush(uint64_t key) { return (key + FIVES) & FLUSH; }
    int noFlush(uint64_t key) const { return val[lo[key & 0x1FFFF] + hi[key >> 17 & 0x3FFF]]; }
};
static const Eval7 E7;

int rank7(const Card* c) {
    int id[7];
    uint64_t key = 0;
    for (int i = 0; i < 7; ++i) key += E7.card[id[i] = cardIndex(c[i])];
    return Eval7::flush(key) ? evalIds(id) : E7.noFlush(key);
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
    uint64_t key[NPOS] = {};
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c) key[i] += E7.card[hand[i][c] = cardIndex(cards[players[i]][c])];

    // splitmix64 seeded once from rng: mt19937 per card was a large share of solver time.
    uint64_t state = uint64_t(rng()) << 32 | rng();
    auto next32 = [&state] {
        uint64_t z = state += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return uint32_t((z ^ (z >> 31)) >> 32);
    };
    for (int k = 0; k < boards; ++k) {
        uint64_t board = 0;
        for (int j = 0; j < 5; ++j) {  // partial Fisher-Yates
            // Lemire multiply-shift instead of uniform_int_distribution: one draw, bias < 1e-8.
            uint32_t pick = uint32_t((uint64_t(next32()) * uint32_t(nd - j)) >> 32);
            std::swap(deck[j], deck[j + pick]);
            board += E7.card[deck[j]];
        }
        int best = 0, winners = 0, score[NPOS];
        for (int i = 0; i < n; ++i) {
            uint64_t k7 = board + key[i];
            if (Eval7::flush(k7)) {
                std::copy(deck, deck + 5, hand[i] + 2);
                score[i] = evalIds(hand[i]);
            } else {
                score[i] = E7.noFlush(k7);
            }
            if (score[i] > best) best = score[i], winners = 0;
            winners += score[i] == best;
        }
        for (int i = 0; i < n; ++i)
            if (score[i] == best) out[players[i]] += 1.0 / winners;
    }
    for (int i = 0; i < n; ++i) out[players[i]] /= boards;
}

}  // namespace pf
