// Solves one heads-up postflop spot and prints size, timing, exploitability and realization by class.
// Usage: postflop_solve [--oop file:line] [--ip file:line] [--flop Td9d6h] [--turn c] [--river c]
//                       [--pot 5.5] [--stack 97.5] [--bet 0.5,0.66,0.66] [--raise 2.5] [--max-raises 2]
//                       [--allin-spr 1.5] [--force-spr 0.15] [--merge 0.1]
//                       [--iters 1000] [--target 0.01] [--threads 0] [--compress] [--measure]
//   --oop/--ip   a range from a reference chart: file and its "raise" or "call" line
//                (defaults: BB's call vs the BTN open, and the BTN open)
//   --bet        bet size per street as a fraction of the pot (flop,turn,river); --raise is the
//                multiple of the bet faced, on every street. All-in and merging follow the last three
//                options (TreeConfig in postflop.h).
//   --target     stop at this exploitability, as a fraction of the pot
//   --measure    build the tree and print its size only
// Realization per class = sum(w * ev) / (sum(w * equity) * pot) (postflop.h).
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "postflop/stats.h"
#include "solver/range.h"

using namespace pf;
using namespace pf::post;

static const char *RANKS = "23456789TJQKA", *SUITS = "cdhs";

static int parseCard(const char* s) {
    const char *r = s[0] ? strchr(RANKS, s[0]) : nullptr, *u = s[0] && s[1] ? strchr(SUITS, s[1]) : nullptr;
    if (!r || !u) return -1;
    return cardIndex({uint8_t(r - RANKS), uint8_t(u - SUITS)});
}

static bool loadRange(const std::string& arg, ComboWeights& w) {
    size_t colon = arg.rfind(':');
    std::string path = arg.substr(0, colon), line = colon == std::string::npos ? "raise" : arg.substr(colon + 1);
    Chart c;
    std::string err;
    if (!readChart(path, c, err) && !readChart(std::string(REF_DIR) + "/" + path, c, err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    const RangeFreqs& f = c.act[line == "call" ? 1 : 2];
    for (int b = 1; b < 52; ++b)
        for (int a = 0; a < b; ++a)
            w[comboIndex(cardFromIndex(a), cardFromIndex(b))] = f[bucket(cardFromIndex(a), cardFromIndex(b))];
    return true;
}

static std::vector<double> list(const char* s) {
    std::vector<double> v;
    std::stringstream ss(s);
    for (std::string x; std::getline(ss, x, ',');) v.push_back(std::stod(x));
    return v;
}

int main(int argc, char** argv) {
    std::string oop = "gto_vs_open_bb_vs_btn.txt:call", ip = "gto_open_btn.txt:raise", flop = "Td9d6h";
    Spot spot;
    spot.tree.pot = 5.5, spot.tree.stack = 97.5;
    std::vector<double> bets = {0.5, 0.66, 0.66}, raise = {2.5};
    Options opt;
    bool measureOnly = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--compress") opt.compress = true;
        else if (a == "--measure") measureOnly = true;
        else if (i + 1 >= argc) return std::fprintf(stderr, "missing value for %s\n", a.c_str()), 1;
        else if (++i, a == "--oop") oop = v;
        else if (a == "--ip") ip = v;
        else if (a == "--flop") flop = v;
        else if (a == "--turn") spot.turn = parseCard(v);
        else if (a == "--river") spot.river = parseCard(v);
        else if (a == "--pot") spot.tree.pot = std::stod(v);
        else if (a == "--stack") spot.tree.stack = std::stod(v);
        else if (a == "--bet") bets = list(v);
        else if (a == "--raise") raise = list(v);
        else if (a == "--max-raises") spot.tree.maxRaises = std::stoi(v);
        else if (a == "--allin-spr") spot.tree.addAllinBelowSpr = std::stod(v);
        else if (a == "--force-spr") spot.tree.forceAllinSpr = std::stod(v);
        else if (a == "--merge") spot.tree.mergeFrac = std::stod(v);
        else if (a == "--iters") opt.maxIters = std::stoi(v);
        else if (a == "--target") opt.targetExpl = std::stod(v);
        else if (a == "--threads") opt.threads = std::stoi(v);
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 1;
    }
    if (flop.size() != 6 || bets.size() != 3) return std::fprintf(stderr, "--flop needs 3 cards, --bet 3 sizes\n"), 1;
    for (int i = 0; i < 3; ++i)
        if ((spot.flop[i] = parseCard(flop.c_str() + 2 * i)) < 0) return std::fprintf(stderr, "bad flop %s\n", flop.c_str()), 1;
    for (int s = 0; s < 3; ++s) spot.tree.sizes[s] = {{bets[s]}, raise};
    if (!loadRange(oop, spot.range[0]) || !loadRange(ip, spot.range[1])) return 1;

    if (measureOnly) {
        Stats st = measure(spot, opt);
        std::printf("hands %d / %d, nodes %zu (%zu action), memory %.1f MB\n", st.hands[0], st.hands[1], st.nodes,
                    st.actionNodes, st.bytes / 1e6);
        return 0;
    }
    Stats st;
    Result r = solve(spot, opt, &st);
    std::printf("hands %d / %d, nodes %zu (%zu action), memory %.1f MB\n", st.hands[0], st.hands[1], st.nodes,
                st.actionNodes, st.bytes / 1e6);
    std::printf("build %.2fs, solve %.2fs, %d iterations, exploitability %.3f%% of the pot\n", st.buildSec,
                st.solveSec, r.iterations, 100 * r.exploitability);

    for (int p = 0; p < 2; ++p) {
        double ev[NBUCKETS] = {}, eq[NBUCKETS] = {}, tot = 0;
        for (int b = 1; b < 52; ++b)
            for (int a = 0; a < b; ++a) {
                int c = comboIndex(cardFromIndex(a), cardFromIndex(b)), k = bucket(cardFromIndex(a), cardFromIndex(b));
                ev[k] += r.weight[p][c] * r.ev[p][c], eq[k] += r.weight[p][c] * r.equity[p][c];
                tot += r.weight[p][c] * r.ev[p][c];
            }
        std::printf("\n%s: ev %.3f bb, realization by class\n", p ? "IP" : "OOP", tot);
        for (int row = 0; row < 13; ++row) {
            for (int col = 0; col < 13; ++col) {
                int k = row * 13 + col;
                if (eq[k] > 0) std::printf(" %4s %4.2f", bucketName(k).c_str(), ev[k] / (eq[k] * spot.tree.pot));
                else std::printf(" %4s  .  ", bucketName(k).c_str());
            }
            std::printf("\n");
        }
    }
}
