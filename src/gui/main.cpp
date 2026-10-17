// GGPoker-style 6-max preflop trainer table (docs/trainer.md "GUI"). Bots play the strategy with
// human-like think times; each of the user's decisions is rolled out on a worker thread and graded.
// Usage: preflop [strategy file] [heads-up equity table]  (defaults: data/strategy_6max.bin, data/hu_equity.bin)
#include "raylib.h"
#include "solver/cards.h"
#include "solver/strategy.h"
#include "trainer/game.h"
#include "trainer/trainer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <optional>
#include <string>

namespace {

constexpr int W = 1280, H = 800;
constexpr int HERO = 0;  // physical seat 0 is bottom centre; seats go clockwise
const char* POS_NAME[] = {"UTG", "HJ", "CO", "BTN", "SB", "BB"};
const char* SEAT_NAME[] = {"Hero", "Bot 1", "Bot 2", "Bot 3", "Bot 4", "Bot 5"};

const Vector2 SEAT[] = {{640, 690}, {175, 505}, {235, 180}, {640, 95}, {1045, 180}, {1105, 505}};
const Vector2 BET[] = {{640, 505}, {340, 450}, {370, 275}, {640, 225}, {910, 275}, {940, 450}};
const Vector2 DEALER[] = {{730, 535}, {300, 395}, {330, 225}, {560, 225}, {960, 225}, {980, 395}};
const Color AVATAR[] = {{0, 0, 0, 0}, {94, 129, 172, 255}, {163, 98, 98, 255},
                        {112, 150, 96, 255}, {170, 132, 70, 255}, {128, 104, 168, 255}};

Font font, bold;

std::string fmt(double x) {  // 8.75 -> "8.75", 100 -> "100"
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", x);
    std::string s = b;
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s;
}

void text(const Font& f, const std::string& s, Vector2 c, float size, Color col) {  // centred
    Vector2 m = MeasureTextEx(f, s.c_str(), size, 0);
    DrawTextEx(f, s.c_str(), {std::round(c.x - m.x / 2), std::round(c.y - m.y / 2)}, size, 0, col);
}

void tri(Vector2 a, Vector2 b, Vector2 c, Color col) {  // raylib wants counter-clockwise on screen
    if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0) std::swap(b, c);
    DrawTriangle(a, b, c, col);
}

// Suits drawn as shapes: the default/Arial fonts have no suit glyphs we can rely on.
void suit(int s, Vector2 c, float r, Color col) {
    float x = c.x, y = c.y;
    switch (s) {
    case 0:  // clubs
        DrawCircleV({x, y - r * .45f}, r * .42f, col);
        DrawCircleV({x - r * .45f, y + r * .12f}, r * .42f, col);
        DrawCircleV({x + r * .45f, y + r * .12f}, r * .42f, col);
        tri({x, y}, {x - r * .3f, y + r}, {x + r * .3f, y + r}, col);
        break;
    case 1:  // diamonds
        tri({x, y - r}, {x - r * .72f, y}, {x + r * .72f, y}, col);
        tri({x, y + r}, {x - r * .72f, y}, {x + r * .72f, y}, col);
        break;
    case 2:  // hearts
        DrawCircleV({x - r * .45f, y - r * .3f}, r * .5f, col);
        DrawCircleV({x + r * .45f, y - r * .3f}, r * .5f, col);
        tri({x - r * .93f, y - r * .12f}, {x + r * .93f, y - r * .12f}, {x, y + r}, col);
        break;
    default:  // spades
        DrawCircleV({x - r * .45f, y + r * .2f}, r * .48f, col);
        DrawCircleV({x + r * .45f, y + r * .2f}, r * .48f, col);
        tri({x - r * .9f, y + r * .05f}, {x + r * .9f, y + r * .05f}, {x, y - r}, col);
        tri({x, y + r * .2f}, {x - r * .3f, y + r}, {x + r * .3f, y + r}, col);
    }
}

// GG-style 4-colour deck: clubs green, diamonds blue, hearts red, spades black.
const Color SUIT_COL[] = {{38, 140, 72, 255}, {36, 104, 196, 255}, {204, 46, 52, 255}, {30, 30, 34, 255}};

