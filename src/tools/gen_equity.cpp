// Builds data/hu_equity.bin: exact heads-up equity (all C(48,5) boards) for every pair of disjoint
// combos, as float[comboIndex(a)][comboIndex(b)]. Matchups equal up to suit relabeling or swapping
// sides are enumerated once. Usage: gen_equity [out_path]
#include <phevaluator/phevaluator.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "solver/cards.h"
#include "solver/equity.h"

using namespace pf;

static double exactEquity(int a1, int a2, int b1, int b2) {
    int deck[48], n = 0;
    for (int c = 0; c < 52; ++c)
        if (c != a1 && c != a2 && c != b1 && c != b2) deck[n++] = c;
    long win = 0, tie = 0, total = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            for (int k = j + 1; k < n; ++k)
                for (int l = k + 1; l < n; ++l)
                    for (int m = l + 1; m < n; ++m) {
                        int d0 = deck[i], d1 = deck[j], d2 = deck[k], d3 = deck[l], d4 = deck[m];
                        int ra = evaluate_7cards(a1, a2, d0, d1, d2, d3, d4);  // lower = better
                        int rb = evaluate_7cards(b1, b2, d0, d1, d2, d3, d4);
                        win += ra < rb, tie += ra == rb, ++total;
                    }
    return (win + 0.5 * tie) / total;
}

// Canonical (a1,a2,b1,b2) over suit relabelings and side swaps; `swapped` if sides were swapped.
static int canonical(int a1, int a2, int b1, int b2, bool& swapped) {
    int best = 1 << 30, perm[4] = {0, 1, 2, 3};
    auto key = [](int x1, int x2, int y1, int y2) {
        if (x1 < x2) std::swap(x1, x2);
        if (y1 < y2) std::swap(y1, y2);
        return ((x1 * 52 + x2) * 52 + y1) * 52 + y2;
    };
    do {
        auto m = [&](int c) { return c / 4 * 4 + perm[c % 4]; };
        int k = key(m(a1), m(a2), m(b1), m(b2)), ks = key(m(b1), m(b2), m(a1), m(a2));
        if (k < best) best = k, swapped = false;
        if (ks < best) best = ks, swapped = true;
    } while (std::next_permutation(perm, perm + 4));
    return best;
}

static double seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : DATA_DIR "/hu_equity.bin";
    auto t0 = std::chrono::steady_clock::now();

    // Map every ordered pair of disjoint combos to a canonical matchup.
    std::vector<int> classOf(52 * 52 * 52 * 52, -1), keys;
    std::vector<int> pairClass(size_t(NCOMBOS) * NCOMBOS, -1);
    std::vector<char> pairSwapped(pairClass.size());
    for (int a2 = 0; a2 < 52; ++a2)
        for (int a1 = a2 + 1; a1 < 52; ++a1)
            for (int b2 = 0; b2 < 52; ++b2)
                for (int b1 = b2 + 1; b1 < 52; ++b1) {
                    if (b1 == a1 || b1 == a2 || b2 == a1 || b2 == a2) continue;
                    bool sw;
                    int k = canonical(a1, a2, b1, b2, sw);
                    if (classOf[k] < 0) classOf[k] = int(keys.size()), keys.push_back(k);
                    size_t idx = size_t(comboIndex(cardFromIndex(a1), cardFromIndex(a2))) * NCOMBOS +
                                 comboIndex(cardFromIndex(b1), cardFromIndex(b2));
                    pairClass[idx] = classOf[k], pairSwapped[idx] = sw;
                }
    std::printf("%zu canonical matchups\n", keys.size());

    std::vector<double> eq(keys.size());
    std::atomic<size_t> next{0}, doneCount{0};
    unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < keys.size();) {
                int k = keys[i];
                eq[i] = exactEquity(k / (52 * 52 * 52), k / (52 * 52) % 52, k / 52 % 52, k % 52);
                if (size_t d = ++doneCount; d % 2000 == 0)
                    std::printf("  %zu / %zu (%.0fs)\n", d, keys.size(), seconds(t0)), std::fflush(stdout);
            }
        });
    for (auto& th : pool) th.join();

    std::vector<float> table(pairClass.size(), 0.0f);
    for (size_t i = 0; i < table.size(); ++i)
        if (pairClass[i] >= 0) table[i] = float(pairSwapped[i] ? 1 - eq[pairClass[i]] : eq[pairClass[i]]);
    std::filesystem::create_directories(std::filesystem::path(out).parent_path());
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(table.data()),
                                               table.size() * sizeof(float));

    // Self-check: lookups through the reloaded table match direct enumeration of the raw pair,
    // which catches mistakes in the canonical/swap mapping.
    Equity tab;
    if (!tab.loadHuTable(out)) return std::puts("FAILED: cannot reload table"), 1;
    std::mt19937 rng(1);
    for (int trial = 0; trial < 10; ++trial) {
        int deck[52];
        for (int i = 0; i < 52; ++i) deck[i] = i;
        std::shuffle(deck, deck + 52, rng);
        Holes h;
        for (int p = 0; p < NPOS; ++p) h[p] = {cardFromIndex(deck[2 * p]), cardFromIndex(deck[2 * p + 1])};
        std::array<bool, NPOS> live{};
        live[0] = live[1] = true;
        std::array<double, NPOS> e;
        tab.compute(h, live, rng, 1, e);
        double want = exactEquity(deck[0], deck[1], deck[2], deck[3]);
        if (std::fabs(e[0] - want) > 1e-6 || std::fabs(e[1] - (1 - want)) > 1e-6)
            return std::printf("FAILED: table %.6f vs exact %.6f\n", e[0], want), 1;
    }
    std::printf("wrote %s, self-check ok (%.0fs)\n", out.c_str(), seconds(t0));
}
