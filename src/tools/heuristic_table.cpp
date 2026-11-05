// Heuristic realization table (docs/architecture.md, "Version 2", heuristic track): the same table
// as realization_table, with the postflop solve replaced by a threshold-bot rollout.
//
// On each flop both players follow fixed rules on their equity against the opponent's range, as
// narrowed by the opponent's actions so far (never against the opponent's actual cards):
//   - no bet faced: bet when equity >= value or <= bluff. The out-of-position player bets only with
//     the initiative (made the last bet or raise, the preflop raise to begin with) or when nobody has
//     it (the previous street checked through); otherwise checks to the aggressor.
//   - facing a bet: raise when equity >= raise (one raise per street), call when equity >= the pot
//     odds plus margin, otherwise fold. Facing a raise: call or fold the same way.
// Bets are a fraction of the pot per street, raises a multiple of the bet, both capped at the stack.
// A hand settles at the true equity of the two holdings over the remaining cards: at the last street
// played, or as soon as a player is all in. No rake postflop (the solver path has none either).
//
// Each player's path depends only on their own combo, because equity is against a range, so every
// node's reach factorises into one range per player and the rollout is exact per flop: one
// BoardEquity pass per decision and per showdown, no hand sampling. Turns and rivers are enumerated.
//
// Usage: heuristic_table [--strategy file] [--charts dir] [--out file] [--flops N] [--flop 2c7dAs]
//                        [--spots N] [--spot RAISER-CALLER] [--jobs N] [--streets 1-3]
//                        [--value X] [--bluff X] [--raise X] [--margin X] [--bet f,t,r] [--raise-size X]
//   --flops N   the same canonical-flop sample as realization_table (default 0 = all 1755)
//   --flop      one flop instead (repeatable); weight 1 each
//   --spot      only this single-raised-pot spot, e.g. BTN-BB (first match in flopSpots order)
//   --out       write the table there (default: print it only)
//   --self-check  test the rollout against a passive bot and a hand-by-hand replay, then exit
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <mutex>
#include <string>
#include <thread>

#include "postflop/postflop.h"
#include "solver/board_equity.h"
#include "solver/flops.h"
#include "solver/spots.h"

using namespace pf;

struct BotConfig {
    double value = 0.65;      // bet at or above this equity
    double bluff = 0.20;      // ... or at or below it
    double raise = 0.80;      // raise a bet at or above this
    double margin = 0.0;      // call when equity >= pot odds + margin
    double bet[3] = {0.33, 0.75, 0.75};  // bet as a fraction of the pot, per street (realization_table's tree)
    double raiseSize = 3;     // raise to this multiple of the bet faced
    int streets = 3;          // 1 = flop only, settled at flop equity; 3 = to the river
};

static const char* SEAT[] = {"UTG", "HJ", "CO", "BTN", "SB", "BB"};

using Sums = std::array<double, NCOMBOS>;

// One flop's rollout for one spot. Player 0 is out of position.
class Rollout {
public:
    Rollout(const BotConfig& c, const std::array<int, 3>& flop, double pot, double stack, int raiserSide)
        : cfg_(c), pot0_(pot), stack0_(stack) {
        board_ = {flop[0], flop[1], flop[2], -1, -1};
        boards_[key(3)] = std::make_unique<BoardEquity>(board_.data(), 3);
        init_ = raiserSide;
    }

    // ev / equity / weight per side, as post::Result.
    post::Result run(const std::array<ComboWeights, 2>& range) {
        const BoardEquity& be = eqFor(3);
        std::array<ComboWeights, 2> r = range;
        for (auto& side : r)
            for (int x = 0; x < NCOMBOS; ++x)
                if (be.blocked(x)) side[x] = 0;
        acc_[0].fill(0), acc_[1].fill(0);
        street(3, r, 0, 0, init_, 1.0);
        post::Result res;
        for (int p = 0; p < 2; ++p) {
            Sums num, den;
            be.sums(r[1 - p], num, den);
            double tot = 0;
            for (int x = 0; x < NCOMBOS; ++x) tot += r[p][x] * den[x];
            for (int x = 0; x < NCOMBOS; ++x) {
                if (r[p][x] == 0 || den[x] == 0) continue;
                res.ev[p][x] = float(acc_[p][x] / den[x]);
                res.equity[p][x] = float(num[x] / den[x]);
                res.weight[p][x] = float(r[p][x] * den[x] / tot);
            }
        }
        return res;
    }

private:
    const BotConfig& cfg_;
    double pot0_, stack0_;
    int init_;
    std::array<int, 5> board_;
    std::map<int, std::unique_ptr<BoardEquity>> boards_;  // by key()
    std::array<Sums, 2> acc_;  // per side and combo: sum over opponent combos of weight * chips won

