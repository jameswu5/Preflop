#include "trainer/game.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace pf {

Config loadConfig(const std::string& path) {
    Config c;
    const std::pair<const char*, double*> fields[] = {
        {"sb", &c.sb}, {"bb", &c.bb}, {"stack", &c.stack},
        {"open", &c.open}, {"sbOpen", &c.sbOpen},
        {"threeBetIP", &c.threeBetIP}, {"threeBetOOP", &c.threeBetOOP},
        {"fourBetIP", &c.fourBetIP}, {"fourBetOOP", &c.fourBetOOP}, {"tick", &c.tick},
    };
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config " + path);
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r"));
        s.erase(s.find_last_not_of(" \t\r") + 1);
        return s;
    };
    for (std::string line; std::getline(in, line);) {
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) throw std::runtime_error("bad config line: " + line);
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        auto f = std::find_if(std::begin(fields), std::end(fields),
                              [&](auto& kv) { return key == kv.first; });
        if (f == std::end(fields)) throw std::runtime_error("unknown config key: " + key);
        size_t used = 0;
        try { *f->second = std::stod(val, &used); } catch (...) {}
        if (used == 0 || used != val.size()) throw std::runtime_error("bad value for " + key + ": " + val);
    }
    return c;
}

// Postflop order: SB acts first, BTN last.
static int postflopRank(int p) { return (p + 2) % NPOS; }

// x * (1/tick) keeps exact halves exact (8.75 * 10 = 87.5); the epsilon catches products that
// land just under a half (3.5 * 2.3 = 8.0499..). Dividing back gives the double nearest the tick.
double Hand::snap(double x) const {
    if (cfg_.tick <= 0) return x;
    double inv = 1 / cfg_.tick;
    return std::round(x * inv + 1e-6) / inv;
}

Hand::Hand(std::mt19937& rng, const Config& cfg) : cfg_(cfg) {
    std::array<uint8_t, 52> deck;
    std::iota(deck.begin(), deck.end(), 0);
    std::shuffle(deck.begin(), deck.end(), rng);
    for (int p = 0; p < NPOS; ++p)
        for (int c = 0; c < 2; ++c) {
            uint8_t d = deck[p * 2 + c];
            cards[p][c] = {uint8_t(d / 4), uint8_t(d % 4)};
        }
    committed_[SB] = cfg_.sb;
    committed_[BB] = cfg_.bb;
}

bool Hand::done() const { return toAct_ < 0; }
int Hand::toAct() const { return toAct_; }
double Hand::currentBet() const { return *std::max_element(committed_.begin(), committed_.end()); }
double Hand::committed(int pos) const { return committed_[pos]; }
double Hand::stack(int pos) const { return snap(cfg_.stack - committed_[pos]); }
double Hand::pot() const { return snap(std::accumulate(committed_.begin(), committed_.end(), 0.0)); }
bool Hand::folded(int pos) const { return folded_[pos]; }

std::vector<Act> Hand::legal() const {
    if (done()) return {};
    if (currentBet() >= cfg_.stack) return {Act::Fold, Act::Call};
    if (raises_ == 0) return {Act::Fold, Act::Raise};  // no limping
    // Cold calling only vs a single open; after a 3-bet only previous raisers may call.
    if (raises_ == 1 || raised_[toAct_]) return {Act::Fold, Act::Call, Act::Raise};
    return {Act::Fold, Act::Raise};
}

double Hand::raiseTo() const {
    if (done()) return 0;
    // IP/OOP is relative to the player being re-raised.
    bool ip = lastRaiser_ >= 0 && postflopRank(toAct_) > postflopRank(lastRaiser_);
    double to = cfg_.stack;  // 5-bet: all-in
    if (raises_ == 0)
        to = toAct_ == SB ? cfg_.sbOpen : cfg_.open;
    else if (raises_ == 1)
        to = (ip ? cfg_.threeBetIP : cfg_.threeBetOOP) * openSize_ + openCallers_ * openSize_;
    else if (raises_ == 2)
        to = (ip ? cfg_.fourBetIP : cfg_.fourBetOOP) * currentBet();
    return std::min(snap(to), cfg_.stack);
}

void Hand::apply(Act a) {
    auto l = legal();
    assert(std::find(l.begin(), l.end(), a) != l.end());
    int p = toAct_;
    if (a == Act::Fold) {
        folded_[p] = true;
    } else if (a == Act::Call) {
        if (raises_ == 1) ++openCallers_;
        committed_[p] = currentBet();
    } else {
        double to = raiseTo();
        if (raises_ == 0) openSize_ = to;
        committed_[p] = to;
        ++raises_;
        lastRaiser_ = p;
        raised_[p] = true;
        acted_.fill(false);
    }
    acted_[p] = true;
    advance();
}

void Hand::advance() {
    if (std::count(folded_.begin(), folded_.end(), false) <= 1) {
        toAct_ = -1;
        return;
    }
    double bet = currentBet();
    for (int i = 1; i <= NPOS; ++i) {
        int q = (toAct_ + i) % NPOS;
        if (!folded_[q] && (!acted_[q] || committed_[q] < bet) && committed_[q] < cfg_.stack) {
            toAct_ = q;
            return;
        }
    }
    toAct_ = -1;
}

}  // namespace pf
