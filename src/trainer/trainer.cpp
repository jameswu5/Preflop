#include "trainer/trainer.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "solver/cards.h"

namespace pf {

const char* spotName(SpotType t) {
    static const char* names[] = {"Open", "vs open", "Squeeze", "vs 3-bet", "vs 4-bet+", "vs squeeze", "Any"};
    return names[int(t)];
}

SpotType classify(const Strategy& s, const std::vector<Step>& path, int user) {
    int raises = 0, callers = 0;
    bool userCalled = false;  // the user cold-called the open
    for (const Step& st : path) {
        if (st.act == Act::Raise) ++raises;
        else if (st.act == Act::Call && raises == 1) ++callers, userCalled |= s.actor(st.node) == user;
    }
    if (raises == 0) return SpotType::Open;
    if (raises == 1) return callers ? SpotType::Squeeze : SpotType::ThreeBet;
    if (raises == 2) return userCalled ? SpotType::VsSqueeze : SpotType::FourBet;
    return SpotType::FiveBet;
}

static const std::array<float, 3>& handProbs(const Strategy& s, int node, const Holes& cards) {
    const auto& c = cards[s.actor(node)];
    return s.probs(node, bucket(c[0], c[1]));
}

// Samples the actor's action at `node`, applies it and moves `node` to the child.
static Act step(const Strategy& s, Hand& h, int& node, std::mt19937& rng) {
    const auto& p = handProbs(s, node, h.cards);
    float u = std::uniform_real_distribution<float>(0, 1)(rng);
    int a = -1;
    for (int i = 0; i < 3; ++i) {
        if (p[i] <= 0) continue;
        a = i;  // if rounding leaves u >= 0 after the loop, the last action with p > 0 is taken
        if ((u -= p[i]) < 0) break;
    }
    h.apply(Act(a));
    node = s.child(node, Act(a));
    return Act(a);
}

std::optional<Spot> deal(const Strategy& s, const SpotFilter& f, std::mt19937& rng, int tries) {
    std::uniform_int_distribution<int> seat(0, NPOS - 1);
    for (int t = 0; t < tries; ++t) {
        int user = f.pos >= 0 ? f.pos : seat(rng);
        Spot sp{Hand(rng, s.config()), user, s.root(), {}};
        while (!sp.hand.done() && !sp.hand.folded(user)) {
            if (sp.hand.toAct() == user) {
                sp.type = classify(s, sp.path, user);
                if (f.type == SpotType::NSPOT || f.type == sp.type) return sp;
            }
            int node = sp.node;
            sp.path.push_back({node, step(s, sp.hand, sp.node, rng)});
        }
    }
    return std::nullopt;
}

double historyWeight(const Strategy& s, const Spot& sp, const Holes& cards) {
    double w = 1;
    for (const Step& st : sp.path)
        if (s.actor(st.node) != sp.user && (w *= handProbs(s, st.node, cards)[int(st.act)]) == 0) break;
    return w;
}

int playOn(const Strategy& s, Hand& h, int node, int user, Act a, std::mt19937& rng) {
    h.apply(a);
    int edge = node * 3 + int(a);
    node = s.child(node, a);
    while (!h.done() && !h.folded(user)) {
        int n = node;
        edge = n * 3 + int(step(s, h, node, rng));
    }
    return edge;
}

// User's payoff after taking `a` at the spot, everyone (user included) then following the strategy.
static double playout(const Strategy& s, const Equity& e, Hand h, int node, int user, Act a,
                      std::mt19937& rng, const RolloutConfig& rc) {
    playOn(s, h, node, user, a, rng);
    if (h.folded(user)) return -h.committed(user);
    return utility(h, e, rng, rc.flopBoards, rc.rc)[user];
}

std::array<double, 3> rollout(const Strategy& s, const Equity& e, const Spot& sp, std::mt19937& rng,
                              const RolloutConfig& rc, RolloutStats* stats) {
    const auto& mine = sp.hand.cards[sp.user];
    int deck[52], nd = 0;
    for (int i = 0; i < 52; ++i)
        if (i != cardIndex(mine[0]) && i != cardIndex(mine[1])) deck[nd++] = i;
    auto legal = sp.hand.legal();

    double wsum = 0, w2sum = 0;
    std::array<double, 3> sum{}, sum2{};  // sum w x, sum w^2 x
    std::array<std::array<double, 3>, 3> cross{};  // sum w^2 x_a x_b
    Hand h = sp.hand;
    long d = 0;
    for (; d < rc.maxDeals && (d < rc.deals || wsum * wsum < rc.minEss * w2sum || wsum == 0); ++d) {
        for (int j = 0, p = 0; p < NPOS; ++p) {  // partial Fisher-Yates over the 50 cards
            if (p == sp.user) continue;
            for (int c = 0; c < 2; ++c, ++j) {
                std::swap(deck[j], deck[j + std::uniform_int_distribution<int>(0, nd - 1 - j)(rng)]);
                h.cards[p][c] = cardFromIndex(deck[j]);
            }
        }
        double w = historyWeight(s, sp, h.cards);
        if (w == 0) continue;
        wsum += w, w2sum += w * w;
        std::array<double, 3> x{};
        for (Act a : legal) x[int(a)] = playout(s, e, h, sp.node, sp.user, a, rng, rc);
        for (int a = 0; a < 3; ++a) {
            sum[a] += w * x[a], sum2[a] += w * w * x[a];
            for (int b = 0; b < 3; ++b) cross[a][b] += w * w * x[a] * x[b];
        }
    }
    std::array<double, 3> ev;
    ev.fill(std::numeric_limits<double>::quiet_NaN());
    if (wsum > 0)
        for (Act a : legal) ev[int(a)] = sum[int(a)] / wsum;
    if (stats && wsum > 0) {
        std::array<double, 3> m{};
        for (Act a : legal) m[int(a)] = ev[int(a)];
        auto cov = [&](int a, int b) {  // sum w^2 (x_a - m_a)(x_b - m_b) / (sum w)^2
            return (cross[a][b] - m[a] * sum2[b] - m[b] * sum2[a] + m[a] * m[b] * w2sum) / (wsum * wsum);
        };
        for (int a = 0; a < 3; ++a) {
            stats->se[a] = std::sqrt(std::max(cov(a, a), 0.0));
            for (int b = 0; b < 3; ++b) stats->diffSe[a][b] = std::sqrt(std::max(cov(a, a) + cov(b, b) - 2 * cov(a, b), 0.0));
        }
        stats->ess = wsum * wsum / w2sum;
    }
    if (stats) stats->deals = d;
    return ev;
}

Feedback grade(const Strategy& s, const Spot& sp, const std::array<double, 3>& ev, Act choice,
               const GradeConfig& g, const RolloutStats* stats) {
    double best = -std::numeric_limits<double>::infinity();
    int bestAct = int(choice);
    for (Act a : sp.hand.legal())
        if (ev[int(a)] > best) best = ev[int(a)], bestAct = int(a);
    Feedback f{ev, handProbs(s, sp.node, sp.hand.cards), choice, best - ev[int(choice)], 0, false};
    if (stats) f.evLossSe = stats->diffSe[bestAct][int(choice)];
    f.correct = f.evLoss <= g.maxLoss || (f.mix[int(choice)] >= g.minFreq && f.evLoss <= g.mixedLossCap) ||
                (stats && f.evLoss <= g.noiseSe * f.evLossSe);
    return f;
}

void Stats::add(const Spot& sp, const Feedback& f) {
    Cell& c = cell[int(sp.type)][sp.user];
    ++c.n;
    c.correct += f.correct;
    c.evLoss += f.evLoss;
}

std::vector<std::pair<SpotType, int>> Stats::leaks() const {
    std::vector<std::pair<SpotType, int>> out;
    for (int t = 0; t < NSPOT; ++t)
        for (int p = 0; p < NPOS; ++p)
            if (cell[t][p].n) out.push_back({SpotType(t), p});
    std::stable_sort(out.begin(), out.end(), [&](auto& a, auto& b) {
        return cell[int(a.first)][a.second].evLoss > cell[int(b.first)][b.second].evLoss;
    });
    return out;
}

}  // namespace pf