void card(pf::Card c, Rectangle r) {
    DrawRectangleRounded(r, .12f, 8, RAYWHITE);
    const char* ranks = "23456789TJQKA";
    Color col = SUIT_COL[c.suit];
    text(bold, std::string(1, ranks[c.rank]), {r.x + r.width * .28f, r.y + r.height * .22f}, r.height * .34f, col);
    suit(c.suit, {r.x + r.width * .28f, r.y + r.height * .5f}, r.height * .1f, col);
    suit(c.suit, {r.x + r.width * .62f, r.y + r.height * .72f}, r.height * .2f, col);
}

void cardBack(Rectangle r, unsigned char a) {
    DrawRectangleRounded(r, .15f, 6, {170, 30, 40, a});
    DrawRectangleRoundedLinesEx({r.x + 3, r.y + 3, r.width - 6, r.height - 6}, .15f, 6, 1.5f, {240, 200, 200, (unsigned char)(a / 2)});
}

void chips(Vector2 p, double amt) {
    DrawCircleV(p, 10, {214, 158, 46, 255});
    DrawCircleV(p, 6.5f, {245, 205, 110, 255});
    std::string s = fmt(amt);
    Vector2 m = MeasureTextEx(bold, s.c_str(), 17, 0);
    DrawRectangleRounded({p.x + 14, p.y - 11, m.x + 14, 22}, 1, 8, {0, 0, 0, 120});
    DrawTextEx(bold, s.c_str(), {p.x + 21, std::round(p.y - m.y / 2)}, 17, 0, WHITE);
}

int seatAt(Vector2 m) {  // seat under the mouse, -1 if none
    for (int s = 0; s < 6; s++)
        if (CheckCollisionPointRec(m, {SEAT[s].x - 78, SEAT[s].y - 60, 156, 120})) return s;
    return -1;
}

int posOf(int seat, int btn) { return (seat - btn + pf::BTN + pf::NPOS) % pf::NPOS; }
int seatOf(int pos, int btn) { return (pos - pf::BTN + btn + pf::NPOS) % pf::NPOS; }


// One decision taken this hand: the strategy node it was made at, for the range panel.
struct Decision {
    int node, pos;
    pf::Act act;
    std::string did;   // "Raise 2.5", "Call 2.5", "Fold", "All-in 100"
    std::string spot;  // non-fold actions before it, e.g. "UTG Raise 2.5, HJ Call 2.5"
    // The user's own choices only:
    std::optional<pf::Spot> sp;              // state before acting, for grading
    std::future<std::array<double, 3>> ev;   // rollout in flight
    std::optional<pf::Feedback> fb;
};

// Grid/legend colours per pf::Act (Fold, Call, Raise).
const Color ACT_COL[] = {{58, 98, 150, 255}, {46, 140, 92, 255}, {196, 82, 40, 255}};
const char* ACT_NAME[] = {"Fold", "Call", "Raise"};
const Color GREY = {130, 134, 142, 255};
constexpr Rectangle PANEL = {190, 60, 900, 680};
constexpr Rectangle CLOSE_BTN = {PANEL.x + PANEL.width - 224, PANEL.y + PANEL.height - 72, 200, 48};
constexpr float CELL = 42, GX = PANEL.x + 24, GY = PANEL.y + 88;
constexpr Rectangle MENU = {290, 170, 700, 420};

int combos(int b) { return b / 13 == b % 13 ? 6 : b / 13 < b % 13 ? 4 : 12; }

