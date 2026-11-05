// Vector-form discounted CFR over a fixed heads-up bet tree, flop to river (postflop.h).
//
// Every node works on whole ranges: a traversal for player `tr` carries the opponent's reach per
// hand down the tree and returns tr's counterfactual value per hand. Chance nodes deal one
// representative card per suit-isomorphism orbit and map its values onto the other cards of the
// orbit; showdowns pay out in one sweep over hands sorted by strength. Once both players are all-in
// there is no runout subtree: the showdown node sums the sweep over every runout instead.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <unordered_map>

#include "postflop/stats.h"
#include "solver/equity.h"

namespace pf::post {
namespace {

using Vec = std::vector<float>;
enum Kind : uint8_t { ACTION, CHANCE, FOLD, SHOWDOWN };
enum Mode { TRAIN, BEST, EVAL };  // EVAL: both players play the average strategy

struct Node {
    Kind kind;
    uint8_t player = 0;    // ACTION: actor; FOLD: folder
    uint16_t nKids = 0;
    float contrib[2] = {};  // postflop chips put in
    int kids = 0;          // first child in Solver::kids_
    int aux = 0;           // CHANCE: chance_ index; SHOWDOWN: runout_ index; ACTION: scale index
    size_t off = 0;        // ACTION: offset of [action][hand] in the regret and strategy-sum arenas
};

struct Chance {
    std::vector<int> card;                   // dealt card per child
    std::vector<std::array<int, 3>> mapped;  // {child, hand permutation, card} for the other orbit cards, by child
    std::vector<int> first;                  // mapped[first[k] .. first[k + 1]) belong to child k
    float scale = 0;                         // 1 / cards left once both hands are out
    bool parallel = false;
};

struct Runout {  // every completion of a board, by river_ index
    std::vector<int> rivers;
    float scale = 0;  // 1 / completions once both hands are out
    // Turn or flop board: pair[p][h * n_opp + o] = P(p's h beats o) - P(o beats h) over the
    // runouts, 0 for hands sharing a card, so a showdown is one matrix-vector product.
    std::vector<float> pair[2];
};

struct River {  // one complete board: each player's unblocked hands, ascending strength
    std::vector<uint16_t> order[2];
    std::vector<int> strength[2];
};

struct State {
    int street;
    std::vector<int> board;
    double c[2], sc[2];  // postflop chips in, this street's chips in
    int raises, actor;
    std::vector<int> group;  // suit permutations fixing the board and both ranges
};

constexpr double EPS = 1e-9;

// Per-thread stack of float buffers for the traversal: scratch() takes from the top and a Frame
// gives everything taken since it was made back on destruction. Sized for the deepest tree path.
struct Stack {
    std::unique_ptr<float[]> buf{new float[SIZE]};
    size_t top = 0;
    static constexpr size_t SIZE = size_t(1) << 22;
};
thread_local Stack tls;

struct Frame {
    size_t top = tls.top;
    ~Frame() { tls.top = top; }
};

float* scratch(size_t n) {
    float* p = tls.buf.get() + tls.top;
    tls.top += (n + 15) & ~size_t(15);
    assert(tls.top <= Stack::SIZE);
    return p;
}

class Solver {
public:
    Solver(const Spot& spot, const std::vector<int>& board, const Options& opt)
        : spot_(spot), opt_(opt), pot_(spot.tree.pot), stack_(spot.tree.stack), board_(board) {
        threads_ = opt.threads > 0 ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
        uint64_t dead = 0;
        for (int c : board) dead |= 1ull << c;
        for (int p = 0; p < 2; ++p)
            for (int b = 1; b < 52; ++b)
                for (int a = 0; a < b; ++a) {
                    float w = spot.range[p][comboIndex(cardFromIndex(a), cardFromIndex(b))];
                    if (w > 0 && !(dead >> a & 1) && !(dead >> b & 1))
                        hands_[p].push_back({uint8_t(a), uint8_t(b)}), w_[p].push_back(w);
                }
        for (int p = 0; p < 2; ++p)
            for (int h = 0; h < nh(p); ++h) {
                card0_[p].push_back(hands_[p][h][0]), card1_[p].push_back(hands_[p][h][1]);
                blocked_[p][hands_[p][h][0]].push_back(uint16_t(h)), blocked_[p][hands_[p][h][1]].push_back(uint16_t(h));
            }
        for (int p = 0; p < 2; ++p) {
            std::vector<int> at(NCOMBOS, -1);
            for (int h = 0; h < nh(1 - p); ++h) at[combo(1 - p, h)] = h;
            for (int h = 0; h < nh(p); ++h) same_[p].push_back(at[combo(p, h)]);
        }
        buildSymmetry(board, dead);
        root_ = action({int(board.size()) - 3, board, {0, 0}, {0, 0}, 0, 0, group0_});
    }

