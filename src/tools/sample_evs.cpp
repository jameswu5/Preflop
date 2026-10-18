// Samples hero playouts at every charted spot for fitting the realization model without solves
// (docs/fit-spec.md, local). Every player follows the strategy; at spot s, per hand class k the hero
// brings there, deals like rollout() and plays out each legal action on every deal. Flop terminals are
// recorded with what payoffs() needs (classes, equities, terminal), everything else is summed per
// (s, k, action) as a constant.
// Usage: sample_evs [--strategy file] [--equity file] [--ref dir] [--out dir] [--ess 1000]
//                   [--max-deals N] [--threads 8] [--seed S] [--only text] [--check]
//   --only: keep ref files whose name contains `text` (spot indices then change; for profiling)
//   --check: Gate 1, EVs from the records vs rollout() at a few spots and classes; writes nothing
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include "solver/range.h"
#include "trainer/trainer.h"

using namespace pf;
namespace fs = std::filesystem;

struct SpotInfo {
    std::string name, category, history;
    Spot sp;  // hand state at the decision (cards are re-dealt), path of the bots' and hero's actions
    uint8_t legal;
    Chart chart;
    std::array<double, NBUCKETS> reach;
};

// One (spot, class): sums over deals and the flop-terminal records, columnar as written.
struct Out {
    double W = 0, W2 = 0, c[3] = {}, csq[3] = {}, cw2x[3] = {};
    double sw2x[3] = {}, sw2x2[3] = {};  // sum w^2 x, sum w^2 x^2 over all playouts: --check's SE
    long deals = 0;
    std::vector<uint32_t> term;
    std::vector<uint8_t> act, cls;
    std::vector<float> w, eq, pay;
};

struct Sampler {
    const Strategy& s;
    const Equity& e;
    const RealizationConfig& rc;
    std::vector<Terminal> term;      // [node * 3 + act], valid at terminal edges
    std::vector<int32_t> flopIndex;  // [node * 3 + act] -> index into the flop terminals, -1 otherwise
    std::vector<uint32_t> flopEdge;
    std::vector<std::array<Card, 2>> combos[NBUCKETS];
    int flopBoards = RolloutConfig{}.flopBoards;

    Sampler(const Strategy& s, const Equity& e) : s(s), e(e), rc(s.realization()) {
        term.resize(size_t(s.tree().size()) * 3);
        flopIndex.assign(term.size(), -1);
        std::mt19937 rng(0);
        walk(s.root(), Hand(rng, s.config()));
        for (int a = 0; a < 52; ++a)
            for (int b = a + 1; b < 52; ++b)
                combos[bucket(cardFromIndex(a), cardFromIndex(b))].push_back({cardFromIndex(a), cardFromIndex(b)});
    }
    void walk(int node, const Hand& h) {
        for (int a = 0; a < 3; ++a) {
            if (!(s.tree().legal[node] >> a & 1)) continue;
            Hand c = h;
            c.apply(Act(a));
            if (int k = s.child(node, Act(a)); k >= 0) {
                walk(k, c);
                continue;
            }
            Terminal& t = term[node * 3 + a] = Terminal(c);
            if (__builtin_popcount(t.live) > 1 && !t.allIn)
                flopIndex[node * 3 + a] = int(flopEdge.size()), flopEdge.push_back(node * 3 + a);
        }
    }

