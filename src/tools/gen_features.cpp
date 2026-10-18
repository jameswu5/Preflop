// Per-class hand features for the realization fit (fit-spec.md Phase 3), Monte Carlo against one
// uniformly random opponent hand: card removal is exact, there are no other dead cards. Writes
// <out>/class_features.npy (f32, 169 x F, rows by bucket()) and <out>/class_features.txt (column names).
// Usage: gen_features [--samples 200000] [--seed 1] [--threads 8] [--out dir]
// Each sample deals a flop, then two independent (opponent hand, turn, river) completions of it; every
// feature averages both, and eq2 is the product of their results (unbiased for E[eq_flop^2]).
// Sanity checks run every time and nothing is written if they fail.
#include <phevaluator/phevaluator.h>
#include <phevaluator/rank.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>

#include "solver/cards.h"
#include "tools/npy.h"

using namespace pf;

// eq, eq2, then the made hands: P(best 7 cards are that category and beat the board alone).
const char* NAMES[] = {"eq", "eq2", "p_pair", "p_twopair", "p_trips", "p_straight", "p_flush", "p_full", "p_nutflush"};
constexpr int F = std::size(NAMES);

static std::array<double, F> features(int k, long samples, uint32_t seed) {
    int h0 = -1, h1 = -1;  // any combo of the class: suits are symmetric against a random hand
    for (int a = 0; a < 52 && h1 < 0; ++a)
        for (int b = a + 1; b < 52 && h1 < 0; ++b)
            if (bucket(cardFromIndex(a), cardFromIndex(b)) == k) h0 = a, h1 = b;
    int d[50], n = 0;
    for (int i = 0; i < 52; ++i)
        if (i != h0 && i != h1) d[n++] = i;
    std::seed_seq seq{seed, uint32_t(k)};
    std::mt19937 rng(seq);
    auto swapIn = [&](int j) { std::swap(d[j], d[j + int((uint64_t(rng()) * uint32_t(n - j)) >> 32)]); };
    std::array<double, F> sum{};
    for (long i = 0; i < samples; ++i) {
        for (int j = 0; j < 3; ++j) swapIn(j);  // flop d[0..2]
        double win[2];
        for (int r = 0; r < 2; ++r) {
            for (int j = 3; j < 7; ++j) swapIn(j);  // opponent d[3], d[4]; turn d[5]; river d[6]
            int hero = evaluate_7cards(h0, h1, d[0], d[1], d[2], d[5], d[6]);  // PHE: 1 = best
            int opp = evaluate_7cards(d[3], d[4], d[0], d[1], d[2], d[5], d[6]);
            win[r] = hero < opp ? 1 : hero == opp ? 0.5 : 0;
            if (hero >= evaluate_5cards(d[0], d[1], d[2], d[5], d[6])) continue;  // the hole cards don't play
            int cat = get_rank_category(hero);
            int col = cat <= FULL_HOUSE ? 7 : cat == FLUSH ? 6 : cat == STRAIGHT ? 5 : cat == THREE_OF_A_KIND ? 4
                      : cat == TWO_PAIR ? 3 : cat == ONE_PAIR ? 2 : -1;
            if (col >= 0) sum[col] += 0.5;
            if (cat != FLUSH && cat != STRAIGHT_FLUSH) continue;
            int count[4] = {}, s = 0;
            const int seven[7] = {h0, h1, d[0], d[1], d[2], d[5], d[6]};
            for (int c : seven) ++count[c & 3];
            while (count[s] < 5) ++s;
            int top = 12;  // highest rank of the flush suit not on the board
            auto onBoard = [&](int c) { return c == d[0] || c == d[1] || c == d[2] || c == d[5] || c == d[6]; };
            while (onBoard(top * 4 + s)) --top;
            sum[8] += 0.5 * (h0 == top * 4 + s || h1 == top * 4 + s);
        }
        sum[0] += (win[0] + win[1]) / 2, sum[1] += win[0] * win[1];
    }
    for (double& x : sum) x /= samples;
    return sum;
}

int main(int argc, char** argv) {
    long samples = 200000;
    uint32_t seed = 1;
    int threads = 8;
    std::string out = DATA_DIR;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i], v = argv[i + 1];
        if (a == "--samples") samples = std::stol(v);
        else if (a == "--seed") seed = uint32_t(std::stoul(v));
        else if (a == "--threads") threads = std::stoi(v);
        else if (a == "--out") out = v;
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    std::vector<std::array<double, F>> t(NBUCKETS);
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i)
        pool.emplace_back([&] {
            for (int k; (k = next++) < NBUCKETS;) t[k] = features(k, samples, seed);
        });
    for (auto& th : pool) th.join();

    auto at = [&](const char* name, int col) { return t[bucketFromName(name)][col]; };
    double suited = 0, offMax = 0, se = 0.6 / std::sqrt(double(samples));  // >= 1.2 SE of eq for any class
    bool ok = std::fabs(at("AA", 0) - 0.852) < 3 * se + 0.002 && std::fabs(at("72o", 0) - 0.346) < 3 * se + 0.002;
    for (int b = 0; b < NBUCKETS; ++b) {
        int r = b / 13, c = b % 13;
        ok &= t[b][1] >= t[b][0] * t[b][0] - 4 * se;  // eq2 >= eq^2 up to noise
        if (r < c) suited += t[b][6] / 78;
        if (r > c) offMax = std::max(offMax, t[b][6]);
        ok &= t[b][8] <= t[b][6] + t[b][7];  // nut flushes are flushes (or straight flushes, in p_full)
    }
    ok &= std::fabs(suited - 0.064) < 0.006 && offMax < 0.03;
    std::printf("AA eq %.4f, 72o eq %.4f, suited p_flush %.4f, offsuit max %.4f\n", at("AA", 0), at("72o", 0), suited, offMax);
    if (!ok) return std::fprintf(stderr, "sanity check failed: nothing written\n"), 1;

    std::vector<float> flat;
    for (auto& row : t) flat.insert(flat.end(), row.begin(), row.end());
    writeNpy(out + "/class_features.npy", "<f4", flat, F);
    std::ofstream names(out + "/class_features.txt");
    for (const char* nm : NAMES) names << nm << '\n';
    return 0;
}