    Result run() {
        if (opt_.compress) {
            reg16_.assign(arena_, 0), sum16_.assign(arena_, 0);
            regScale_.assign(actions_, 0), sumScale_.assign(actions_, 0);
        } else {
            regret_.assign(arena_, 0), sum_.assign(arena_, 0);
        }
        Result res;
        double expl = exploitability();
        int t = 0;
        while (t < opt_.maxIters && expl > opt_.targetExpl) {
            ++t;
            double d = t - 1, a = std::pow(d, 1.5);
            dpos_ = float(a / (a + 1)), dneg_ = 0.5f, dsum_ = float(std::pow(d / t, 2));
            for (int tr = 0; tr < 2; ++tr) {
                Vec out(nh(tr));
                walk(root_, tr, w_[1 - tr].data(), out.data(), TRAIN);
            }
            if (t % 10 == 0 || t == opt_.maxIters) expl = exploitability();
        }
        res.exploitability = expl, res.iterations = t;

        double z = norm();
        for (int p = 0; p < 2; ++p) {
            Vec ev(nh(p)), compat(nh(p)), eq = equity(p);
            walk(root_, p, w_[1 - p].data(), ev.data(), EVAL);
            compatSums(p, w_[1 - p].data(), compat.data());
            for (int h = 0; h < nh(p); ++h) {
                if (compat[h] <= 0) continue;
                int c = combo(p, h);
                res.ev[p][c] = ev[h] / compat[h];
                res.equity[p][c] = eq[h];
                res.weight[p][c] = float(w_[p][h] * compat[h] / z);
            }
        }
        return res;
    }

    void stats(Stats& st) const {
        st.bytes = 2 * arena_ * (opt_.compress ? 2 : sizeof(float)) + nodes_.size() * sizeof(Node) + kids_.size() * sizeof(int);
        for (const River& rv : river_) st.bytes += (rv.order[0].size() + rv.order[1].size()) * 6;
        for (const Runout& ro : runout_) st.bytes += (ro.pair[0].size() + ro.pair[1].size()) * sizeof(float);
        st.nodes = nodes_.size();
        st.actionNodes = std::count_if(nodes_.begin(), nodes_.end(), [](const Node& n) { return n.kind == ACTION; });
        st.hands[0] = nh(0), st.hands[1] = nh(1);
    }

private:
    const Spot& spot_;
    Options opt_;
    double pot_, stack_;
    int threads_ = 1, root_ = 0;
    std::vector<std::array<uint8_t, 2>> hands_[2];
    Vec w_[2];
    std::vector<uint8_t> card0_[2], card1_[2];      // each hand's two cards
    std::vector<uint16_t> blocked_[2][52];          // hands holding each card
    std::vector<int> same_[2];  // index of the same combo in the other player's list, -1 if absent
    std::array<std::array<int, 4>, 24> perm_{};
    int inv_[24] = {};
    std::vector<int> group0_;
    std::vector<int> hperm_[2][24];  // hand h -> index of the suit-permuted hand
    std::vector<Node> nodes_;
    std::vector<int> kids_;
    std::vector<Chance> chance_;
    std::vector<River> river_;
    std::unordered_map<uint64_t, int> riverId_;
    std::vector<Runout> runout_;
    std::unordered_map<uint64_t, int> runoutId_;
    std::vector<int> board_;
    size_t arena_ = 0;
    int actions_ = 0;
    Vec regret_, sum_;
    // Options::compress: regrets as int16 and strategy sums as uint16, each times a per-node scale.
    std::vector<int16_t> reg16_;
    std::vector<uint16_t> sum16_;
    Vec regScale_, sumScale_;
    float dpos_ = 0, dneg_ = 0, dsum_ = 0;

