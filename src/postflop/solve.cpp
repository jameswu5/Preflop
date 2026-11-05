// Vector-form discounted CFR over a fixed heads-up bet tree, flop to river (postflop.h).
//
// Every node works on whole ranges: a traversal for player `tr` carries the opponent's reach per
// hand down the tree and returns tr's counterfactual value per hand. Chance nodes deal one
// representative card per suit-isomorphism orbit and map its values onto the other cards of the
// orbit; showdowns pay out in one sweep over hands sorted by strength. Once both players are all-in
// there is no runout subtree: the showdown node sums the sweep over every runout instead.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
    int aux = 0;           // CHANCE: chance_ index; SHOWDOWN: runout_ index
    size_t off = 0;        // ACTION: offset of [action][hand] in the regret and strategy-sum arenas
};

struct Chance {
    std::vector<int> card;                   // dealt card per child
    std::vector<std::array<int, 3>> mapped;  // {child, hand permutation, card} for the other orbit cards
    float scale = 0;                         // 1 / cards left once both hands are out
    bool parallel = false;
};

struct Runout {  // every completion of a board, by river_ index
    std::vector<int> rivers;
    float scale = 0;  // 1 / completions once both hands are out
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
        for (int p = 0; p < 2; ++p) {
            std::vector<int> at(NCOMBOS, -1);
            for (int h = 0; h < nh(1 - p); ++h) at[combo(1 - p, h)] = h;
            for (int h = 0; h < nh(p); ++h) same_[p].push_back(at[combo(p, h)]);
        }
        buildSymmetry(board, dead);
        root_ = action({int(board.size()) - 3, board, {0, 0}, {0, 0}, 0, 0, group0_});
    }

    Result run() {
        regret_.assign(arena_, 0);
        sum_.assign(arena_, 0);
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
        st.bytes = 2 * arena_ * sizeof(float) + nodes_.size() * sizeof(Node) + kids_.size() * sizeof(int);
        for (const River& rv : river_) st.bytes += (rv.order[0].size() + rv.order[1].size()) * 6;
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
    std::vector<int> board_;
    size_t arena_ = 0;
    Vec regret_, sum_;
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
        std::vector<double> adds;  // chips added by each bet or raise
        if (facing <= EPS)
            for (double f : sz.bet) adds.push_back(f * (pot_ + s.c[0] + s.c[1]));
        else if (s.raises < spot_.tree.maxRaises)
            for (double m : sz.raise) adds.push_back(m * s.sc[q] - s.sc[p]);
        if (facing <= EPS || s.raises < spot_.tree.maxRaises) adds.push_back(rem);
        for (double& a : adds) a = std::min(a, rem);
        std::erase_if(adds, [&](double a) { return a <= facing + EPS; });
        std::sort(adds.begin(), adds.end());
        adds.erase(std::unique(adds.begin(), adds.end(), [](double a, double b) { return b - a < 1e-6; }),
                   adds.end());

        Node nd{ACTION, uint8_t(p)};
        nd.contrib[0] = float(s.c[0]), nd.contrib[1] = float(s.c[1]);
        int n = add(nd), passive = facing > EPS ? 2 : 1;
        int base = reserveKids(n, passive + int(adds.size()));
        nodes_[n].off = arena_;
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
            nd.aux = int(runout_.size());
            runout_.push_back(runouts(s.board));
            return add(nd);
        }
        uint64_t dead = 0;
        for (int c : s.board) dead |= 1ull << c;
        Chance ch;
        ch.scale = 1.0f / float(52 - int(s.board.size()) - 4);
        ch.parallel = s.street == 0;
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
        return ro;
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
    void compatSums(int tr, const float* reach, float* compat) const {
        int op = 1 - tr;
        double total = 0, card[52] = {};
        for (int o = 0; o < nh(op); ++o) {
            total += reach[o];
            card[hands_[op][o][0]] += reach[o], card[hands_[op][o][1]] += reach[o];
        }
        for (int h = 0; h < nh(tr); ++h) {
            int s = same_[tr][h];
            compat[h] = float(total - card[hands_[tr][h][0]] - card[hands_[tr][h][1]] + (s >= 0 ? reach[s] : 0));
        }
    }

    // For tr's hands on this board: diff = reach beaten - reach beating, compat as above (only
    // hands on the board's lists are counted). Hands off the board are left untouched.
    void sweep(const River& rv, int tr, const float* reach, float* diff, float* compat) const {
        int op = 1 - tr;
        const auto &ot = rv.order[tr], &oo = rv.order[op];
        const auto &st = rv.strength[tr], &so = rv.strength[op];
        const auto &ht = hands_[tr], &ho = hands_[op];
        double total = 0, card[52] = {};
        for (int o : oo) total += reach[o], card[ho[o][0]] += reach[o], card[ho[o][1]] += reach[o];
        for (size_t i = 0; i < ot.size(); ++i) {
            int h = ot[i], s = same_[tr][h];
            compat[h] = float(total - card[ht[h][0]] - card[ht[h][1]] + (s >= 0 ? reach[s] : 0));
        }
        double run = 0, rc[52] = {};
        for (size_t i = 0, j = 0; i < ot.size(); ++i) {
            for (; j < oo.size() && so[j] < st[i]; ++j) {
                int o = oo[j];
                run += reach[o], rc[ho[o][0]] += reach[o], rc[ho[o][1]] += reach[o];
            }
            int h = ot[i];
            diff[h] = float(run - rc[ht[h][0]] - rc[ht[h][1]]);
        }
        run = 0, std::fill(rc, rc + 52, 0.0);
        for (size_t i = ot.size(), j = oo.size(); i-- > 0;) {
            for (; j > 0 && so[j - 1] > st[i]; --j) {
                int o = oo[j - 1];
                run += reach[o], rc[ho[o][0]] += reach[o], rc[ho[o][1]] += reach[o];
            }
            int h = ot[i];
            diff[h] -= float(run - rc[ht[h][0]] - rc[ht[h][1]]);
        }
    }

    // sweep() averaged over the runouts (zeroed first).
    void showdown(const Runout& ro, int tr, const float* reach, float* diff, float* compat) const {
        if (ro.rivers.size() == 1) return sweep(river_[ro.rivers[0]], tr, reach, diff, compat);
        int nt = nh(tr);
        Vec d(nt), c(nt);
        for (int id : ro.rivers) {
            std::fill(d.begin(), d.end(), 0.0f), std::fill(c.begin(), c.end(), 0.0f);
            sweep(river_[id], tr, reach, d.data(), c.data());
            for (int h = 0; h < nt; ++h) diff[h] += d[h], compat[h] += c[h];
        }
        for (int h = 0; h < nt; ++h) diff[h] *= ro.scale, compat[h] *= ro.scale;
    }

    static void matching(const float* r, int na, int n, float* s) {  // regret matching
        for (int h = 0; h < n; ++h) {
            float tot = 0;
            for (int a = 0; a < na; ++a) tot += std::max(r[a * n + h], 0.0f);
            for (int a = 0; a < na; ++a) s[a * n + h] = tot > 0 ? std::max(r[a * n + h], 0.0f) / tot : 1.0f / na;
        }
    }
    static void average(const float* sum, int na, int n, float* s) {
        for (int h = 0; h < n; ++h) {
            float tot = 0;
            for (int a = 0; a < na; ++a) tot += sum[a * n + h];
            for (int a = 0; a < na; ++a) s[a * n + h] = tot > 0 ? sum[a * n + h] / tot : 1.0f / na;
        }
    }

    void walk(int n, int tr, const float* reach, float* out, Mode m) {
        const Node& nd = nodes_[n];
        int op = 1 - tr, nt = nh(tr), no = nh(op);
        switch (nd.kind) {
        case FOLD: {
            float pay = nd.player == tr ? -nd.contrib[tr] : float(pot_) + nd.contrib[op];
            compatSums(tr, reach, out);
            for (int h = 0; h < nt; ++h) out[h] *= pay;
            return;
        }
        case SHOWDOWN: {
            Vec diff(nt), compat(nt);
            showdown(runout_[nd.aux], tr, reach, diff.data(), compat.data());
            float a = nd.contrib[0] + float(pot_) / 2, b = float(pot_) / 2;
            for (int h = 0; h < nt; ++h) out[h] = a * diff[h] + b * compat[h];
            return;
        }
        case CHANCE: {
            const Chance& ch = chance_[nd.aux];
            int nk = nd.nKids;
            std::vector<Vec> vals(nk, Vec(nt));
            std::atomic<int> next{0};
            auto work = [&] {
                Vec r(no);
                for (int k; (k = next++) < nk;) {
                    int c = ch.card[k];
                    for (int o = 0; o < no; ++o)
                        r[o] = hands_[op][o][0] == c || hands_[op][o][1] == c ? 0 : reach[o];
                    walk(kids_[nd.kids + k], tr, r.data(), vals[k].data(), m);
                    for (int h = 0; h < nt; ++h)
                        if (hands_[tr][h][0] == c || hands_[tr][h][1] == c) vals[k][h] = 0;
                }
            };
            if (ch.parallel && threads_ > 1) {
                std::vector<std::thread> pool;
                for (int i = 1; i < std::min(threads_, nk); ++i) pool.emplace_back(work);
                work();
                for (auto& th : pool) th.join();
            } else {
                work();
            }
            std::fill(out, out + nt, 0.0f);
            for (int k = 0; k < nk; ++k)
                for (int h = 0; h < nt; ++h) out[h] += vals[k][h];
            for (auto [k, pk, c] : ch.mapped) {
                const int* hp = hperm_[tr][pk].data();
                for (int h = 0; h < nt; ++h) out[h] += vals[k][hp[h]];
            }
            for (int h = 0; h < nt; ++h) out[h] *= ch.scale;
            return;
        }
        case ACTION: break;
        }

        int na = nd.nKids, p = nd.player, np = nh(p);
        float* reg = regret_.data() + nd.off;
        float* sum = sum_.data() + nd.off;
        Vec s(size_t(na) * np);
        if (m == TRAIN) matching(reg, na, np, s.data());
        else if (p != tr || m == EVAL) average(sum, na, np, s.data());

        if (p == tr) {
            Vec v(size_t(na) * nt);
            for (int a = 0; a < na; ++a) walk(kids_[nd.kids + a], tr, reach, &v[size_t(a) * nt], m);
            for (int h = 0; h < nt; ++h) {
                float e = m == BEST ? v[h] : 0;
                for (int a = 0; a < na; ++a)
                    e = m == BEST ? std::max(e, v[a * nt + h]) : e + s[a * nt + h] * v[a * nt + h];
                out[h] = e;
            }
            if (m == TRAIN)
                for (int a = 0; a < na; ++a)
                    for (int h = 0; h < nt; ++h) {
                        float& r = reg[a * nt + h];
                        r = r * (r > 0 ? dpos_ : dneg_) + v[a * nt + h] - out[h];
                    }
            return;
        }

        std::fill(out, out + nt, 0.0f);
        Vec r(no), v(nt);
        for (int a = 0; a < na; ++a) {
            bool any = false;
            for (int o = 0; o < no; ++o) any |= (r[o] = reach[o] * s[a * no + o]) > 0;
            if (!any) continue;
            walk(kids_[nd.kids + a], tr, r.data(), v.data(), m);
            for (int h = 0; h < nt; ++h) out[h] += v[h];
        }
        if (m == TRAIN)
            for (size_t i = 0; i < s.size(); ++i) sum[i] = sum[i] * dsum_ + reach[i % no] * s[i];
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
        Vec diff(nh(p)), compat(nh(p));
        showdown(runouts(board_), p, w_[1 - p].data(), diff.data(), compat.data());
        for (int h = 0; h < nh(p); ++h) diff[h] = compat[h] > 0 ? (diff[h] + compat[h]) / 2 / compat[h] : 0;
        return diff;
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

Stats measure(const Spot& spot) {
    Stats st;
    Solver(spot, boardOf(spot), Options{}).stats(st);
    return st;
}

}  // namespace pf::post
