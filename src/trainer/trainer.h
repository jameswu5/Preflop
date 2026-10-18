#pragma once
// Headless trainer core (docs/trainer.md "Core"): spot dealing, bots, rollout EVs, grading, stats.
//
// Driving it (all functions are const over Strategy/Equity; pass each thread its own rng):
//   Strategy s(cfg);  s.load(path);                // uniform until a solve exists
//   Equity e;         e.loadHuTable(huTablePath);  // optional; heads-up is sampled without it
//   std::mt19937 rng(seed);
//   auto spot = deal(s, {SpotType::ThreeBet, BTN}, rng);  // nullopt if the filter never matched
//   ... show spot->hand (only spot->hand.cards[spot->user] is the user's to see), take choice ...
//   auto ev = rollout(s, e, *spot, rng);           // slow (~20k deals): run on a worker thread
//   Feedback f = grade(s, *spot, ev, choice);
//   stats.add(*spot, f);
#include <array>
#include <optional>
#include <random>
#include <vector>

#include "solver/strategy.h"
#include "solver/utility.h"
#include "trainer/game.h"

namespace pf {

// The user's decision, by what they face. NSPOT doubles as "any" in a filter.
enum class SpotType { Open, ThreeBet, Squeeze, FourBet, FiveBet, VsSqueeze, NSPOT };
// Open: unopened pot. ThreeBet: one open, no callers. Squeeze: an open plus callers.
// FourBet: facing a 3-bet. FiveBet: facing a 4-bet or more (incl. an all-in).
// VsSqueeze: the user cold-called the open and now faces a 3-bet (Config::squeezeCall).
constexpr int NSPOT = int(SpotType::NSPOT);
const char* spotName(SpotType t);

struct Step {
    int node;  // tree node the action was taken at; s.actor(node) is who took it
    Act act;
};

struct Spot {
    Hand hand;               // state at the user's decision (all seats' cards are dealt)
    int user;                // seat; == hand.toAct()
    int node;                // tree node of the decision
    std::vector<Step> path;  // every action so far, bots' and the user's own
    SpotType type = SpotType::NSPOT;
};

// Type of the decision `user` faces after `path`.
SpotType classify(const Strategy& s, const std::vector<Step>& path, int user);

struct SpotFilter {
    SpotType type = SpotType::NSPOT;  // NSPOT = any
    int pos = -1;                     // user's seat, -1 = random
};

// Deals hands until the user reaches a decision matching `f`. Bots (and the user's own earlier
// decisions in the hand) sample from the strategy. nullopt after `tries` hands without a match.
std::optional<Spot> deal(const Strategy& s, const SpotFilter& f, std::mt19937& rng, int tries = 100000);

// Probability that the bots take the spot's path holding `cards`: product of
// probs(node_k, bucket)[act_k] over prior bot actions. The user's own actions are excluded.
double historyWeight(const Strategy& s, const Spot& sp, const Holes& cards);

// Takes `a` at `node` (h's state), then everyone, `user` included, follows the strategy until the hand
// is done or `user` has folded. Returns the last edge played, node * 3 + int(act): at a done() hand
// that is the terminal edge, which fixes Terminal(h).
int playOn(const Strategy& s, Hand& h, int node, int user, Act a, std::mt19937& rng);

struct RolloutConfig {
    // Opponent re-deals (every legal action is played out on each): at least `deals`, then more until
    // the effective sample size (sum w)^2 / sum w^2 reaches `minEss`, up to `maxDeals`. Deals the
    // history rules out (weight 0) cost almost nothing, so rare spots keep dealing.
    int deals = 20000;
    double minEss = 2000;
    int maxDeals = 2000000;
    int flopBoards = 100;  // boards per multiway flop equity (utility())
    RealizationConfig rc;
};

// Precision of a rollout. Standard errors of the self-normalized estimate (delta method); every action
// is played on the same deals, so a difference between two actions is much more precise than either.
struct RolloutStats {
    std::array<double, 3> se{};                   // per action
    std::array<std::array<double, 3>, 3> diffSe{};  // [a][b]: of ev[a] - ev[b]
    double ess = 0;                               // effective sample size
    long deals = 0;                               // re-deals made, weight 0 included
};

// EV in bb per action (indexed by int(Act); NaN if illegal or no deal had weight > 0).
// Opponents are re-dealt from the deck minus the user's cards and weighted by historyWeight.
std::array<double, 3> rollout(const Strategy& s, const Equity& e, const Spot& sp, std::mt19937& rng,
                              const RolloutConfig& rc = {}, RolloutStats* stats = nullptr);

// Correct if evLoss <= maxLoss, or if the solved mix plays the choice at least minFreq of the time
// and evLoss <= mixedLossCap (so rollout noise or model quirks can't pass a big loss as "mixed").
struct GradeConfig {
    double minFreq = 0.1;       // the choice counts as part of the solved mix at this frequency
    double maxLoss = 0.1;       // EV loss in bb that is always fine
    double mixedLossCap = 1.0;  // largest EV loss in bb a mixed choice may still have
    double noiseSe = 2;         // with rollout stats: a loss within this many standard errors is noise
};

struct Feedback {
    std::array<double, 3> ev;   // from rollout()
    std::array<float, 3> mix;   // solved strategy for the user's hand
    Act choice;
    double evLoss;              // best legal EV - ev[choice]
    double evLossSe = 0;        // its standard error, when grade() got rollout stats
    bool correct;
};

Feedback grade(const Strategy& s, const Spot& sp, const std::array<double, 3>& ev, Act choice,
               const GradeConfig& g = {}, const RolloutStats* stats = nullptr);

struct Stats {
    struct Cell {
        int n = 0, correct = 0;
        double evLoss = 0;  // total; average = evLoss / n
    };
    std::array<std::array<Cell, NPOS>, NSPOT> cell{};  // [type][user seat]

    void add(const Spot& sp, const Feedback& f);
    // (type, seat) of every cell played, most total EV lost first.
    std::vector<std::pair<SpotType, int>> leaks() const;
};

}  // namespace pf