    int nh(int p) const { return int(hands_[p].size()); }
    int combo(int p, int h) const {
        return comboIndex(cardFromIndex(hands_[p][h][0]), cardFromIndex(hands_[p][h][1]));
    }
    int apply(int k, int card) const { return card / 4 * 4 + perm_[k][card % 4]; }

    void buildSymmetry(const std::vector<int>& board, uint64_t dead) {
        std::array<int, 4> s = {0, 1, 2, 3};
        for (int k = 0; k < 24; ++k, std::next_permutation(s.begin(), s.end())) perm_[k] = s;
        for (int k = 0; k < 24; ++k)
            for (int j = 0; j < 24; ++j)
                if (apply(k, apply(j, 0)) == 0 && apply(k, apply(j, 1)) == 1 && apply(k, apply(j, 2)) == 2)
                    inv_[k] = j;
        for (int k = 0; k < 24; ++k) {
            uint64_t img = 0;
            for (int c : board) img |= 1ull << apply(k, c);
            bool ok = img == dead;
            for (int p = 0; p < 2 && ok; ++p)
                for (int b = 1; b < 52 && ok; ++b)
                    for (int a = 0; a < b && ok; ++a)
                        ok = spot_.range[p][comboIndex(cardFromIndex(a), cardFromIndex(b))] ==
                             spot_.range[p][comboIndex(cardFromIndex(apply(k, a)), cardFromIndex(apply(k, b)))];
            if (!ok) continue;
            group0_.push_back(k);
            for (int p = 0; p < 2; ++p) {
                std::vector<int> at(NCOMBOS, -1);
                for (int h = 0; h < nh(p); ++h) at[combo(p, h)] = h;
                for (int h = 0; h < nh(p); ++h)
                    hperm_[p][k].push_back(at[comboIndex(cardFromIndex(apply(k, hands_[p][h][0])),
                                                         cardFromIndex(apply(k, hands_[p][h][1])))]);
            }
        }
    }

    // ---- tree ----

    int add(Node n) {
        nodes_.push_back(n);
        return int(nodes_.size()) - 1;
    }
    int reserveKids(int n, int count) {
        nodes_[n].kids = int(kids_.size()), nodes_[n].nKids = uint16_t(count);
        kids_.resize(kids_.size() + count);
        return nodes_[n].kids;
    }

    int action(const State& s) {
        int p = s.actor, q = 1 - p;
        double rem = stack_ - s.c[p], facing = s.sc[q] - s.sc[p];
        const StreetSizes& sz = spot_.tree.sizes[s.street];
        const TreeConfig& tc = spot_.tree;
        double pot = pot_ + s.c[0] + s.c[1];
        std::vector<double> adds;  // chips added by each bet or raise
        if (facing <= EPS)
            for (double f : sz.bet) adds.push_back(f * pot);
        else if (s.raises < tc.maxRaises)
            for (double m : sz.raise) adds.push_back(m * s.sc[q] - s.sc[p]);
        if ((facing <= EPS || s.raises < tc.maxRaises) && rem <= tc.addAllinBelowSpr * pot + EPS) adds.push_back(rem);
        for (double& a : adds)
            if (a >= rem || rem - a <= tc.forceAllinSpr * (pot + 2 * a - facing) + EPS) a = rem;
        std::erase_if(adds, [&](double a) { return a <= facing + EPS; });
        std::sort(adds.begin(), adds.end(), std::greater<>());
        std::vector<double> kept;
        for (double a : adds)
            if (kept.empty() || kept.back() - a > std::max(tc.mergeFrac * pot, 1e-6)) kept.push_back(a);
        adds.assign(kept.rbegin(), kept.rend());

        Node nd{ACTION, uint8_t(p)};
        nd.contrib[0] = float(s.c[0]), nd.contrib[1] = float(s.c[1]);
        int n = add(nd), passive = facing > EPS ? 2 : 1;
        int base = reserveKids(n, passive + int(adds.size()));
        nodes_[n].off = arena_, nodes_[n].aux = actions_++;
        arena_ += size_t(passive + adds.size()) * nh(p);

        int k = base;
        if (facing <= EPS) {  // check
            State s2 = s;
            s2.actor = q;
            kids_[k++] = p == 0 ? action(s2) : endStreet(s2);
        } else {
            Node f{FOLD, uint8_t(p)};
            f.contrib[0] = nd.contrib[0], f.contrib[1] = nd.contrib[1];
            kids_[k++] = add(f);
            State s2 = s;
            double call = std::min(facing, rem);
            s2.c[p] += call, s2.sc[p] += call;
            kids_[k++] = endStreet(s2);
        }
        for (double a : adds) {
            State s2 = s;
            s2.c[p] += a, s2.sc[p] += a;
            s2.raises = facing > EPS ? s.raises + 1 : 0;
            s2.actor = q;
            int child = action(s2);
            kids_[k++] = child;
        }
        return n;
    }