    int key(int n) const { return n == 3 ? -1 : n == 4 ? board_[3] : 52 + board_[3] * 52 + board_[4]; }
    const BoardEquity& eqFor(int n) {
        auto& p = boards_[key(n)];
        if (!p) p = std::make_unique<BoardEquity>(board_.data(), n);
        return *p;
    }

    // Opponent weight disjoint from each combo (the range is already 0 on the board).
    static void disjoint(const ComboWeights& w, Sums& d) {
        double all = 0, card[52] = {};
        for (int y = 0; y < NCOMBOS; ++y)
            if (w[y] > 0) {
                auto [a, b] = comboCards(y);
                all += w[y], card[a] += w[y], card[b] += w[y];
            }
        for (int x = 0; x < NCOMBOS; ++x) {
            auto [a, b] = comboCards(x);
            d[x] = all - card[a] - card[b] + w[x];
        }
    }

    // One side's sums against the other's current range, already computed by its last decision.
    struct Known {
        int side = -1;
        const Sums *num = nullptr, *den = nullptr;
    };

    // Fold: `winner` takes the pot. inv = postflop chips each side has put in.
    void fold(const std::array<ComboWeights, 2>& r, int winner, const double inv[2], double scale, const Known& k) {
        double pot = pot0_ + inv[0] + inv[1];
        for (int p = 0; p < 2; ++p) {
            Sums own;
            if (p != k.side) disjoint(r[1 - p], own);
            const Sums& d = p == k.side ? *k.den : own;
            double won = (p == winner ? pot : 0) - inv[p];
            for (int x = 0; x < NCOMBOS; ++x)
                if (r[p][x] > 0) acc_[p][x] += scale * won * d[x];
        }
    }

    void showdown(int n, const std::array<ComboWeights, 2>& r, const double inv[2], double scale, const Known& k) {
        double pot = pot0_ + inv[0] + inv[1];
        for (int p = 0; p < 2; ++p) {
            Sums ownNum, ownDen;
            if (p != k.side) eqFor(n).sums(r[1 - p], ownNum, ownDen);
            const Sums &num = p == k.side ? *k.num : ownNum, &den = p == k.side ? *k.den : ownDen;
            for (int x = 0; x < NCOMBOS; ++x)
                if (r[p][x] > 0) acc_[p][x] += scale * (num[x] * pot - inv[p] * den[x]);
        }
    }

    // The street on a board of n cards closed with inv chips in from each side: settle or deal on.
    void endStreet(int n, const std::array<ComboWeights, 2>& r, const double inv[2], int init, double scale,
                   const Known& k) {
        if (n - 2 >= cfg_.streets || inv[0] >= stack0_) return showdown(n, r, inv, scale, k);
        bool used[52] = {};
        for (int i = 0; i < n; ++i) used[board_[i]] = true;
        double per = scale / (n == 3 ? 45 : 44);  // cards left for each pair of hands
        for (int c = 0; c < 52; ++c) {
            if (used[c]) continue;
            board_[n] = c;
            auto nextp = std::make_unique<std::array<ComboWeights, 2>>(r);
            auto& next = *nextp;
            bool any[2] = {};
            for (int p = 0; p < 2; ++p)
                for (int x = 0; x < NCOMBOS; ++x) {
                    auto [a, b] = comboCards(x);
                    if (a == c || b == c) next[p][x] = 0;
                    any[p] |= next[p][x] > 0;
                }
            if (any[0] && any[1]) street(n + 1, next, inv[0], inv[1], init, per);
        }
        board_[n] = -1;
    }

