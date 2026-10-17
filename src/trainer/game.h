#pragma once
// Headless preflop rules engine (docs/game-rules.md). No raylib dependency.
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace pf {

// Positions in preflop action order. Index == position everywhere in this API.
enum Pos { UTG, HJ, CO, BTN, SB, BB, NPOS };

enum class Act : uint8_t { Fold, Call, Raise };

struct Card {
    uint8_t rank;  // 0..12 = 2..A
    uint8_t suit;  // 0..3 = clubs, diamonds, hearts, spades
};

// game-rules.md v1 defaults. All amounts in bb.
struct Config {
    double sb = 0.5, bb = 1.0, stack = 100.0;
    double open = 2.5, sbOpen = 3.0;
    double threeBetIP = 3.5, threeBetOOP = 4.5;
    double fourBetIP = 2.3, fourBetOOP = 2.75;
    double tick = 0.1;  // raise sizes round to this (0.1bb = 1 cent at 10NL); <= 0 disables
    // 1 = a player who cold-called the open may call a squeeze (one 3-bet); 0 = fold or 4-bet only.
    // A double so config files and the bytewise config checks treat it like every other field.
    double squeezeCall = 1;
    // Rake min(rake * pot, rakeCap), taken only when a flop is dealt or an all-in is called
    // (no flop, no drop). rake = 0 turns it off. It doesn't change the betting tree.
    double rake = 0.05, rakeCap = 4.0;
};

// Reads key=value lines (keys = Config field names, '#' starts a comment) over the defaults.
// Throws std::runtime_error on a missing file, unknown key or bad value.
Config loadConfig(const std::string& path);

class Hand {
public:
    // Shuffles, deals 6 hands, posts blinds. Action starts at UTG.
    explicit Hand(std::mt19937& rng, const Config& cfg = {});

    std::array<std::array<Card, 2>, NPOS> cards;

    bool done() const;              // preflop action closed (all folded or action closed)
    int toAct() const;              // position to act, -1 if done()
    std::vector<Act> legal() const; // legal actions for toAct()
    void apply(Act a);              // precondition: a in legal()

    double raiseTo() const;         // total commitment if toAct() raises (== stack when it is an all-in)
    double currentBet() const;      // highest total commitment on the table; call puts toAct() at this
    double committed(int pos) const;// chips this position has put in this hand
    double stack(int pos) const;    // chips behind
    double pot() const;             // sum of committed()
    bool folded(int pos) const;
    const Config& config() const { return cfg_; }
    int lastRaiser() const { return lastRaiser_; }  // seat that made the last raise, -1 if none

private:
    void advance();
    double snap(double x) const;  // round to the nearest cfg_.tick

    Config cfg_;
    std::array<double, NPOS> committed_{};
    std::array<bool, NPOS> folded_{}, acted_{}, raised_{}, called_{};  // called_: called the open
    int toAct_ = UTG;
    int raises_ = 0;      // 0 = blinds only, 1 = open, 2 = 3-bet, 3 = 4-bet, 4 = 5-bet
    int lastRaiser_ = -1;
    double openSize_ = 0; // total of the open, base for 3-bet sizing
    int openCallers_ = 0; // players who called the open before any 3-bet
};

}  // namespace pf