    int endStreet(const State& s) {
        if (s.street == 2 || std::min(stack_ - s.c[0], stack_ - s.c[1]) <= EPS) {
            Node nd{SHOWDOWN};
            nd.contrib[0] = float(s.c[0]), nd.contrib[1] = float(s.c[1]);
            nd.aux = runoutFor(s.board);
            return add(nd);
        }
        uint64_t dead = 0;
        for (int c : s.board) dead |= 1ull << c;
        Chance ch;
        ch.scale = 1.0f / float(52 - int(s.board.size()) - 4);
        ch.parallel = s.street == int(board_.size()) - 3;  // the first deal below the root
        std::vector<int> repOf(52, -1);
        for (int c = 0; c < 52; ++c) {
            if (dead >> c & 1) continue;
            int rep = c;
            for (int k : s.group) rep = std::min(rep, apply(k, c));
            if (rep == c) repOf[c] = int(ch.card.size()), ch.card.push_back(c);
        }
        for (int c = 0; c < 52; ++c) {
            if (dead >> c & 1 || repOf[c] >= 0) continue;
            int rep = c;
            for (int k : s.group) rep = std::min(rep, apply(k, c));
            for (int k : s.group)
                if (apply(k, rep) == c) {
                    ch.mapped.push_back({repOf[rep], inv_[k], c});
                    break;
                }
        }
        std::stable_sort(ch.mapped.begin(), ch.mapped.end(), [](auto& x, auto& y) { return x[0] < y[0]; });
        ch.first.assign(ch.card.size() + 1, 0);
        for (auto& mp : ch.mapped) ++ch.first[mp[0] + 1];
        for (size_t k = 0; k < ch.card.size(); ++k) ch.first[k + 1] += ch.first[k];
        Node nd{CHANCE};
        nd.contrib[0] = float(s.c[0]), nd.contrib[1] = float(s.c[1]);
        int n = add(nd);
        nodes_[n].aux = int(chance_.size());
        chance_.push_back(ch);
        int base = reserveKids(n, int(ch.card.size()));
        for (size_t i = 0; i < ch.card.size(); ++i) {
            int c = ch.card[i];
            State s2 = s;
            s2.street++, s2.board.push_back(c), s2.sc[0] = s2.sc[1] = 0, s2.raises = 0, s2.actor = 0;
            std::erase_if(s2.group, [&](int k) { return apply(k, c) != c; });
            int child = action(s2);
            kids_[base + i] = child;
        }
        return n;
    }

    int runoutFor(const std::vector<int>& board) {
        uint64_t mask = 0;
        for (int c : board) mask |= 1ull << c;
        if (auto it = runoutId_.find(mask); it != runoutId_.end()) return it->second;
        runout_.push_back(runouts(board));
        return runoutId_[mask] = int(runout_.size()) - 1;
    }

    Runout runouts(const std::vector<int>& board) {
        uint64_t dead = 0;
        for (int c : board) dead |= 1ull << c;
        Runout ro;
        std::vector<int> b5 = board;
        b5.resize(5);
        int left = 52 - int(board.size()) - 4;
        if (board.size() == 5) ro.rivers.push_back(riverFor(b5)), ro.scale = 1;
        if (board.size() == 4) ro.scale = 1.0f / left;
        if (board.size() == 3) ro.scale = 2.0f / (left * (left - 1));
        for (int t = 0; t < 52 && board.size() == 3; ++t)
            for (int r = t + 1; r < 52 && !(dead >> t & 1); ++r)
                if (!(dead >> r & 1)) b5[3] = t, b5[4] = r, ro.rivers.push_back(riverFor(b5));
        for (int r = 0; r < 52 && board.size() == 4; ++r)
            if (!(dead >> r & 1)) b5[4] = r, ro.rivers.push_back(riverFor(b5));
        if (board.size() == 3) pairTable(ro);  // a turn board: its 44 sweeps are cheaper than the product
        return ro;
    }

