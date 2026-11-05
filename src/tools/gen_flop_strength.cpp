// Flop hand-strength table (version 2, heuristic track): for every canonical flop (flops.h) and
// every combo, against a uniform random opponent hand,
//   HS    P(ahead now) + P(tied now) / 2, on the five cards of the flop
//   PPot  P(ahead at the river | behind now), ties counting half (Billings et al.)
//   NPot  P(behind at the river | ahead now), ties counting half
// exact over every opponent hand and every turn and river. Combos are in the canonical flop's suits;
// map a real flop with canonicalFlop() and permuteCombo().
// Output data/flop_strength.bin: float[1755][1326][3] = (HS, PPot, NPot), 0 for combos on the flop.
// Usage: gen_flop_strength [--threads 3] [--flops N] [--out path]
//   --flops N: only the first N flops, a timing run; also checks the river equities against
//              BoardEquity, and writes nothing unless --out is given.
#include <phevaluator/phevaluator.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "solver/board_equity.h"
#include "solver/cards.h"
#include "solver/equity.h"
#include "solver/flops.h"

using namespace pf;

static constexpr int NV = 1176;  // combos disjoint from a flop, C(49, 2)

// Pairwise river counts on one flop: win[i][j], tie[i][j] for i < j over the 990 shared runouts.
struct Work {
    std::vector<uint16_t> win = std::vector<uint16_t>(NV * NV), tie = std::vector<uint16_t>(NV * NV);
};

static void flopRow(const CanonFlop& f, Work& wk, float* out, bool check) {
    std::vector<int> combo;  // valid combos in index order
    bool onFlop[52] = {};
    for (int c : f.cards) onFlop[c] = true;
    for (int x = 0; x < NCOMBOS; ++x) {
        auto [a, b] = comboCards(x);
        if (!onFlop[a] && !onFlop[b]) combo.push_back(x);
    }
    std::vector<int> now(NV);
    std::vector<std::array<int, 2>> cards(NV);
    for (int i = 0; i < NV; ++i) {
        cards[i] = comboCards(combo[i]);
        now[i] = 7463 - evaluate_5cards(cards[i][0], cards[i][1], f.cards[0], f.cards[1], f.cards[2]);
    }
    std::fill(wk.win.begin(), wk.win.end(), 0);
    std::fill(wk.tie.begin(), wk.tie.end(), 0);
    std::vector<uint16_t> r(NV), m(NV);
    Card c7[7];
    for (int k = 0; k < 3; ++k) c7[2 + k] = cardFromIndex(f.cards[k]);
    for (int t = 0; t < 52; ++t)
        for (int v = t + 1; v < 52; ++v) {
            if (onFlop[t] || onFlop[v]) continue;
            c7[5] = cardFromIndex(t), c7[6] = cardFromIndex(v);
            for (int i = 0; i < NV; ++i) {
                bool ok = cards[i][0] != t && cards[i][0] != v && cards[i][1] != t && cards[i][1] != v;
                m[i] = ok;
                if (ok) c7[0] = cardFromIndex(cards[i][0]), c7[1] = cardFromIndex(cards[i][1]), r[i] = uint16_t(rank7(c7));
                else r[i] = 0;
            }
            for (int i = 0; i < NV; ++i) {
                if (!m[i]) continue;
                uint16_t ri = r[i], *wr = &wk.win[size_t(i) * NV], *tr = &wk.tie[size_t(i) * NV];
                const uint16_t* rp = r.data();
                const uint16_t* mp = m.data();
                for (int j = i + 1; j < NV; ++j) {
                    wr[j] += mp[j] & (ri > rp[j]);
                    tr[j] += mp[j] & (ri == rp[j]);
                }
            }
        }
    // For each combo: HP[now][river] over disjoint opponents, now/river = 0 ahead, 1 tied, 2 behind.
    std::vector<double> riverEq(NV);
    for (int i = 0; i < NV; ++i) {
        double hp[3][3] = {}, tot[3] = {};
        for (int j = 0; j < NV; ++j) {
            if (j == i || cards[i][0] == cards[j][0] || cards[i][0] == cards[j][1] || cards[i][1] == cards[j][0] ||
                cards[i][1] == cards[j][1])
                continue;
            int lo = std::min(i, j), hi = std::max(i, j);
            double w = wk.win[size_t(lo) * NV + hi] / 990.0, tt = wk.tie[size_t(lo) * NV + hi] / 990.0, l = 1 - w - tt;
            if (i > j) std::swap(w, l);  // counts are from the lower index's side
            int s = now[i] > now[j] ? 0 : now[i] == now[j] ? 1 : 2;
            hp[s][0] += w, hp[s][1] += tt, hp[s][2] += l, tot[s] += 1;
            riverEq[i] += w + tt / 2;
        }
        double n = tot[0] + tot[1] + tot[2];
        riverEq[i] /= n;
        double pDen = tot[2] + tot[1] / 2, nDen = tot[0] + tot[1] / 2;
        float* o = out + size_t(combo[i]) * 3;
        o[0] = float((tot[0] + tot[1] / 2) / n);
        o[1] = pDen > 0 ? float((hp[2][0] + hp[2][1] / 2 + hp[1][0] / 2) / pDen) : 0;
        o[2] = nDen > 0 ? float((hp[0][2] + hp[0][1] / 2 + hp[1][2] / 2) / nDen) : 0;
    }
    if (check) {  // the same river equities by the independent sweep
        BoardEquity be(f.cards.data(), 3);
        ComboWeights uni, eq;
        uni.fill(1);
        be.equity(uni, eq);
        for (int i = 0; i < NV; ++i)
            if (std::fabs(eq[combo[i]] - riverEq[i]) > 1e-5)
                std::fprintf(stderr, "check failed: combo %d %.6f vs %.6f\n", combo[i], eq[combo[i]], riverEq[i]), std::exit(1);
    }
}

int main(int argc, char** argv) {
    int threads = 3, nflops = -1;
    std::string out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (i + 1 >= argc) return std::fprintf(stderr, "%s needs a value\n", a.c_str()), 2;
        if (a == "--threads") threads = std::stoi(argv[++i]);
        else if (a == "--flops") nflops = std::stoi(argv[++i]);
        else if (a == "--out") out = argv[++i];
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    auto& flops = canonicalFlops();
    int n = nflops < 0 ? int(flops.size()) : std::min(nflops, int(flops.size()));
    bool check = nflops >= 0;
    if (out.empty() && !check) out = DATA_DIR "/flop_strength.bin";
    std::vector<float> table(flops.size() * NCOMBOS * 3);
    auto t0 = std::chrono::steady_clock::now();
    std::atomic<int> next = 0;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            Work wk;
            for (int k; (k = next++) < n;) {
                flopRow(flops[k], wk, &table[size_t(k) * NCOMBOS * 3], check);
                if (k % 100 == 0) std::fprintf(stderr, "flop %d/%d\n", k, n);
            }
        });
    for (auto& t : pool) t.join();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d flops in %.1f s (%.3f s per flop per thread, %d threads)\n", n, sec, sec * threads / n, threads);
    if (out.empty()) return 0;
    std::ofstream f(out, std::ios::binary);
    if (!f.write(reinterpret_cast<const char*>(table.data()), table.size() * sizeof(float)))
        return std::fprintf(stderr, "cannot write %s\n", out.c_str()), 1;
    std::printf("wrote %s (%zu bytes)\n", out.c_str(), table.size() * sizeof(float));
}