    // Start of a street: equal stakes, so inv0 == inv1.
    void street(int n, const std::array<ComboWeights, 2>& r, double inv0, double inv1, int init, double scale) {
        double inv[2] = {inv0, inv1};
        act(n, r, inv, 0, 0, init, scale);
    }

    // `toAct` decides; facing = chips to call, raised = a raise was made this street.
    void act(int n, const std::array<ComboWeights, 2>& r, const double inv[2], int toAct, int raised, int init,
             double scale) {
        int opp = 1 - toAct;
        double facing = inv[opp] - inv[toAct];
        double pot = pot0_ + inv[0] + inv[1], behind = stack0_ - inv[toAct];
        // On the heap: the recursion is up to ~15 nodes deep and worker threads get a 512 KB stack.
        struct Scratch {
            Sums num, den;                        // toAct's sums against r[opp], which this decision leaves unchanged
            std::array<ComboWeights, 3> split{};  // by action: [0] passive (check / fold), [1] call, [2] aggressive
            std::array<ComboWeights, 2> nr;
        };
        auto sc = std::make_unique<Scratch>();
        auto &num = sc->num, &den = sc->den;
        auto& split = sc->split;
        eqFor(n).sums(r[opp], num, den);
        const Known known = {toAct, &num, &den};
        for (int x = 0; x < NCOMBOS; ++x) {
            if (r[toAct][x] == 0) continue;
            double e = den[x] > 0 ? num[x] / den[x] : 0;
            int a;
            if (facing == 0) {
                bool mayBet = toAct == 1 || init != 1;  // OOP checks to an in-position aggressor
                a = mayBet && behind > 0 && (e >= cfg_.value || e <= cfg_.bluff) ? 2 : 0;
            } else {
                double call = std::min(facing, behind);
                if (!raised && behind > facing && e >= cfg_.raise) a = 2;
                else a = e >= call / (pot + call) + cfg_.margin ? 1 : 0;
            }
            split[a][x] = r[toAct][x];
        }
        auto has = [](const ComboWeights& w) {
            for (float v : w) if (v > 0) return true;
            return false;
        };
        for (int a = 0; a < 3; ++a) {
            if (!has(split[a])) continue;
            auto& nr = sc->nr;
            nr = r, nr[toAct] = split[a];
            double ni[2] = {inv[0], inv[1]};
            if (facing == 0 && a == 0) {  // check
                if (toAct == 1) endStreet(n, nr, ni, -1, scale, known);  // checked through: nobody has the initiative
                else act(n, nr, ni, opp, raised, init, scale);
            } else if (facing == 0) {  // bet
                ni[toAct] += std::min(cfg_.bet[n - 3] * pot, behind);
                act(n, nr, ni, opp, raised, toAct, scale);
            } else if (a == 0) {  // fold
                fold(nr, opp, ni, scale, known);
            } else if (a == 1) {  // call
                ni[toAct] += std::min(facing, behind);
                endStreet(n, nr, ni, init, scale, known);
            } else {  // raise to raiseSize x the bet (the only bet this street, so facing == the bet)
                ni[toAct] = std::min(inv[toAct] + cfg_.raiseSize * facing, stack0_);
                act(n, nr, ni, opp, 1, toAct, scale);
            }
        }
    }
};

// realization_table's sample: every k-th canonical flop, weighted by its count.
// ponytail: duplicated from realization_table.cpp (static there); move both to flops.h if a third user appears.
static std::vector<std::pair<std::array<int, 3>, double>> sampleFlops(int n) {
    const auto& all = canonicalFlops();
    size_t step = n > 0 ? std::max<size_t>(1, all.size() / n) : 1;
    std::vector<std::pair<std::array<int, 3>, double>> out;
    for (size_t i = 0; i < all.size() && (n <= 0 || int(out.size()) < n); i += step)
        out.push_back({all[i].cards, double(all[i].count)});
    return out;
}