    Out run(const SpotInfo& si, int k, uint32_t seed, int sIdx, double ess, long maxDeals) const {
        std::seed_seq seq{seed, uint32_t(sIdx), uint32_t(k)};
        std::mt19937 rng(seq);
        const Spot& sp = si.sp;
        int hero = sp.user;
        Hand h = sp.hand;
        Out o;
        std::array<double, NPOS> eqCache[2][64];  // [allIn][live mask], per deal
        for (; o.deals < maxDeals && (o.W == 0 || o.W * o.W < ess * o.W2); ++o.deals) {
            h.cards[hero] = combos[k][std::uniform_int_distribution<int>(0, int(combos[k].size()) - 1)(rng)];
            int deck[52], nd = 0;
            for (int i = 0; i < 52; ++i)
                if (i != cardIndex(h.cards[hero][0]) && i != cardIndex(h.cards[hero][1])) deck[nd++] = i;
            for (int j = 0, p = 0; p < NPOS; ++p) {  // partial Fisher-Yates over the 50 cards, as rollout()
                if (p == hero) continue;
                for (int c = 0; c < 2; ++c, ++j) {
                    std::swap(deck[j], deck[j + std::uniform_int_distribution<int>(0, nd - 1 - j)(rng)]);
                    h.cards[p][c] = cardFromIndex(deck[j]);
                }
            }
            double w = historyWeight(s, sp, h.cards);
            if (w == 0) continue;
            o.W += w, o.W2 += w * w;
            uint64_t valid[2] = {0, 0};
            for (int a = 0; a < 3; ++a) {
                if (!(si.legal >> a & 1)) continue;
                Hand g = h;
                int edge = playOn(s, g, sp.node, hero, Act(a), rng);
                double x;
                if (g.folded(hero)) {
                    x = -g.committed(hero);
                } else {
                    const Terminal& t = term[edge];
                    std::array<double, NPOS>& eq = eqCache[t.allIn][t.live];
                    if (__builtin_popcount(t.live) > 1 && !(valid[t.allIn] >> t.live & 1)) {
                        std::array<bool, NPOS> live;
                        for (int p = 0; p < NPOS; ++p) live[p] = t.live >> p & 1;
                        e.compute(h.cards, live, rng, t.allIn ? 1 : flopBoards, eq);
                        valid[t.allIn] |= 1ull << t.live;
                    }
                    x = payoffs(t, h.cards, eq, rc)[hero];
                    if (int fi = flopIndex[edge]; fi >= 0) {
                        o.term.push_back(fi), o.act.push_back(a), o.w.push_back(float(w)), o.pay.push_back(float(x));
                        for (int p = 0; p < NPOS; ++p) {
                            bool in = t.live >> p & 1;
                            o.cls.push_back(in ? bucket(h.cards[p][0], h.cards[p][1]) : 255);
                            o.eq.push_back(in ? float(eq[p]) : 0);
                        }
                        o.sw2x[a] += w * w * x, o.sw2x2[a] += w * w * x * x;
                        continue;
                    }
                }
                o.c[a] += w * x, o.csq[a] += w * w * x * x, o.cw2x[a] += w * w * x;
                o.sw2x[a] += w * w * x, o.sw2x2[a] += w * w * x * x;
            }
        }
        return o;
    }
};

// EV per action from an Out, the spec's formula (records via rec_pay), and its SE (delta method).
static std::array<double, 3> evOf(const Out& o, std::array<double, 3>* se = nullptr) {
    std::array<double, 3> ev{};
    for (int a = 0; a < 3; ++a) ev[a] = o.c[a];
    for (size_t r = 0; r < o.pay.size(); ++r) ev[o.act[r]] += double(o.w[r]) * o.pay[r];
    for (int a = 0; a < 3; ++a) {
        ev[a] /= o.W;
        if (se) (*se)[a] = std::sqrt(std::max(o.sw2x2[a] - 2 * ev[a] * o.sw2x[a] + ev[a] * ev[a] * o.W2, 0.0)) / o.W;
    }
    return ev;
}

// numpy format 1.0 with a fixed 128-byte header, so a streamed file's header can be rewritten once N is known.
static std::string npyHeader(const char* descr, size_t n, int width) {
    std::string d = std::string("{'descr': '") + descr + "', 'fortran_order': False, 'shape': (" + std::to_string(n) +
                    (width > 1 ? ", " + std::to_string(width) + ")" : ",)") + ", }";
    d.resize(128 - 10 - 1, ' ');
    d += '\n';
    return std::string("\x93NUMPY\x01\x00", 8) + char(d.size() & 0xff) + char(d.size() >> 8) + d;
}