    void pairTable(Runout& ro) const {
        int n0 = nh(0), n1 = nh(1);
        std::vector<float> d(size_t(n0) * n1);
        std::vector<int> s0(n0), s1(n1);
        for (int id : ro.rivers) {
            const River& rv = river_[id];
            std::fill(s0.begin(), s0.end(), -1), std::fill(s1.begin(), s1.end(), -1);
            for (size_t i = 0; i < rv.order[0].size(); ++i) s0[rv.order[0][i]] = rv.strength[0][i];
            for (size_t i = 0; i < rv.order[1].size(); ++i) s1[rv.order[1][i]] = rv.strength[1][i];
            for (int h = 0; h < n0; ++h) {
                if (s0[h] < 0) continue;
                float* row = &d[size_t(h) * n1];
                for (int o = 0; o < n1; ++o)
                    row[o] += s1[o] < 0 ? 0.0f : float((s0[h] > s1[o]) - (s0[h] < s1[o]));
            }
        }
        ro.pair[0].resize(d.size()), ro.pair[1].resize(d.size());
        for (int h = 0; h < n0; ++h)
            for (int o = 0; o < n1; ++o) {
                bool overlap = card0_[0][h] == card0_[1][o] || card0_[0][h] == card1_[1][o] ||
                               card1_[0][h] == card0_[1][o] || card1_[0][h] == card1_[1][o];
                float x = overlap ? 0 : d[size_t(h) * n1 + o] * ro.scale;
                ro.pair[0][size_t(h) * n1 + o] = x, ro.pair[1][size_t(o) * n0 + h] = -x;
            }
    }

    int riverFor(const std::vector<int>& board) {
        uint64_t mask = 0;
        for (int c : board) mask |= 1ull << c;
        if (auto it = riverId_.find(mask); it != riverId_.end()) return it->second;
        River rv;
        Card cards[7];
        for (int i = 0; i < 5; ++i) cards[2 + i] = cardFromIndex(board[i]);
        for (int p = 0; p < 2; ++p) {
            std::vector<std::pair<int, int>> v;
            for (int h = 0; h < nh(p); ++h) {
                auto [a, b] = hands_[p][h];
                if (mask >> a & 1 || mask >> b & 1) continue;
                cards[0] = cardFromIndex(a), cards[1] = cardFromIndex(b);
                v.push_back({rank7(cards), h});
            }
            std::sort(v.begin(), v.end());
            for (auto [s, h] : v) rv.strength[p].push_back(s), rv.order[p].push_back(uint16_t(h));
        }
        river_.push_back(std::move(rv));
        return riverId_[mask] = int(river_.size()) - 1;
    }

    // ---- traversal ----

    // compat[h] = opponent reach over hands sharing no card with tr's hand h.
    [[gnu::noinline]] void compatSums(int tr, const float* reach, float* compat) const {
        int op = 1 - tr, no = nh(op), nt = nh(tr);
        const uint8_t *t0 = card0_[tr].data(), *t1 = card1_[tr].data();
        const int* same = same_[tr].data();
        float total = 0, card[52];
        for (int o = 0; o < no; ++o) total += reach[o];
        for (int c = 0; c < 52; ++c) {  // gather per card: no store-to-load chains, unlike scatter
            float x = 0;
            for (uint16_t o : blocked_[op][c]) x += reach[o];
            card[c] = x;
        }
        for (int h = 0; h < nt; ++h)
            compat[h] = total - card[t0[h]] - card[t1[h]] + (same[h] >= 0 ? reach[same[h]] : 0);
    }