// --self-check: (1) a bot that never bets realizes exactly its equity, on one street and on three;
// (2) on the flop alone, every combo's ev matches a hand-by-hand replay of each pair of holdings,
// which narrows the ranges along the pair's own line and settles at the pair's exact equity.
static bool selfCheck() {
    std::mt19937 rng(3);
    std::array<int, 3> flop = {33, 29, 18};  // Tc9c6d
    BoardEquity be(flop.data(), 3);
    std::array<ComboWeights, 2> range{};
    for (int p = 0; p < 2; ++p)
        for (int k = 0; k < 60; ++k) {
            int x = std::uniform_int_distribution<int>(0, NCOMBOS - 1)(rng);
            if (!be.blocked(x)) range[p][x] = std::uniform_real_distribution<float>(0.2f, 1)(rng);
        }
    double pot = 6, stack = 20;  // shallow, so the all-in paths are reached too
    bool ok = true;
    for (int streets : {1, 3}) {
        BotConfig passive;
        passive.value = passive.raise = 2, passive.bluff = -1, passive.streets = streets;
        post::Result r = Rollout(passive, flop, pot, stack, 1).run(range);
        for (int p = 0; p < 2; ++p)
            for (int x = 0; x < NCOMBOS; ++x)
                if (r.weight[p][x] > 0 && std::fabs(r.ev[p][x] - r.equity[p][x] * pot) > 1e-4)
                    std::printf("passive streets=%d side %d combo %d: ev %.5f != eq * pot %.5f\n", streets, p, x,
                                r.ev[p][x], r.equity[p][x] * pot), ok = false;
    }

    BotConfig cfg;
    cfg.streets = 1, cfg.value = 0.6, cfg.bluff = 0.3, cfg.raise = 0.7;
    for (int raiserSide : {0, 1}) {
        post::Result res = Rollout(cfg, flop, pot, stack, raiserSide).run(range);
        // Each node by its action string: both ranges there, and the actor's decision per combo.
        struct Node {
            std::array<ComboWeights, 2> r;
            std::array<int, NCOMBOS> act;
        };
        std::map<std::string, Node> nodes;
        auto node = [&](const std::string& h, const std::array<ComboWeights, 2>& r, int toAct, double facing,
                        double potNow, double behind, bool raised, int init) -> Node& {
            auto it = nodes.find(h);
            if (it != nodes.end()) return it->second;
            Node& n = nodes[h];
            n.r = r;
            ComboWeights eq;
            be.equity(r[1 - toAct], eq);
            for (int x = 0; x < NCOMBOS; ++x) {
                double e = eq[x];
                if (facing == 0) n.act[x] = (toAct == 1 || init != 1) && (e >= cfg.value || e <= cfg.bluff) ? 2 : 0;
                else if (!raised && behind > facing && e >= cfg.raise) n.act[x] = 2;
                else n.act[x] = e >= std::min(facing, behind) / (potNow + std::min(facing, behind)) + cfg.margin ? 1 : 0;
            }
            return n;
        };
        std::array<ComboWeights, NCOMBOS>* pairEq = new std::array<ComboWeights, NCOMBOS>{};  // [y][x] = eq(x, y)
        for (int y = 0; y < NCOMBOS; ++y) {
            if (range[0][y] == 0 && range[1][y] == 0) continue;
            ComboWeights one{};
            one[y] = 1;
            be.equity(one, (*pairEq)[y]);
        }
        // Replay (x for side 0, y for side 1): returns side 0's chips won from the flop on.
        auto replay = [&](int x, int y) {
            std::array<ComboWeights, 2> r = range;
            double inv[2] = {0, 0};
            int toAct = 0, init = raiserSide;
            bool raised = false;
            std::string h;
            for (;;) {
                int opp = 1 - toAct;
                double facing = inv[opp] - inv[toAct], potNow = pot + inv[0] + inv[1], behind = stack - inv[toAct];
                Node& n = node(h, r, toAct, facing, potNow, behind, raised, init);
                int a = n.act[toAct == 0 ? x : y];
                for (int z = 0; z < NCOMBOS; ++z)
                    if (n.act[z] != a) r[toAct][z] = 0;
                h += char('0' + a);
                if (facing == 0 && a == 0) {
                    if (toAct == 1) return (*pairEq)[y][x] * potNow - inv[0];
                } else if (facing == 0) {
                    inv[toAct] += std::min(cfg.bet[0] * potNow, behind), init = toAct;
                } else if (a == 0) {
                    return toAct == 0 ? -inv[0] : potNow - inv[0];
                } else if (a == 1) {
                    inv[toAct] += std::min(facing, behind);
                    return (*pairEq)[y][x] * (pot + inv[0] + inv[1]) - inv[0];
                } else {
                    inv[toAct] = std::min(inv[toAct] + cfg.raiseSize * facing, stack), raised = true, init = toAct;
                }
                toAct = opp;
            }
        };
        double worst = 0;
        for (int x = 0; x < NCOMBOS; ++x) {
            if (range[0][x] == 0) continue;
            auto cx = comboCards(x);
            double num = 0, den = 0;
            for (int y = 0; y < NCOMBOS; ++y) {
                auto cy = comboCards(y);
                if (range[1][y] == 0 || cx[0] == cy[0] || cx[0] == cy[1] || cx[1] == cy[0] || cx[1] == cy[1]) continue;
                num += range[1][y] * replay(x, y), den += range[1][y];
            }
            if (den > 0) worst = std::max(worst, std::fabs(num / den - res.ev[0][x]));
        }
        delete pairEq;
        std::printf("replay, raiser on side %d: %zu nodes, max |ev - replay| = %.2e\n", raiserSide, nodes.size(), worst);
        ok = ok && worst < 1e-4 && nodes.size() > 3;
    }
    std::printf("self-check %s\n", ok ? "OK" : "FAILED");
    return ok;
}