// Immediate-mode button: draws, returns true on click.
bool button(Rectangle r, const std::string& label, bool on = false, float size = 18) {
    bool hov = CheckCollisionPointRec(GetMousePosition(), r);
    Color c = on ? Color{214, 158, 46, 255} : hov ? Color{70, 74, 84, 255} : Color{52, 56, 64, 255};
    DrawRectangleRounded(r, .3f, 8, c);
    text(bold, label, {r.x + r.width / 2, r.y + r.height / 2}, size, on ? Color{20, 20, 24, 255} : WHITE);
    return hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void panelBg(Rectangle r) {
    DrawRectangle(0, 0, W, H, {0, 0, 0, 140});
    DrawRectangleRounded(r, .03f, 12, {24, 26, 31, 252});
}

void actRows(const std::array<float, 3>& p, float x, float y) {  // "Raise 33.3%" lines, aggressive first
    for (int a = 2; a >= 0; a--, y += 24) {
        DrawRectangleRounded({x, y + 3, 14, 14}, .3f, 4, ACT_COL[a]);
        DrawTextEx(font, ACT_NAME[a], {x + 22, y}, 18, 0, {200, 204, 210, 255});
        char b[16];
        std::snprintf(b, sizeof b, "%.1f%%", p[a] * 100);
        DrawTextEx(bold, b, {x + 110, y}, 18, 0, WHITE);
    }
}

// EV per action, the user's EV loss and verdict.
void feedback(const Decision& d, float x, float y) {
    DrawTextEx(font, "EV (bb)", {x, y}, 15, 0, GREY);
    if (!d.fb) {
        DrawTextEx(font, "Computing...", {x, y + 20}, 18, 0, {200, 204, 210, 255});
        return;
    }
    const pf::Feedback& f = *d.fb;
    y += 20;
    for (int a = 2; a >= 0; a--) {
        if (std::isnan(f.ev[a])) continue;
        DrawRectangleRounded({x, y + 3, 14, 14}, .3f, 4, ACT_COL[a]);
        DrawTextEx(font, ACT_NAME[a], {x + 22, y}, 18, 0, {200, 204, 210, 255});
        char b[32];
        std::snprintf(b, sizeof b, "%+.2f%s", f.ev[a], a == int(f.choice) ? "  < you" : "");
        DrawTextEx(bold, b, {x + 110, y}, 18, 0, WHITE);
        y += 24;
    }
    char b[48];
    if (std::isnan(f.evLoss)) std::snprintf(b, sizeof b, "No EV: path never reached");
    else std::snprintf(b, sizeof b, "%s   EV loss %.2f bb", f.correct ? "Correct" : "Mistake", f.evLoss);
    DrawTextEx(bold, b, {x, y + 6}, 20, 0, std::isnan(f.evLoss) ? GREY : f.correct ? Color{80, 200, 120, 255} : Color{230, 80, 70, 255});
}

// 13x13 strategy grid for one decision. reach = how often each class gets here (dimmed if never);
// highlight = the user's bucket, -1 for bots (cards hidden).
void rangePanel(const pf::Strategy& st, const Decision& d, const std::array<float, 169>& reach, const std::string& title,
                int highlight, bool nextHand) {
    panelBg(PANEL);
    DrawTextEx(bold, title.c_str(), {GX, PANEL.y + 20}, 26, 0, WHITE);
    DrawTextEx(font, (d.spot.empty() ? "Unopened pot" : d.spot).c_str(), {GX, PANEL.y + 54}, 17, 0, {150, 154, 162, 255});

    Vector2 m = GetMousePosition();
    int hover = -1;
    for (int b = 0; b < pf::NBUCKETS; b++) {
        Rectangle c = {GX + b % 13 * CELL, GY + b / 13 * CELL, CELL - 2, CELL - 2};
        if (CheckCollisionPointRec(m, c)) hover = b;
        bool reached = reach[b] >= 1e-3f;
        auto& p = st.probs(d.node, b);
        float x = c.x;
        if (!reached) DrawRectangleRec(c, {38, 41, 48, 255});
        else
            for (int a = 2; a >= 0; a--) {  // raise | call | fold, left to right
                DrawRectangleRec({x, c.y, c.width * p[a], c.height}, ACT_COL[a]);
                x += c.width * p[a];
            }
        std::string n = pf::bucketName(b);
        if (reached) text(bold, n, {c.x + c.width / 2 + 1, c.y + c.height / 2 + 1}, 14, {0, 0, 0, 120});
        text(bold, n, {c.x + c.width / 2, c.y + c.height / 2}, 14, reached ? WHITE : Color{90, 94, 102, 255});
    }
    auto outline = [](int b, Color col) {
        DrawRectangleLinesEx({GX + b % 13 * CELL - 2, GY + b / 13 * CELL - 2, CELL + 2, CELL + 2}, 3, col);
    };
    if (highlight >= 0) outline(highlight, {240, 196, 64, 255});
    if (hover >= 0) outline(hover, WHITE);

    // right column: action taken, hovered (or own) hand, range widths by reached combos, EV
    float x = GX + 13 * CELL + 28, y = GY;
    DrawTextEx(font, "Action", {x, y}, 15, 0, GREY);
    DrawTextEx(bold, d.did.c_str(), {x, y + 18}, 22, 0, WHITE);
    y += 66;
    int shown = hover >= 0 ? hover : highlight;
    if (shown >= 0) {
        DrawTextEx(font, hover >= 0 ? "Hand" : "Your hand", {x, y}, 15, 0, GREY);
        DrawTextEx(bold, pf::bucketName(shown).c_str(), {x, y + 18}, 26, 0, WHITE);
        actRows(st.probs(d.node, shown), x, y + 54);
    } else {
        DrawTextEx(font, "Hover a hand for its mix", {x, y + 18}, 16, 0, GREY);
    }
    y += 150;
    std::array<float, 3> width{};
    float total = 0;
    for (int b = 0; b < pf::NBUCKETS; b++) {
        float w = combos(b) * reach[b];
        total += w;
        for (int a = 0; a < 3; a++) width[a] += w * st.probs(d.node, b)[a];
    }
    for (auto& w : width) w = total > 0 ? w / total : 0;
    char hdr[48];
    std::snprintf(hdr, sizeof hdr, "Whole range  (%.1f%% of hands)", 100 * total / pf::NCOMBOS);
    DrawTextEx(font, hdr, {x, y}, 15, 0, GREY);
    float bx = x;
    for (int a = 2; a >= 0; a--) {
        DrawRectangleRec({bx, y + 24, 260 * width[a], 12}, ACT_COL[a]);
        bx += 260 * width[a];
    }
    actRows(width, x, y + 48);
    if (d.sp) feedback(d, x, y + 136);

    DrawTextEx(font, "<- -> other decisions this hand", {GX, PANEL.y + PANEL.height - 30}, 14, 0, GREY);
    button(CLOSE_BTN, nextHand ? "Next hand  (Space)" : "Close  (Esc)");  // click handled in update
}

const char* POS_PICK[] = {"Random", "UTG", "HJ", "CO", "BTN", "SB", "BB"};

// Spot picker: which decision to drill. Applies from the next hand.
bool pickerPanel(pf::SpotFilter& f) {
    panelBg(MENU);
    DrawTextEx(bold, "Drill spots", {MENU.x + 28, MENU.y + 24}, 26, 0, WHITE);
    DrawTextEx(font, "Applies from the next hand", {MENU.x + 28, MENU.y + 58}, 16, 0, GREY);
    DrawTextEx(font, "Spot", {MENU.x + 28, MENU.y + 100}, 15, 0, GREY);
    for (int t = 0; t <= pf::NSPOT; t++) {  // NSPOT ("Any") first
        int i = (t + pf::NSPOT) % (pf::NSPOT + 1);
        if (button({MENU.x + 28 + t * 108.f, MENU.y + 124, 100, 40}, pf::spotName(pf::SpotType(i)), int(f.type) == i, 16))
            f.type = pf::SpotType(i);
    }
    DrawTextEx(font, "Your position", {MENU.x + 28, MENU.y + 196}, 15, 0, GREY);
    for (int p = -1; p < pf::NPOS; p++)
        if (button({MENU.x + 28 + (p + 1) * 92.f, MENU.y + 220, 84, 40}, POS_PICK[p + 1], f.pos == p, 16)) f.pos = p;
    DrawTextEx(font, "Random position with any spot rotates the button as usual.", {MENU.x + 28, MENU.y + 280}, 15, 0, GREY);
    return button({MENU.x + MENU.width - 228, MENU.y + MENU.height - 72, 200, 48}, "Done  (Esc)");
}

// Stats per (spot, position), biggest total EV loss first.
bool statsPanel(const pf::Stats& st) {
    panelBg(MENU);
    int n = 0, correct = 0;
    double loss = 0;
    for (auto& row : st.cell)
        for (auto& c : row) n += c.n, correct += c.correct, loss += c.evLoss;
    char b[96];
    std::snprintf(b, sizeof b, "%d decisions   %.0f%% correct   avg EV loss %.2f bb", n, n ? 100.0 * correct / n : 0.0,
                  n ? loss / n : 0.0);
    DrawTextEx(bold, "Stats", {MENU.x + 28, MENU.y + 24}, 26, 0, WHITE);
    DrawTextEx(font, b, {MENU.x + 28, MENU.y + 58}, 16, 0, GREY);
    const float cols[] = {0, 130, 200, 280, 370, 480};
    const char* head[] = {"Spot", "Pos", "Hands", "Correct", "Avg loss", "Total loss"};
    for (int i = 0; i < 6; i++) DrawTextEx(font, head[i], {MENU.x + 28 + cols[i], MENU.y + 96}, 15, 0, GREY);
    float y = MENU.y + 122;
    auto leaks = st.leaks();
    for (size_t r = 0; r < leaks.size() && r < 8; r++, y += 26) {
        auto [t, p] = leaks[r];
        const auto& c = st.cell[int(t)][p];
        char v[4][24];
        std::snprintf(v[0], 24, "%d", c.n);
        std::snprintf(v[1], 24, "%.0f%%", 100.0 * c.correct / c.n);
        std::snprintf(v[2], 24, "%.2f", c.evLoss / c.n);
        std::snprintf(v[3], 24, "%.2f", c.evLoss);
        const char* cells[] = {pf::spotName(t), POS_NAME[p], v[0], v[1], v[2], v[3]};
        for (int i = 0; i < 6; i++) DrawTextEx(i ? font : bold, cells[i], {MENU.x + 28 + cols[i], y}, 17, 0, WHITE);
    }
    if (leaks.empty()) DrawTextEx(font, "No graded decisions yet", {MENU.x + 28, y}, 17, 0, GREY);
    return button({MENU.x + MENU.width - 228, MENU.y + MENU.height - 72, 200, 48}, "Close  (Esc)");
}

struct Button {
    Rectangle r;
    pf::Act act;
    const char* title;
    std::string amount;
    Color col;
    KeyboardKey key;
};

}  // namespace