    // For tr's hands on this river, out[h] += a * (reach beaten - reach beating) + b * compat. One
    // ascending pass: per group of equal strength, the reach strictly below (W) and up to and
    // including it (WE) give beating = compat - WE, and the pass's final sums give compat. Only
    // the board's hands are counted; tr's blocked hands are left untouched.
    [[gnu::noinline]] void sweep(const River& rv, int tr, const float* reach, float a, float b, float* out) const {
        int op = 1 - tr;
        const uint16_t *ot = rv.order[tr].data(), *oo = rv.order[op].data();
        const int *st = rv.strength[tr].data(), *so = rv.strength[op].data();
        int mt = int(rv.order[tr].size()), mo = int(rv.order[op].size());
        const uint8_t *o0 = card0_[op].data(), *o1 = card1_[op].data(), *t0 = card0_[tr].data(), *t1 = card1_[tr].data();
        const int* same = same_[tr].data();
        Frame f;
        float* w = scratch(mt);  // W + WE per position in ot
        float run = 0, rc[52] = {};
        int j = 0;
        for (int i = 0; i < mt;) {
            int g = i, s = st[i];
            for (; j < mo && so[j] < s; ++j) {
                float r = reach[oo[j]];
                run += r, rc[o0[oo[j]]] += r, rc[o1[oo[j]]] += r;
            }
            for (; g < mt && st[g] == s; ++g) w[g] = run - rc[t0[ot[g]]] - rc[t1[ot[g]]];
            for (; j < mo && so[j] == s; ++j) {
                float r = reach[oo[j]];
                run += r, rc[o0[oo[j]]] += r, rc[o1[oo[j]]] += r;
            }
            for (; i < g; ++i) {
                int h = ot[i];
                w[i] += run - rc[t0[h]] - rc[t1[h]] + (same[h] >= 0 ? reach[same[h]] : 0);
            }
        }
        for (; j < mo; ++j) {  // finish the totals: compat comes from them
            float r = reach[oo[j]];
            run += r, rc[o0[oo[j]]] += r, rc[o1[oo[j]]] += r;
        }
        for (int i = 0; i < mt; ++i) {
            int h = ot[i];
            float compat = run - rc[t0[h]] - rc[t1[h]] + (same[h] >= 0 ? reach[same[h]] : 0);
            out[h] += a * (w[i] - compat) + b * compat;
        }
    }

    // out = a * diff + b * compat, averaged over the runouts.
    void showdown(const Runout& ro, int tr, const float* reach, float a, float b, float* out) const {
        int nt = nh(tr), no = nh(1 - tr);
        if (ro.rivers.size() == 1) {
            std::fill(out, out + nt, 0.0f);
            return sweep(river_[ro.rivers[0]], tr, reach, a, b, out);
        }
        if (ro.pair[tr].empty()) {  // a turn board
            std::fill(out, out + nt, 0.0f);
            for (int id : ro.rivers) sweep(river_[id], tr, reach, a * ro.scale, b * ro.scale, out);
            return;
        }
        compatSums(tr, reach, out);
        const float* d = ro.pair[tr].data();
        for (int h = 0; h < nt; ++h) {
            float x = 0;
            for (int o = 0; o < no; ++o) x += d[size_t(h) * no + o] * reach[o];
            out[h] = a * x + b * out[h];
        }
    }

    [[gnu::noinline]] static void matching(const float* r, int na, int n, float* s, float* tot) {  // regret matching
        std::fill(tot, tot + n, 0.0f);
        for (int a = 0; a < na; ++a)
            for (int h = 0; h < n; ++h) tot[h] += std::max(r[a * n + h], 0.0f);
        for (int h = 0; h < n; ++h) tot[h] = tot[h] > 0 ? 1 / tot[h] : 0;
        float u = 1.0f / na;
        for (int a = 0; a < na; ++a)
            for (int h = 0; h < n; ++h) s[a * n + h] = tot[h] > 0 ? std::max(r[a * n + h], 0.0f) * tot[h] : u;
    }
    [[gnu::noinline]] static void average(const float* sum, int na, int n, float* s, float* tot) {
        std::fill(tot, tot + n, 0.0f);
        for (int a = 0; a < na; ++a)
            for (int h = 0; h < n; ++h) tot[h] += sum[a * n + h];
        for (int h = 0; h < n; ++h) tot[h] = tot[h] > 0 ? 1 / tot[h] : 0;
        float u = 1.0f / na;
        for (int a = 0; a < na; ++a)
            for (int h = 0; h < n; ++h) s[a * n + h] = tot[h] > 0 ? sum[a * n + h] * tot[h] : u;
    }

    // Reach with the hands holding card c removed.
    void dealReach(int op, int c, const float* reach, float* r) const {
        std::copy(reach, reach + nh(op), r);
        for (uint16_t o : blocked_[op][c]) r[o] = 0;
    }