static int parseCard(const char* s) {
    const char *r = std::strchr("23456789TJQKA", s[0]), *u = std::strchr("cdhs", s[1]);
    return r && u && s[0] && s[1] ? int(r - "23456789TJQKA") * 4 + int(u - "cdhs") : -1;
}

int main(int argc, char** argv) {
    std::string stratPath = DATA_DIR "/strategy_anchored.bin", chartDir, out, spotName;
    BotConfig cfg;
    int nFlops = 0, jobs = 1;
    size_t maxSpots = size_t(-1);
    std::vector<std::pair<std::array<int, 3>, double>> flops;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--self-check") return selfCheck() ? 0 : 1;
        if (i + 1 >= argc) return std::fprintf(stderr, "%s needs a value\n", a.c_str()), 2;
        const char* v = argv[++i];
        if (a == "--strategy") stratPath = v;
        else if (a == "--charts") chartDir = v;
        else if (a == "--out") out = v;
        else if (a == "--flops") nFlops = std::stoi(v);
        else if (a == "--spots") maxSpots = std::stoul(v);
        else if (a == "--spot") spotName = v;
        else if (a == "--jobs") jobs = std::stoi(v);
        else if (a == "--streets") cfg.streets = std::stoi(v);
        else if (a == "--value") cfg.value = std::stod(v);
        else if (a == "--bluff") cfg.bluff = std::stod(v);
        else if (a == "--raise") cfg.raise = std::stod(v);
        else if (a == "--margin") cfg.margin = std::stod(v);
        else if (a == "--raise-size") cfg.raiseSize = std::stod(v);
        else if (a == "--bet") {
            if (std::sscanf(v, "%lf,%lf,%lf", &cfg.bet[0], &cfg.bet[1], &cfg.bet[2]) != 3)
                return std::fprintf(stderr, "--bet wants flop,turn,river\n"), 2;
        } else if (a == "--flop") {
            std::array<int, 3> f;
            for (int k = 0; k < 3; ++k) f[k] = std::strlen(v) == 6 ? parseCard(v + 2 * k) : -1;
            if (f[0] < 0 || f[1] < 0 || f[2] < 0 || f[0] == f[1] || f[0] == f[2] || f[1] == f[2])
                return std::fprintf(stderr, "bad flop '%s'\n", v), 2;
            flops.push_back({f, 1.0});
        } else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    if (cfg.streets < 1 || cfg.streets > 3) return std::fprintf(stderr, "--streets is 1-3\n"), 2;
    Strategy s;
    if (!s.load(stratPath)) return std::fprintf(stderr, "cannot load strategy '%s'\n", stratPath.c_str()), 1;
    if (std::string err; !chartDir.empty() && applyCharts(s, chartDir, nullptr, &err) < 0)
        return std::fprintf(stderr, "%s: %s\n", chartDir.c_str(), err.c_str()), 1;
    if (flops.empty()) flops = sampleFlops(nFlops);

    auto spots = flopSpots(s.config());
    if (!spotName.empty()) {
        std::vector<FlopSpot> keep;
        for (auto& sp : spots)
            if (keep.empty() && sp.pot == PotType::Single && spotName == std::string(SEAT[sp.raiser]) + "-" + SEAT[sp.caller])
                keep.push_back(sp);
        if (keep.empty()) return std::fprintf(stderr, "no single-raised spot %s\n", spotName.c_str()), 1;
        spots = keep;
    }
    if (spots.size() > maxSpots) spots.resize(maxSpots);

    RealizationTable table;
    char settings[256];
    std::snprintf(settings, sizeof settings,
                  "heuristic strategy=%s%s%s value=%g bluff=%g raise=%g margin=%g bet=%g,%g,%g raiseSize=%g streets=%d flops=%zu",
                  stratPath.c_str(), chartDir.empty() ? "" : " charts=", chartDir.c_str(), cfg.value, cfg.bluff, cfg.raise,
                  cfg.margin, cfg.bet[0], cfg.bet[1], cfg.bet[2], cfg.raiseSize, cfg.streets, flops.size());
    table.source = settings;
    for (const FlopSpot& fs : spots) {
        auto t0 = std::chrono::steady_clock::now();
        std::array<ComboWeights, 2> range = {lineRange(s, fs.history, fs.oop()), lineRange(s, fs.history, fs.ip())};
        int raiserSide = fs.oop() == fs.raiser ? 0 : 1;
        RealizationAccum acc(fs);
        std::mutex mu;
        double worst = 0;
        std::atomic<size_t> next = 0;
        std::vector<std::thread> pool;
        for (int j = 0; j < jobs; ++j)
            pool.emplace_back([&] {
                for (size_t k; (k = next++) < flops.size();) {
                    Rollout ro(cfg, flops[k].first, fs.potBB, fs.stackBB, raiserSide);
                    post::Result r = ro.run(range);
                    double z = 0;  // chips are conserved: the weighted evs of both sides sum to the pot
                    for (int p = 0; p < 2; ++p)
                        for (int x = 0; x < NCOMBOS; ++x) z += double(r.weight[p][x]) * r.ev[p][x];
                    std::lock_guard lock(mu);
                    worst = std::max(worst, std::fabs(z - fs.potBB));
                    acc.add(r, flops[k].second);
                }
            });
        for (auto& t : pool) t.join();
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        RealizationEntry e = acc.entry();
        table.entries.push_back(e);
        // Range-level R per role, then per class: R(n) with n the class's share of the role's weight.
        double rr[2] = {}, nn[2] = {};
        for (int role = 0; role < 2; ++role)
            for (int b = 0; b < NBUCKETS; ++b) rr[role] += acc.ev[role][b], nn[role] += acc.eq[role][b];
        std::printf("%d-bet %s vs %s pot %.1f stack %.1f: %zu flops in %.1fs (%.3fs per flop per job)  R raiser %.3f caller %.3f  |sum ev - pot| <= %.1e\n",
                    int(fs.pot) + 2, SEAT[fs.raiser], SEAT[fs.caller], fs.potBB, fs.stackBB, flops.size(), sec,
                    sec * jobs / flops.size(), rr[0] / (nn[0] * fs.potBB), rr[1] / (nn[1] * fs.potBB), worst);
        for (int role = 0; role < 2; ++role) {
            std::printf("  %s:", role ? "caller" : "raiser");
            int col = 0;
            for (int b = 0; b < NBUCKETS; ++b) {
                float rv = role ? e.rCaller[b] : e.rRaiser[b];
                if (rv == 0) continue;
                std::printf("%s %4s %.2f", col++ % 10 ? "" : "\n   ", bucketName(b).c_str(), rv);
            }
            std::printf("\n");
        }
    }
    if (!out.empty()) {
        if (std::string err; !saveRealizationTable(out, table, &err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
        std::printf("wrote %zu entries to %s\n", table.entries.size(), out.c_str());
    }
}