template <class T>
static void writeNpy(const fs::path& path, const char* descr, const std::vector<T>& v, int width = 1) {
    std::ofstream f(path, std::ios::binary);
    f << npyHeader(descr, v.size() / width, width);
    f.write(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
}

int main(int argc, char** argv) {
    std::string stratPath = DATA_DIR "/strategy_anchored.bin", eqPath = DATA_DIR "/hu_equity.bin", refDir = REF_DIR,
                outDir = DATA_DIR "/samples", only;
    double ess = 1000;
    long maxDeals = 2000000;
    int threads = 8;
    uint32_t seed = 1;
    bool check = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--check") { check = true; continue; }
        if (i + 1 >= argc) return std::fprintf(stderr, "%s needs a value\n", a.c_str()), 2;
        std::string v = argv[++i];
        if (a == "--strategy") stratPath = v;
        else if (a == "--equity") eqPath = v;
        else if (a == "--ref") refDir = v;
        else if (a == "--out") outDir = v;
        else if (a == "--only") only = v;
        else if (a == "--ess") ess = std::stod(v);
        else if (a == "--max-deals") maxDeals = std::stol(v);
        else if (a == "--threads") threads = std::stoi(v);
        else if (a == "--seed") seed = uint32_t(std::stoul(v));
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    Strategy s;
    if (!s.load(stratPath)) return std::fprintf(stderr, "cannot load %s\n", stratPath.c_str()), 1;
    Equity e;
    if (!e.loadHuTable(eqPath)) return std::fprintf(stderr, "cannot load %s\n", eqPath.c_str()), 1;

    std::vector<fs::path> files;
    if (fs::is_directory(refDir))
        for (auto& f : fs::directory_iterator(refDir))
            if (f.path().extension() == ".txt" && f.path().filename().string().find(only) != std::string::npos)
                files.push_back(f.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) return std::fprintf(stderr, "no charts in %s\n", refDir.c_str()), 1;
    std::vector<SpotInfo> spots;
    std::mt19937 rng0(0);
    for (auto& f : files) {
        Chart c;
        std::string err;
        std::vector<Act> h;
        if (!readChart(f.string(), c, err) || !parseHistory(c.spot, h) || s.nodeFor(h) < 0)
            return std::fprintf(stderr, "%s: bad chart %s\n", f.c_str(), err.c_str()), 1;
        if (c.listedOnly) return std::fprintf(stderr, "%s: scope: listed is not supported\n", f.c_str()), 1;
        Spot sp{Hand(rng0, s.config()), 0, s.root(), {}};
        for (Act a : h) sp.path.push_back({sp.node, a}), sp.hand.apply(a), sp.node = s.child(sp.node, a);
        sp.user = s.actor(sp.node);
        spots.push_back({f.stem().string(), c.category, c.spot, sp, s.tree().legal[sp.node], c, s.reach(h)});
    }
    Sampler sm(s, e);

    if (check) {
        // Multiway flop lines (open, cold call with blinds behind), heads-up, 5-bet all-in raise and call.
        const std::pair<const char*, const char*> cases[] = {
            {"gto_open_lj", "76s"}, {"gto_open_btn", "A5o"}, {"gto_vs_open_btn_vs_co", "T9s"},
            {"gto_vs_open_co_vs_lj", "88"}, {"gto_vs_open_bb_vs_btn", "KJo"}, {"gto_vs_3bet_co_vs_btn", "AJs"},
            {"gto_vs_4bet_bb_vs_btn", "AKo"}, {"gto_vs_5bet_co_vs_bb", "QQ"}};
        double worst = 0;
        for (auto [name, cls] : cases) {
            auto it = std::find_if(spots.begin(), spots.end(), [&](auto& si) { return si.name == name; });
            if (it == spots.end()) return std::fprintf(stderr, "no chart %s\n", name), 1;
            int k = bucketFromName(cls), idx = int(it - spots.begin());
            Out o = sm.run(*it, k, seed, idx, ess, maxDeals);
            std::array<double, 3> se;
            auto ev = evOf(o, &se);
            int multi = 0;
            for (size_t r = 0; r < o.pay.size(); ++r)
                multi += __builtin_popcount(sm.term[sm.flopEdge[o.term[r]]].live) > 2;
            Spot sp = it->sp;
            sp.hand.cards[sp.user] = sm.combos[k][0];
            RolloutConfig rc;
            rc.rc = s.realization(), rc.minEss = ess;
            RolloutStats st;
            std::mt19937 rng(seed + 1000 + idx);
            auto rev = rollout(s, e, sp, rng, rc, &st);
            std::printf("%-24s %-4s ESS %.0f, %zu records (%d multiway)\n", name, cls, o.W * o.W / o.W2, o.pay.size(), multi);
            const char* an[] = {"fold", "call", "raise"};
            for (int a = 0; a < 3; ++a) {
                if (!(it->legal >> a & 1)) continue;
                double z = (ev[a] - rev[a]) / std::max(std::hypot(se[a], st.se[a]), 1e-9);  // folds: exact on both sides
                worst = std::max(worst, std::fabs(z));
                std::printf("    %-5s records %8.3f +- %.3f   rollout %8.3f +- %.3f   z %+.2f\n", an[a], ev[a], se[a], rev[a], st.se[a], z);
            }
        }
        std::printf("max |z| %.2f\n", worst);
        return worst < 3 ? 0 : 1;
    }

    std::vector<std::pair<int, int>> jobs;  // (spot, class) with reach > 0, in index order
    for (int i = 0; i < int(spots.size()); ++i)
        for (int k = 0; k < NBUCKETS; ++k)
            if (spots[i].reach[k] > 0) jobs.push_back({i, k});
    fs::create_directories(outDir);
    const char* recName[] = {"rec_sk", "rec_a", "rec_w", "rec_term", "rec_cls", "rec_eq", "rec_pay"};
    const char* recDescr[] = {"<u4", "|u1", "<f4", "<u4", "|u1", "<f4", "<f4"};
    const int recWidth[] = {1, 1, 1, 1, NPOS, NPOS, 1};
    std::ofstream rec[7];
    for (int c = 0; c < 7; ++c) {
        rec[c].open(fs::path(outDir) / (std::string(recName[c]) + ".npy"), std::ios::binary);
        rec[c] << npyHeader(recDescr[c], 0, recWidth[c]);
    }
    size_t S = spots.size(), nrec = 0;
    std::vector<double> W(S * NBUCKETS), W2(S * NBUCKETS), cst(S * NBUCKETS * 3), cstsq(S * NBUCKETS * 3),
        cstw2x(S * NBUCKETS * 3);
    long deals = 0;
    // Finished jobs are written in job order, so the files don't depend on thread timing.
    std::vector<std::unique_ptr<Out>> done(jobs.size());
    size_t next = 0;
    std::mutex mu;
    std::atomic<size_t> claim{0};
    auto t0 = std::chrono::steady_clock::now();
    auto worker = [&] {
        for (size_t j; (j = claim++) < jobs.size();) {
            auto [si, k] = jobs[j];
            auto o = std::make_unique<Out>(sm.run(spots[si], k, seed, si, ess, maxDeals));
            std::lock_guard lock(mu);
            done[j] = std::move(o);
            for (; next < jobs.size() && done[next]; done[next++].reset()) {
                const Out& d = *done[next];
                uint32_t sk = uint32_t(jobs[next].first * NBUCKETS + jobs[next].second);
                W[sk] = d.W, W2[sk] = d.W2, deals += d.deals;
                for (int a = 0; a < 3; ++a) cst[sk * 3 + a] = d.c[a], cstsq[sk * 3 + a] = d.csq[a], cstw2x[sk * 3 + a] = d.cw2x[a];
                std::vector<uint32_t> skv(d.pay.size(), sk);
                auto put = [&](int c, const auto& v) { rec[c].write(reinterpret_cast<const char*>(v.data()), v.size() * sizeof v[0]); };
                put(0, skv), put(1, d.act), put(2, d.w), put(3, d.term), put(4, d.cls), put(5, d.eq), put(6, d.pay);
                nrec += d.pay.size();
                if ((next + 1) % 100 == 0 || next + 1 == jobs.size())
                    std::fprintf(stderr, "%zu/%zu jobs, %zu records, %.0fs\n", next + 1, jobs.size(), nrec,
                                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    for (int c = 0; c < 7; ++c) rec[c].seekp(0), rec[c] << npyHeader(recDescr[c], nrec, recWidth[c]);

    size_t T = sm.flopEdge.size();
    std::vector<float> net(T), spr(T), committed(T * NPOS);
    std::vector<uint8_t> live(T), reraised(T);
    std::vector<int8_t> last(T), raiser(T);
    for (size_t i = 0; i < T; ++i) {
        const Terminal& t = sm.term[sm.flopEdge[i]];
        net[i] = float(t.net), spr[i] = float(t.spr), live[i] = uint8_t(t.live), last[i] = int8_t(t.last);
        raiser[i] = int8_t(t.lastRaiser), reraised[i] = t.reraised;
        for (int p = 0; p < NPOS; ++p) committed[i * NPOS + p] = float(t.committed[p]);
    }
    fs::path od(outDir);
    writeNpy(od / "term_edge.npy", "<u4", sm.flopEdge), writeNpy(od / "term_net.npy", "<f4", net);
    writeNpy(od / "term_spr.npy", "<f4", spr), writeNpy(od / "term_live.npy", "|u1", live);
    writeNpy(od / "term_last.npy", "|i1", last), writeNpy(od / "term_raiser.npy", "|i1", raiser);
    writeNpy(od / "term_reraised.npy", "|u1", reraised), writeNpy(od / "term_committed.npy", "<f4", committed, NPOS);

    std::vector<float> chart(S * NBUCKETS * 3), weight(S * NBUCKETS);
    std::vector<uint8_t> legal(S);
    std::ofstream tsv(od / "spots.tsv");
    tsv << "index\tname\tcategory\tnode\thero_seat\thistory\n";
    for (size_t i = 0; i < S; ++i) {
        const SpotInfo& si = spots[i];
        legal[i] = si.legal;
        for (int k = 0; k < NBUCKETS; ++k) {
            weight[i * NBUCKETS + k] = float(comboCount(k) * si.reach[k]);
            for (int a = 0; a < 3; ++a) chart[(i * NBUCKETS + k) * 3 + a] = si.chart.act[a][k];
        }
        tsv << i << '\t' << si.name << '\t' << si.category << '\t' << si.sp.node << '\t' << si.sp.user << '\t'
            << (si.history.empty() ? "-" : si.history) << '\n';
    }
    writeNpy(od / "sk_W.npy", "<f8", W), writeNpy(od / "sk_W2.npy", "<f8", W2);
    writeNpy(od / "sk_const.npy", "<f8", cst, 3), writeNpy(od / "sk_constsq.npy", "<f8", cstsq, 3);
    writeNpy(od / "sk_constw2x.npy", "<f8", cstw2x, 3);
    writeNpy(od / "sk_chart.npy", "<f4", chart, 3), writeNpy(od / "sk_weight.npy", "<f4", weight);
    writeNpy(od / "sk_legal.npy", "|u1", legal);

    const RealizationConfig& r = s.realization();
    std::ofstream meta(od / "meta.txt");
    auto num = [](double x) { return std::format("{}", x); };  // shortest round-trip
    meta << "strategy " << stratPath << "\nequity " << eqPath << "\nref " << refDir << "\ness " << num(ess)
         << "\nmax_deals " << maxDeals << "\nseed " << seed << "\nflop_boards " << sm.flopBoards << "\nspots " << S
         << "\njobs " << jobs.size() << "\ndeals " << deals << "\nrecords " << nrec << "\nterminals " << T
         << "\nrealization ip=" << num(r.ip) << " oop=" << num(r.oop) << " flush=" << num(r.flush) << " conn=" << num(r.conn)
         << " set=" << num(r.set) << " dom=" << num(r.dom) << " low=" << num(r.low) << " spr=" << num(r.spr) << " init=" << num(r.init)
         << " pairLow=" << num(r.pairLow) << " oopCall=" << num(r.oopCall) << " callerShrink=" << num(r.callerShrink)
         << " ipCall=" << num(r.ipCall) << " domPot=" << num(r.domPot) << "\n";
    std::fprintf(stderr, "%zu jobs, %ld deals, %zu records, %zu flop terminals -> %s\n", jobs.size(), deals, nrec, T,
                 outDir.c_str());
    return 0;
}