    void walk(int n, int tr, const float* reach, float* out, Mode m) {
        const Node& nd = nodes_[n];
        int op = 1 - tr, nt = nh(tr), no = nh(op);
        Frame frame;
        switch (nd.kind) {
        case FOLD: {
            float pay = nd.player == tr ? -nd.contrib[tr] : float(pot_) + nd.contrib[op];
            compatSums(tr, reach, out);
            for (int h = 0; h < nt; ++h) out[h] *= pay;
            return;
        }
        case SHOWDOWN:
            showdown(runout_[nd.aux], tr, reach, nd.contrib[0] + float(pot_) / 2, float(pot_) / 2, out);
            return;
        case CHANCE: {
            const Chance& ch = chance_[nd.aux];
            int nk = nd.nKids;
            auto finish = [&](int k, float* v) {  // zero tr's blocked hands, add v and its orbit images
                for (uint16_t h : blocked_[tr][ch.card[k]]) v[h] = 0;
                for (int h = 0; h < nt; ++h) out[h] += v[h];
                for (int i = ch.first[k]; i < ch.first[k + 1]; ++i) {
                    const int* hp = hperm_[tr][ch.mapped[i][1]].data();
                    for (int h = 0; h < nt; ++h) out[h] += v[hp[h]];
                }
            };
            std::fill(out, out + nt, 0.0f);
            if (ch.parallel && threads_ > 1) {
                float* vals = scratch(size_t(nk) * nt);
                std::atomic<int> next{0};
                auto work = [&] {
                    Frame f;
                    float* r = scratch(no);
                    for (int k; (k = next++) < nk;) {
                        dealReach(op, ch.card[k], reach, r);
                        walk(kids_[nd.kids + k], tr, r, vals + size_t(k) * nt, m);
                    }
                };
                std::vector<std::thread> pool;
                for (int i = 1; i < std::min(threads_, nk); ++i) pool.emplace_back(work);
                work();
                for (auto& th : pool) th.join();
                for (int k = 0; k < nk; ++k) finish(k, vals + size_t(k) * nt);
            } else {
                float *r = scratch(no), *v = scratch(nt);
                for (int k = 0; k < nk; ++k) {
                    dealReach(op, ch.card[k], reach, r);
                    walk(kids_[nd.kids + k], tr, r, v, m);
                    finish(k, v);
                }
            }
            for (int h = 0; h < nt; ++h) out[h] *= ch.scale;
            return;
        }
        case ACTION: break;
        }

        int na = nd.nKids, p = nd.player, np = nh(p);
        size_t len = size_t(na) * np;
        bool z = opt_.compress, needReg = m == TRAIN, needSum = m == TRAIN ? p != tr : p != tr || m == EVAL;
        float* reg = z ? (needReg ? scratch(len) : nullptr) : regret_.data() + nd.off;
        float* sum = z ? (needSum ? scratch(len) : nullptr) : sum_.data() + nd.off;
        if (z && needReg) decode(reg16_.data() + nd.off, regScale_[nd.aux], len, reg);
        if (z && needSum) decode(sum16_.data() + nd.off, sumScale_[nd.aux], len, sum);
        float *s = scratch(len), *tmp = scratch(np);
        if (m == TRAIN) matching(reg, na, np, s, tmp);
        else if (p != tr || m == EVAL) average(sum, na, np, s, tmp);

        if (p == tr) {
            float* v = scratch(size_t(na) * nt);
            for (int a = 0; a < na; ++a) walk(kids_[nd.kids + a], tr, reach, v + size_t(a) * nt, m);
            if (m == BEST) {
                std::copy(v, v + nt, out);
                for (int a = 1; a < na; ++a)
                    for (int h = 0; h < nt; ++h) out[h] = std::max(out[h], v[a * nt + h]);
                return;
            }
            std::fill(out, out + nt, 0.0f);
            for (int a = 0; a < na; ++a)
                for (int h = 0; h < nt; ++h) out[h] += s[a * nt + h] * v[a * nt + h];
            if (m == TRAIN)
                for (int a = 0; a < na; ++a)
                    for (int h = 0; h < nt; ++h) {
                        float r = reg[a * nt + h];
                        reg[a * nt + h] = r * (r > 0 ? dpos_ : dneg_) + v[a * nt + h] - out[h];
                    }
            if (z && m == TRAIN) regScale_[nd.aux] = encode(reg, len, reg16_.data() + nd.off);
            return;
        }

        std::fill(out, out + nt, 0.0f);
        float *r = scratch(no), *v = scratch(nt);
        for (int a = 0; a < na; ++a) {
            float any = 0;
            for (int o = 0; o < no; ++o) any += r[o] = reach[o] * s[a * no + o];
            if (any <= 0) continue;
            walk(kids_[nd.kids + a], tr, r, v, m);
            for (int h = 0; h < nt; ++h) out[h] += v[h];
        }
        if (m == TRAIN)
            for (int a = 0; a < na; ++a)
                for (int o = 0; o < no; ++o) sum[a * no + o] = sum[a * no + o] * dsum_ + reach[o] * s[a * no + o];
        if (z && m == TRAIN) sumScale_[nd.aux] = encode(sum, len, sum16_.data() + nd.off);
    }