int main(int argc, char** argv) {
    pf::Strategy strat;  // uniform dummy if no solved file loads
    std::string sf = argc > 1 ? argv[1] : std::string(GetApplicationDirectory()) + "../data/strategy_6max.bin";
    if (!strat.load(sf)) std::fprintf(stderr, "could not load %s, using uniform strategy\n", sf.c_str());
    pf::Equity eq;
    std::string hu = argc > 2 ? argv[2] : std::string(GetApplicationDirectory()) + "../data/hu_equity.bin";
    if (!eq.loadHuTable(hu)) std::fprintf(stderr, "no heads-up equity table at %s, sampling instead\n", hu.c_str());
    pf::RolloutConfig rc;
    rc.rc = strat.realization();

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    InitWindow(W, H, "Preflop");
    SetExitKey(KEY_NULL);  // Esc closes panels instead
    SetTargetFPS(60);

    auto loadFont = [](const char* path) {
        if (!FileExists(path)) return GetFontDefault();
        Font f = LoadFontEx(path, 64, nullptr, 0);
        SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
        return f;
    };
    font = loadFont("/System/Library/Fonts/Supplemental/Arial.ttf");
    bold = loadFont("/System/Library/Fonts/Supplemental/Arial Bold.ttf");

    std::mt19937 rng{std::random_device{}()};
    auto uni = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };

    int btn = 0;
    std::optional<pf::Hand> hand;
    std::string label[pf::NPOS];  // last action per position
    double clock = 0, thinkLen = 0;
    int node = 0;                 // strategy node of the current state, -1 once off the tree
    std::vector<Decision> log;    // this hand's decisions
    std::vector<pf::Act> replay;  // deal()'s path up to the user's spot, played back with think times
    size_t replayAt = 0;
    int view = -1;                // index into log shown in the range panel, -1 = closed
    int menu = 0;                 // 1 = spot picker, 2 = stats
    bool reviewed = false;        // the user's end-of-hand feedback already shown
    pf::SpotFilter filter;
    pf::Stats stats;
    std::string notice;

    auto lastBy = [&](int pos) {
        for (int i = int(log.size()) - 1; i >= 0; i--)
            if (log[i].pos == pos) return i;
        return -1;
    };
    // Grades a finished rollout into stats; wait = block until it's done.
    auto collect = [&](Decision& d, bool wait) {
        using namespace std::chrono_literals;
        if (!d.ev.valid() || (!wait && d.ev.wait_for(0s) != std::future_status::ready)) return;
        d.fb = pf::grade(strat, *d.sp, d.ev.get(), d.act);
        if (!std::isnan(d.fb->evLoss)) stats.add(*d.sp, *d.fb);
    };

    auto startTurn = [&] {
        clock = 0;
        // ponytail: flat think-time distribution with 8% tanks; per-spot timing if it ever matters
        thinkLen = uni(0, 1) < .08 ? uni(2.5, 4) : uni(.3, 1.5);
    };
    auto newHand = [&] {
        for (auto& d : log) collect(d, true);
        log.clear();
        pf::SpotFilter f = filter;
        if (f.type == pf::SpotType::NSPOT && f.pos < 0) f.pos = posOf(HERO, (btn + 1) % pf::NPOS);  // normal rotation
        auto sp = pf::deal(strat, f, rng);
        notice = sp ? "" : "No such spot found, dealing any spot";
        if (!sp) sp = pf::deal(strat, {}, rng);
        btn = (pf::BTN - sp->user + pf::NPOS) % pf::NPOS;  // puts the user's position in the hero seat
        hand.emplace(rng, strat.config());
        hand->cards = sp->hand.cards;
        replay.clear();
        for (auto& st : sp->path) replay.push_back(st.act);
        replayAt = 0;
        for (auto& l : label) l.clear();
        node = strat.root();
        view = -1;
        reviewed = false;
        startTurn();
    };
    auto act = [&](pf::Act a, bool graded) {
        int p = hand->toAct();
        bool allIn = a == pf::Act::Raise && hand->raiseTo() >= hand->committed(p) + hand->stack(p) - 1e-9;
        label[p] = a == pf::Act::Fold ? "Fold" : a == pf::Act::Call ? "Call" : allIn ? "All-in" : "Raise";
        Decision d{node, p, a, label[p], {}, {}, {}, {}};
        std::vector<pf::Step> path;
        for (auto& e : log) {
            path.push_back({e.node, e.act});
            if (e.act != pf::Act::Fold) d.spot += (d.spot.empty() ? "" : ", ") + std::string(POS_NAME[e.pos]) + " " + e.did;
        }
        if (a != pf::Act::Fold)
            d.did += " " + fmt(a == pf::Act::Call ? std::min(hand->currentBet(), hand->committed(p) + hand->stack(p)) : hand->raiseTo());
        if (graded) {
            d.sp = pf::Spot{*hand, p, node, path, pf::classify(path)};
            d.ev = std::async(std::launch::async, [&strat, &eq, rc, sp = *d.sp, seed = rng()] {
                std::mt19937 r(seed);  // each worker owns its rng
                return pf::rollout(strat, eq, sp, r, rc);
            });
        }
        log.push_back(std::move(d));
        node = strat.child(node, a);
        hand->apply(a);
        startTurn();
    };
    auto botAct = [&] {
        if (replayAt < replay.size()) return act(replay[replayAt++], false);
        const auto& c = hand->cards[hand->toAct()];
        const auto& p = strat.probs(node, pf::bucket(c[0], c[1]));
        act(pf::Act(std::discrete_distribution<int>(p.begin(), p.end())(rng)), false);
    };

    newHand();

    while (!WindowShouldClose()) {
        int hero = posOf(HERO, btn);
        int turn = hand->toAct();
        Vector2 mouse = GetMousePosition();
        bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        bool close = IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_SPACE);
        for (auto& d : log) collect(d, false);

        // --- update ---
        std::vector<Button> buttons;
        if (menu) {  // picker/stats open: game paused, their buttons are handled while drawing
            if (close || (click && !CheckCollisionPointRec(mouse, MENU))) menu = 0;
        } else if (view >= 0) {  // range panel open: game paused
            if (IsKeyPressed(KEY_LEFT) && view > 0) view--;
            if (IsKeyPressed(KEY_RIGHT) && view + 1 < int(log.size())) view++;
            if (close || (click && (CheckCollisionPointRec(mouse, CLOSE_BTN) || !CheckCollisionPointRec(mouse, PANEL)))) {
                view = -1;
                if (hand->done()) newHand();
            }
            turn = hand->toAct();
        } else if (IsKeyPressed(KEY_P)) {
            menu = 1;
        } else if (IsKeyPressed(KEY_S)) {
            menu = 2;
        } else if (int s = click ? seatAt(mouse) : -1; s >= 0 && lastBy(posOf(s, btn)) >= 0) {
            view = lastBy(posOf(s, btn));
        } else if (IsKeyPressed(KEY_H) && lastBy(hero) >= 0) {
            view = lastBy(hero);
        } else if (clock += GetFrameTime(); hand->done()) {
            if (!reviewed && lastBy(hero) >= 0 && clock > 1.0) {  // the user's feedback before the next hand
                view = lastBy(hero);
                reviewed = true;
            } else if (lastBy(hero) < 0 && clock > 2.0) {
                newHand();
            }
        } else if (turn != hero || replayAt < replay.size()) {
            if (clock >= (hand->folded(hero) ? .2 : thinkLen)) botAct();  // no waiting once the user is out
        } else {
            float x = 1260, y = 712, bw = 150, bh = 64;
            auto legal = hand->legal();
            auto has = [&](pf::Act a) { return std::find(legal.begin(), legal.end(), a) != legal.end(); };
            if (has(pf::Act::Raise)) {
                bool allIn = hand->raiseTo() >= hand->committed(hero) + hand->stack(hero) - 1e-9;
                buttons.push_back({{x -= bw, y, bw, bh}, pf::Act::Raise, allIn ? "All-in" : "Raise To",
                                   fmt(hand->raiseTo()), {196, 82, 40, 255}, KEY_R});
                x -= 12;
            }
            if (has(pf::Act::Call)) {
                double c = std::min(hand->currentBet() - hand->committed(hero), hand->stack(hero));
                buttons.push_back({{x -= bw, y, bw, bh}, pf::Act::Call, "Call", fmt(c), {46, 140, 92, 255}, KEY_C});
                x -= 12;
            }
            if (has(pf::Act::Fold))
                buttons.push_back({{x -= bw, y, bw, bh}, pf::Act::Fold, "Fold", "", {150, 44, 50, 255}, KEY_F});
            for (auto& b : buttons)
                if (IsKeyPressed(b.key) || (click && CheckCollisionPointRec(mouse, b.r))) {
                    act(b.act, true);
                    buttons.clear();
                    break;
                }
            turn = hand->toAct();
        }

        // --- draw ---
        BeginDrawing();
        DrawRectangleGradientV(0, 0, W, H, {28, 31, 37, 255}, {12, 13, 16, 255});

        // table: dark rim, green felt, faint inner line
        DrawRectangleRounded({150, 135, 980, 450}, 1, 64, {44, 47, 54, 255});
        DrawRectangleRounded({160, 145, 960, 430}, 1, 64, {20, 22, 26, 255});
        DrawRectangleRounded({176, 161, 928, 398}, 1, 64, {24, 104, 72, 255});
        DrawRectangleRounded({300, 220, 680, 280}, 1, 64, {30, 118, 82, 255});
        DrawRectangleRoundedLinesEx({196, 181, 888, 358}, 1, 64, 2, {255, 255, 255, 22});
        if (!hand->done()) text(bold, "PREFLOP", {640, 400}, 34, {255, 255, 255, 24});
        text(bold, "Pot: " + fmt(hand->pot()), {640, 335}, 22, WHITE);

        for (int s = 0; s < pf::NPOS; s++) {
            int p = posOf(s, btn);
            bool folded = hand->folded(p);
            bool acting = !hand->done() && p == turn;
            unsigned char a = folded ? 110 : 255;
            Vector2 c = SEAT[s];

            if (hand->committed(p) > 0) chips(BET[s], hand->committed(p));

            if (s == HERO) {
                if (!folded) {
                    card(hand->cards[p][0], {c.x - 78, c.y - 125, 74, 104});
                    card(hand->cards[p][1], {c.x + 4, c.y - 125, 74, 104});
                }
            } else {
                if (!folded) {
                    cardBack({c.x - 26, c.y - 78, 30, 42}, a);
                    cardBack({c.x - 4, c.y - 74, 30, 42}, a);
                }
                DrawCircleV({c.x, c.y - 22}, 34, {12, 12, 14, a});
                DrawCircleV({c.x, c.y - 22}, 31, ColorAlpha(AVATAR[s], a / 255.f));
                text(bold, std::string(1, SEAT_NAME[s][4]), {c.x, c.y - 22}, 28, {255, 255, 255, a});
            }

            // nameplate
            Rectangle np = {c.x - 78, c.y + 8, 156, 50};
            DrawRectangleRounded(np, .45f, 12, {16, 17, 20, 240});
            if (acting) DrawRectangleRoundedLinesEx(np, .45f, 12, 2, {240, 196, 64, 255});
            text(font, std::string(SEAT_NAME[s]) + "  " + POS_NAME[p], {c.x, c.y + 22}, 15, {170, 174, 182, a});
            text(bold, hand->stack(p) > 0 ? fmt(hand->stack(p)) + " BB" : "All-in", {c.x, c.y + 42}, 19, {255, 255, 255, a});

            // last action tag
            if (!label[p].empty()) {
                Color lc = label[p] == "Fold" ? Color{90, 94, 102, 255}
                         : label[p] == "Call" ? Color{46, 140, 92, 255}
                                              : Color{196, 82, 40, 255};
                DrawRectangleRounded({c.x - 34, c.y - 4, 68, 20}, 1, 8, lc);
                text(bold, label[p], {c.x, c.y + 6}, 14, WHITE);
            }

            // think timer for bots
            if (acting && s != HERO) {
                float f = (float)std::max(0.0, 1 - clock / std::max(thinkLen, 3.0));
                DrawRectangleRounded({np.x + 10, np.y + np.height + 5, np.width - 20, 6}, 1, 4, {40, 42, 48, 255});
                DrawRectangleRounded({np.x + 10, np.y + np.height + 5, (np.width - 20) * f, 6}, 1, 4,
                                     f > .3f ? Color{80, 200, 120, 255} : Color{230, 80, 70, 255});
            }
        }

        // dealer button
        Vector2 d = DEALER[btn];
        DrawCircleV(d, 14, {250, 250, 250, 255});
        DrawCircleLinesV(d, 14, {120, 120, 120, 255});
        text(bold, "D", d, 16, {30, 30, 30, 255});

        // action buttons
        for (auto& b : buttons) {
            bool hover = CheckCollisionPointRec(mouse, b.r);
            DrawRectangleRounded(b.r, .3f, 12, hover ? ColorBrightness(b.col, .15f) : b.col);
            if (b.amount.empty()) {
                text(bold, b.title, {b.r.x + b.r.width / 2, b.r.y + b.r.height / 2}, 22, WHITE);
            } else {
                text(font, b.title, {b.r.x + b.r.width / 2, b.r.y + 22}, 17, {255, 255, 255, 220});
                text(bold, b.amount, {b.r.x + b.r.width / 2, b.r.y + 44}, 22, WHITE);
            }
        }
        if (!buttons.empty()) text(font, "F / C / R", {1260 - 3 * 150 / 2.f - 12, 790}, 13, GREY);

        if (hand->done() && view < 0) {
            int alive = 0, winner = -1;
            for (int p = 0; p < pf::NPOS; p++)
                if (!hand->folded(p)) alive++, winner = p;
            std::string msg = alive == 1 ? std::string(SEAT_NAME[seatOf(winner, btn)]) + " wins " + fmt(hand->pot()) + " BB"
                                         : std::to_string(alive) + " players to the flop";
            DrawRectangleRounded({490, 360, 300, 40}, 1, 12, {0, 0, 0, 150});
            text(bold, msg, {640, 380}, 20, WHITE);
        }

        // top bar: spot picker, stats, current drill
        if (!menu && view < 0) {
            if (button({16, 16, 110, 34}, "Spots (P)", false, 15)) menu = 1;
            if (button({134, 16, 110, 34}, "Stats (S)", false, 15)) menu = 2;
        }
        std::string drill = "Drill: " + std::string(pf::spotName(filter.type)) + ", " + POS_PICK[filter.pos + 1];
        DrawTextEx(font, (drill + (notice.empty() ? "" : "   (" + notice + ")")).c_str(), {256, 25}, 15, 0, GREY);
        DrawTextEx(font, "Click a player to see their range  |  H: your range", {16, H - 26}, 14, 0, {110, 114, 122, 255});

        if (view >= 0) {
            const Decision& dv = log[view];
            std::array<float, 169> reach;  // how often each class takes this player's earlier actions
            reach.fill(1);
            for (int i = 0; i < view; i++)
                if (log[i].pos == dv.pos)
                    for (int b = 0; b < pf::NBUCKETS; b++) reach[b] *= strat.probs(log[i].node, b)[int(log[i].act)];
            std::string title = std::string(SEAT_NAME[seatOf(dv.pos, btn)]) + " - " + POS_NAME[dv.pos];
            if (dv.sp) title += "  (" + std::string(pf::spotName(dv.sp->type)) + ")";
            rangePanel(strat, dv, reach, title, dv.pos == hero ? pf::bucket(hand->cards[hero][0], hand->cards[hero][1]) : -1,
                       hand->done());
        }
        if (menu == 1 && pickerPanel(filter)) menu = 0;
        if (menu == 2 && statsPanel(stats)) menu = 0;

        EndDrawing();
    }
    CloseWindow();
}
