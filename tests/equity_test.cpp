#undef NDEBUG  // the build defaults to Release; keep asserts on
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "solver/cards.h"
#include "solver/equity.h"

using namespace pf;

static Card card(const char* s) {
    return {uint8_t(std::strchr("23456789TJQKA", s[0]) - "23456789TJQKA"),
            uint8_t(std::strchr("cdhs", s[1]) - "cdhs")};
}

// "AsKs QsJsTs2c3d" -> rank7 of the 7 cards (spaces ignored).
static int r7(const char* s) {
    Card c[7];
    int n = 0;
    for (; *s; ++s)
        if (*s != ' ') c[n++] = card(s), ++s;
    assert(n == 7);
    return rank7(c);
}

static Holes deal(std::initializer_list<const char*> hands) {
    // Seats past the given hands get fixed filler cards so tests are deterministic.
    const char* filler[] = {"4c9d", "5hTs", "6d8s", "3hJc"};
    Holes h;
    int p = 0, f = 0;
    for (auto s : hands) h[p++] = {card(s), card(s + 2)};
    for (; p < NPOS; ++p, ++f) h[p] = {card(filler[f]), card(filler[f] + 2)};
    return h;
}

static std::array<double, NPOS> eq(const Equity& e, const Holes& h, int nlive, int boards = 200000) {
    static std::mt19937 rng(7);
    std::array<bool, NPOS> live{};
    for (int p = 0; p < nlive; ++p) live[p] = true;
    std::array<double, NPOS> out;
    e.compute(h, live, rng, boards, out);
    double sum = 0;
    for (int p = 0; p < NPOS; ++p) sum += out[p], assert(live[p] || out[p] == 0);
    assert(std::fabs(sum - 1) < 1e-9);
    return out;
}

static bool within(double x, double lo, double hi) { return lo <= x && x <= hi; }

int main() {
    {  // combos and buckets
        std::set<int> combos;
        int perBucket[NBUCKETS] = {};
        for (int i = 0; i < 52; ++i)
            for (int j = i + 1; j < 52; ++j) {
                Card a = cardFromIndex(i), b = cardFromIndex(j);
                assert(cardIndex(a) == i && comboIndex(a, b) == comboIndex(b, a));
                assert(comboIndex(a, b) >= 0 && comboIndex(a, b) < NCOMBOS);
                combos.insert(comboIndex(a, b));
                ++perBucket[bucket(a, b)];
            }
        assert(combos.size() == NCOMBOS);
        for (int b = 0; b < NBUCKETS; ++b) {
            int row = b / 13, col = b % 13;
            assert(perBucket[b] == (row == col ? 6 : row < col ? 4 : 12));
            assert(bucketFromName(bucketName(b)) == b);
        }
        assert(bucket(card("As"), card("Ks")) == 1 && bucketName(1) == "AKs");
        assert(bucket(card("Kd"), card("Ah")) == 13 && bucketName(13) == "AKo");
        assert(bucket(card("Ac"), card("Ad")) == 0 && bucket(card("2c"), card("2d")) == 168);
        assert(bucketName(bucket(card("Ts"), card("9s"))) == "T9s");
        assert(bucketFromName("KAs") == -1 && bucketFromName("XX") == -1 && bucketFromName("AAs") == -1);
    }
    {  // evaluator ordering
        int sf = r7("AsKs QsJsTs2c3d"), quads = r7("AcAd AhAsKc2d3h"), fh = r7("KcKd Kh2s2c5d7h"),
            flush = r7("2h5h 7h9hJhAcKd"), straight = r7("5c6d 7h8s9cAcKd"), trips = r7("7c7d 7hKs2d4h9c"),
            twoPair = r7("7c7d KhKs2d4h9c"), pair = r7("7c7d AhKs2d4h9c"), high = r7("7c8d AhKs2d4h9c");
        assert(sf > quads && quads > fh && fh > flush && flush > straight && straight > trips);
        assert(trips > twoPair && twoPair > pair && pair > high);
        int wheel = r7("Ac2d 3h4s5cKdJh"), sixHigh = r7("2c3d 4h5s6cKdJh");
        assert(sixHigh > wheel && wheel > trips);
        assert(r7("2c3d AhKhQhJhTh") == r7("4c5d AhKhQhJhTh"));  // board plays: split
    }
    {  // sampled equity vs exact enumeration of the same 40-card deck (filler seats are dead cards,
       // which moves these 2-4 points off the textbook 81.3% / 42.8% / 12.6%)
        Equity e;
        assert(std::fabs(eq(e, deal({"AsAh", "KdKc"}), 2)[0] - 0.7913) < 0.005);
        assert(std::fabs(eq(e, deal({"AhKd", "QsQc"}), 2)[0] - 0.4674) < 0.005);
        assert(std::fabs(eq(e, deal({"7c2d", "AhAs"}), 2)[0] - 0.1462) < 0.005);
        auto split = eq(e, deal({"AcKd", "AhKs"}), 2);
        assert(within(split[0], 0.47, 0.53) && within(split[1], 0.47, 0.53));
        auto three = eq(e, deal({"AsAh", "KdKc", "QsQc"}), 3);
        assert(three[0] > three[1] && three[1] > three[2] && three[2] > 0.1);
        auto one = eq(e, deal({"7c2d", "AhAs"}), 1);
        assert(one[0] == 1);
        for (int i = 0; i < 100; ++i) {  // single board: whole or split pot only
            auto b1 = eq(e, deal({"AsAh", "KdKc", "QsQc"}), 3, 1);
            for (double x : b1) assert(x == 0 || x == 1 || x == 0.5 || std::fabs(x - 1.0 / 3) < 1e-12);
        }
    }
    {  // heads-up table lookups, if gen_equity has been run
        Equity e;
        if (e.loadHuTable(DATA_DIR "/hu_equity.bin")) {
            auto aa = eq(e, deal({"AsAh", "KdKc"}), 2);
            assert(std::fabs(aa[0] - 0.8126) < 1e-4 && std::fabs(aa[0] + aa[1] - 1) < 1e-6);
            auto kk = eq(e, deal({"KdKc", "AsAh"}), 2);
            assert(std::fabs(kk[0] - aa[1]) < 1e-6);
        } else {
            std::puts("equity_test: no data/hu_equity.bin, skipping table checks (run gen_equity)");
        }
    }
    std::puts("equity_test: ok");
    return 0;
}