    template <class T>
    static void decode(const T* q, float scale, size_t n, float* x) {
        for (size_t i = 0; i < n; ++i) x[i] = float(q[i]) * scale;
    }
    // Rounds to nearest against the largest magnitude; returns the scale.
    template <class T>
    static float encode(const float* x, size_t n, T* q) {
        constexpr float top = float(std::numeric_limits<T>::max());
        float mx = 0;
        for (size_t i = 0; i < n; ++i) mx = std::max(mx, std::abs(x[i]));
        if (mx == 0) return std::fill(q, q + n, T(0)), 0.0f;
        float inv = top / mx;
        for (size_t i = 0; i < n; ++i) q[i] = T(std::rint(x[i] * inv));
        return mx / top;
    }

    double norm() const {  // total weight of compatible (OOP, IP) pairs
        Vec compat(nh(0));
        compatSums(0, w_[1].data(), compat.data());
        double z = 0;
        for (int h = 0; h < nh(0); ++h) z += double(w_[0][h]) * compat[h];
        return z;
    }

    double exploitability() {
        double v = 0, z = norm();
        if (z <= 0 || pot_ <= 0) return 0;
        for (int tr = 0; tr < 2; ++tr) {
            Vec out(nh(tr));
            walk(root_, tr, w_[1 - tr].data(), out.data(), BEST);
            for (int h = 0; h < nh(tr); ++h) v += double(w_[tr][h]) * out[h];
        }
        return (v / z - pot_) / 2 / pot_;
    }

    // Showdown equity of p's hands against the opponent's preflop range, over every runout.
    Vec equity(int p) {
        Vec num(nh(p)), compat(nh(p));
        // Per-river sweeps even where the tree uses the pair table, so tests can check one against the other.
        const Runout& ro = runout_[runoutFor(board_)];
        for (int id : ro.rivers) {
            sweep(river_[id], p, w_[1 - p].data(), 0.5f, 0.5f, num.data());  // (diff + compat) / 2
            sweep(river_[id], p, w_[1 - p].data(), 0, 1, compat.data());
        }
        for (int h = 0; h < nh(p); ++h) num[h] = compat[h] > 0 ? num[h] / compat[h] : 0;
        return num;
    }
};

std::vector<int> boardOf(const Spot& spot) {
    std::vector<int> board(spot.flop.begin(), spot.flop.end());
    if (spot.turn >= 0) board.push_back(spot.turn);
    if (spot.river >= 0) board.push_back(spot.river);
    return board;
}

}  // namespace

Result solve(const Spot& spot, const Options& opt, Stats* stats) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    std::vector<int> board = boardOf(spot);
    Solver s(spot, board, opt);
    auto t1 = Clock::now();
    Result r = s.run();
    if (stats) {
        s.stats(*stats);
        stats->buildSec = std::chrono::duration<double>(t1 - t0).count();
        stats->solveSec = std::chrono::duration<double>(Clock::now() - t1).count();
    }
    return r;
}

Result solve(const Spot& spot, const Options& opt) { return solve(spot, opt, nullptr); }

Stats measure(const Spot& spot, const Options& opt) {
    Stats st;
    Solver(spot, boardOf(spot), opt).stats(st);
    return st;
}

}  // namespace pf::post
