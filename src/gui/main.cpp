// GGPoker-style 6-max preflop trainer table (docs/trainer.md "GUI"). Bots play the strategy with
// human-like think times; each of the user's decisions is rolled out on a worker thread and graded.
// Usage: preflop [strategy file] [heads-up equity table]  (defaults: data/strategy_6max.bin, data/hu_equity.bin)
#include "raylib.h"
#include "solver/cards.h"
#include "solver/range.h"
#include "solver/strategy.h"
#include "solver/utility.h"
#include "trainer/game.h"
#include "trainer/trainer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    std::future<std::pair<std::array<double, 3>, pf::RolloutStats>> ev;  // rollout in flight
    std::optional<pf::Feedback> fb;
    pf::RolloutStats rs;
    bool lowSample = false;  // rollout hit maxDeals before reaching minEss
};

// Grid/legend colours per pf::Act (Fold, Call, Raise).
const Color ACT_COL[] = {{58, 98, 150, 255}, {46, 140, 92, 255}, {196, 82, 40, 255}};
const char* ACT_NAME[] = {"Fold", "Call", "Raise"};
const Color GREY = {130, 134, 142, 255};
constexpr Rectangle PANEL = {190, 60, 900, 680};
constexpr Rectangle CLOSE_BTN = {PANEL.x + PANEL.width - 224, PANEL.y + PANEL.height - 72, 200, 48};
constexpr float CELL = 42, GX = PANEL.x + 24, GY = PANEL.y + 88;
constexpr Rectangle MENU = {290, 170, 700, 420};

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

std::string pm(double se) {  // " ± 0.27"; "" for an exact value (fold, or the loss of the best action)
    char b[24];
    std::snprintf(b, sizeof b, " \u00b1 %.2f", se);
    return se > 0 ? b : "";
}

// EV per action, the user's EV loss and verdict.
void feedback(const Decision& d, float x, float y) {
    const pf::Config& cfg = d.sp->hand.config();  // EVs are net of rake (rollout -> utility)
    char hdr[64];
    if (cfg.rake > 0) std::snprintf(hdr, sizeof hdr, "EV (bb, net of %g%% rake, cap %s)", 100 * cfg.rake, fmt(cfg.rakeCap).c_str());
    else std::snprintf(hdr, sizeof hdr, "EV (bb)   no rake");
    DrawTextEx(font, hdr, {x, y}, 15, 0, GREY);
    if (d.fb && d.lowSample) DrawTextEx(bold, "low sample", {x + MeasureTextEx(font, hdr, 15, 0).x + 10, y + 1}, 14, 0, {230, 160, 60, 255});
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
        std::snprintf(b, sizeof b, "%+.2f%s%s", f.ev[a], pm(d.rs.se[a]).c_str(), a == int(f.choice) ? "  < you" : "");
        DrawTextEx(bold, b, {x + 110, y}, 18, 0, WHITE);
        y += 24;
    }
    char b[64];
    if (std::isnan(f.evLoss)) std::snprintf(b, sizeof b, "No EV: path never reached");
    else std::snprintf(b, sizeof b, "%s   loss %.2f%s bb", f.correct ? "Correct" : "Mistake", f.evLoss, pm(f.evLossSe).c_str());
    DrawTextEx(bold, b, {x, y + 6}, 20, 0, std::isnan(f.evLoss) ? GREY : f.correct ? Color{80, 200, 120, 255} : Color{230, 80, 70, 255});
}

// A strategy the trainer can play (T cycles them): the solve, or a chart set written over a base strategy.
struct Mode {
    std::string name, dir, bin;   // chart dir and chart-locked solve file, "" for the solver
    pf::Strategy st;
    std::vector<uint8_t> locked;  // [node * NBUCKETS + bucket] = 1 where st's mix comes from a chart; empty = no charts
    std::string fallback;         // "no chart, ..." label for uncharted spots
    bool charted(int node, int b) const { return !locked.empty() && locked[size_t(node) * pf::NBUCKETS + b]; }
};

// 13x13 strategy grid for one decision. reach = how often each class gets here (dimmed if never);
// highlight = the user's bucket, -1 for bots (cards hidden).
void rangePanel(const Mode& mode, const Decision& d, const std::array<double, pf::NBUCKETS>& reach, const std::string& title,
                int highlight, bool nextHand) {
    const pf::Strategy& st = mode.st;
    panelBg(PANEL);
    DrawTextEx(bold, title.c_str(), {GX, PANEL.y + 20}, 26, 0, WHITE);
    DrawTextEx(font, (d.spot.empty() ? "Unopened pot" : d.spot).c_str(), {GX, PANEL.y + 54}, 17, 0, {150, 154, 162, 255});
    bool anyChart = false;
    for (int b = 0; b < pf::NBUCKETS; b++) anyChart |= mode.charted(d.node, b);
    if (!mode.locked.empty() && !anyChart)
        text(bold, mode.fallback, {PANEL.x + PANEL.width - 150, PANEL.y + 34}, 16, {230, 160, 60, 255});

    Vector2 m = GetMousePosition();
    int hover = -1;
    for (int b = 0; b < pf::NBUCKETS; b++) {
        Rectangle c = {GX + b % 13 * CELL, GY + b / 13 * CELL, CELL - 2, CELL - 2};
        if (CheckCollisionPointRec(m, c)) hover = b;
        bool reached = reach[b] >= 1e-3;
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
        std::string src = "  (" + (mode.charted(d.node, shown) ? mode.name : std::string("Solved")) + ")";
        DrawTextEx(font, ((hover >= 0 ? "Hand" : "Your hand") + src).c_str(), {x, y}, 15, 0, GREY);
        DrawTextEx(bold, pf::bucketName(shown).c_str(), {x, y + 18}, 26, 0, WHITE);
        actRows(st.probs(d.node, shown), x, y + 54);
    } else {
        DrawTextEx(font, "Hover a hand for its mix", {x, y + 18}, 16, 0, GREY);
    }
    y += 150;
    std::array<float, 3> width{};
    float total = 0;
    for (int b = 0; b < pf::NBUCKETS; b++) {
        float w = pf::comboCount(b) * float(reach[b]);
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

    DrawTextEx(font, "<- -> other decisions this hand   I: realization for this spot", {GX, PANEL.y + PANEL.height - 30}, 14, 0, GREY);
    button(CLOSE_BTN, nextHand ? "Next hand  (Space)" : "Close  (Esc)");  // click handled in update
}

const char* POS_PICK[] = {"Random", "UTG", "HJ", "CO", "BTN", "SB", "BB"};

// Whether the user can ever face spot type t in position pos (-1 = random: always). Measured by deal()
// running out of tries; the GUI also falls back to any spot if a combination is missed here.
bool possible(pf::SpotType t, int pos) {
    using enum pf::SpotType;
    switch (t) {
    case Open: return pos != pf::BB;
    case ThreeBet: return pos != pf::UTG;
    case Squeeze: return pos != pf::UTG && pos != pf::HJ;
    case VsSqueeze: return pos != pf::UTG && pos != pf::BB;
    default: return true;
    }
}

// Spot picker: which decision to drill. Applies from the next hand.
bool pickerPanel(pf::SpotFilter& f) {
    panelBg(MENU);
    DrawTextEx(bold, "Drill spots", {MENU.x + 28, MENU.y + 24}, 26, 0, WHITE);
    DrawTextEx(font, "Applies from the next hand", {MENU.x + 28, MENU.y + 58}, 16, 0, GREY);
    DrawTextEx(font, "Spot", {MENU.x + 28, MENU.y + 100}, 15, 0, GREY);
    float bw = (MENU.width - 56 + 8) / (pf::NSPOT + 1);  // one row of buttons, however many spot types exist
    for (int t = 0; t <= pf::NSPOT; t++) {  // NSPOT ("Any") first
        int i = (t + pf::NSPOT) % (pf::NSPOT + 1);
        if (button({MENU.x + 28 + t * bw, MENU.y + 124, bw - 8, 40}, pf::spotName(pf::SpotType(i)), int(f.type) == i, 15))
            f.type = pf::SpotType(i);
    }
    DrawTextEx(font, "Your position", {MENU.x + 28, MENU.y + 196}, 15, 0, GREY);
    if (!possible(f.type, f.pos)) f.pos = -1;
    for (int p = -1; p < pf::NPOS; p++) {
        Rectangle r = {MENU.x + 28 + (p + 1) * 92.f, MENU.y + 220, 84, 40};
        if (!possible(f.type, p)) {  // greyed out, not clickable
            DrawRectangleRounded(r, .3f, 8, {36, 38, 44, 255});
            text(bold, POS_PICK[p + 1], {r.x + r.width / 2, r.y + r.height / 2}, 16, {80, 84, 92, 255});
        } else if (button(r, POS_PICK[p + 1], f.pos == p, 16)) {
            f.pos = p;
        }
    }
    DrawTextEx(font, "Random position with any spot rotates the button as usual.", {MENU.x + 28, MENU.y + 280}, 15, 0, GREY);
    return button({MENU.x + MENU.width - 228, MENU.y + MENU.height - 72, 200, 48}, "Done  (Esc)");
}

// Stats per (spot, position), biggest total EV loss first.
bool statsPanel(const pf::Stats& st, const char* title) {
    panelBg(MENU);
    int n = 0, correct = 0;
    double loss = 0;
    for (auto& row : st.cell)
        for (auto& c : row) n += c.n, correct += c.correct, loss += c.evLoss;
    char b[96];
    std::snprintf(b, sizeof b, "%d decisions   %.0f%% correct   avg EV loss %.2f bb", n, n ? 100.0 * correct / n : 0.0,
                  n ? loss / n : 0.0);
    DrawTextEx(bold, title, {MENU.x + 28, MENU.y + 24}, 26, 0, WHITE);
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

// ---- Reference painter: enter reference charts by hand as ref/*.txt for the validate tool ----

pf::Hand handAt(const pf::Strategy& st, const std::vector<pf::Act>& hist) {  // betting state after `hist`
    std::mt19937 r(0);
    pf::Hand h(r, st.config());
    for (auto a : hist) h.apply(a);
    return h;
}

std::string actLabel(const pf::Hand& h, pf::Act a) {  // "Fold", "Call 2.5", "Raise 8.8"
    int p = h.toAct();
    if (a == pf::Act::Fold) return "Fold";
    return std::string(ACT_NAME[int(a)]) + " " +
           fmt(a == pf::Act::Call ? std::min(h.currentBet(), h.committed(p) + h.stack(p)) : h.raiseTo());
}

struct Painter {
    const pf::Strategy& st;
    std::string dir;                  // the repo's ref/
    std::vector<pf::Act> hist;        // the spot: action history from UTG
    int node = 0;
    pf::RangeFreqs freq[3]{};         // per pf::Act; fold is the remainder, freq[0] unused
    std::array<double, pf::NBUCKETS> reach{};
    int brush = 2;                    // action being painted: 1 call, 2 raise
    float level = 1;                  // brush frequency
    bool overlay = false;             // solved strategy strip under each cell
    bool stroke = false, dirty = false;
    std::string path, comments, status;  // path "" = not saving (unreadable file in the way)
    std::string category;             // kept from the loaded file ("other" = no line)
    bool listedOnly = false;          // likewise its "scope: listed"
    std::vector<std::string> files;
    bool inspect = false;             // asks main to open the R inspector for this spot

    static constexpr float PX = 24, PY = 136, PC = 44;  // grid origin and cell size

    Painter(const pf::Strategy& s, std::string d) : st(s), dir(std::filesystem::weakly_canonical(d).string()) {
        refresh();
        go({});
    }

    bool legal(int a) const { return st.tree().legal[node] >> a & 1; }
    std::string letters() const {
        std::string s;
        for (auto a : hist) s += "FCR"[int(a)];
        return s;
    }
    std::string name(const std::string& p) const { return std::filesystem::path(p).filename().string(); }

    void refresh() {
        files.clear();
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().extension() == ".txt") files.push_back(e.path().string());
        std::sort(files.begin(), files.end());
    }

    void setSpot(const std::vector<pf::Act>& h) {
        hist = h;
        node = st.nodeFor(h);
        reach = st.reach(h);
        for (auto& x : freq) x.fill(0);
        brush = legal(2) ? 2 : 1;
        comments = "# painted in the preflop GUI\n";
        category = "other", listedOnly = false;
        path = dir + "/" + POS_NAME[st.actor(node)] + "_" + (h.empty() ? "open" : letters()) + ".txt";
    }

    bool load(const std::string& p) {
        pf::Chart c;
        std::string err;
        if (!pf::readChart(p, c, err)) return status = name(p) + ": " + err, false;
        std::vector<pf::Act> h;
        if (!pf::parseHistory(c.spot, h)) return status = name(p) + ": bad spot '" + c.spot + "'", false;
        if (st.nodeFor(h) < 0) return status = name(p) + ": spot not in the tree", false;
        setSpot(h);
        std::copy(std::begin(c.act), std::end(c.act), std::begin(freq));
        comments = c.comments, category = c.category, listedOnly = c.listedOnly;
        path = p;
        status = "Editing " + name(p);
        return true;
    }

    // Switches spot, opening the ref file for it if there is one.
    void go(const std::vector<pf::Act>& h) {
        setSpot(h);
        for (auto& f : files) {
            pf::Chart c;
            std::string err;
            if (pf::readChart(f, c, err) && c.spot == letters() && load(f)) return;
        }
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {  // never paint over a file we couldn't read
            pf::Chart c;
            std::string err;
            pf::readChart(path, c, err);
            status = name(path) + " exists but can't be used (" + err + "); fix it by hand";
            path.clear();
        } else {
            status = "New spot, saves to " + name(path);
        }
    }

    // Autosaved after every stroke. Never deletes: an all-fold chart keeps empty lines (= 0%, a valid reference).
    void save() {
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(path);
        out << comments;
        if (category != "other") out << "category: " << category << "\n";
        out << "spot: " << letters() << "\n";
        for (int a : {2, 1})
            if (legal(a)) out << (a == 2 ? "raise: " : "call: ") << pf::formatRange(freq[a]) << "\n";
        if (listedOnly) out << "scope: listed\n";
        status = out ? "Saved " + name(path) : "Could not write " + path;
        refresh();
    }

    // One frame: input + drawing. Returns false to go back to the table.
    bool frame() {
        Vector2 m = GetMousePosition();
        std::optional<std::vector<pf::Act>> to;
        DrawRectangleGradientV(0, 0, W, H, {28, 31, 37, 255}, {12, 13, 16, 255});
        DrawTextEx(bold, "Reference painter", {PX, 18}, 26, 0, WHITE);
        DrawTextEx(font, status.c_str(), {PX + 250, 25}, 16, 0, path.empty() ? Color{230, 80, 70, 255} : GREY);

        // spot: history and navigation
        pf::Hand h = handAt(st, hist);
        std::string spot;
        {
            pf::Hand g = handAt(st, {});
            for (auto a : hist) spot += std::string(POS_NAME[g.toAct()]) + " " + actLabel(g, a) + ", ", g.apply(a);
        }
        spot += std::string(POS_NAME[h.toAct()]) + " to act";
        DrawTextEx(font, spot.c_str(), {PX, 58}, 17, 0, {200, 204, 210, 255});
        float x = PX;
        if (button({x, 86, 80, 34}, "Root", false, 15) && !hist.empty()) to = std::vector<pf::Act>{};
        x += 88;
        if (button({x, 86, 80, 34}, "Back", false, 15) && !hist.empty()) to = std::vector<pf::Act>(hist.begin(), hist.end() - 1);
        x += 100;
        for (int a = 0; a < 3; a++) {
            if (!legal(a) || st.child(node, pf::Act(a)) < 0) continue;  // terminal: no spot after it
            std::string t = POS_NAME[h.toAct()] + std::string(" ") + actLabel(h, pf::Act(a)) + " >";
            float w = MeasureTextEx(bold, t.c_str(), 15, 0).x + 28;
            if (button({x, 86, w, 34}, t, false, 15)) to = hist, to->push_back(pf::Act(a));
            x += w + 8;
        }

        // grid: left-drag paints the brush, right-drag erases the brush action
        int hover = -1;
        for (int b = 0; b < pf::NBUCKETS; b++) {
            Rectangle c = {PX + b % 13 * PC, PY + b / 13 * PC, PC - 2, PC - 2};
            if (CheckCollisionPointRec(m, c)) hover = b;
        }
        bool down = IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
        if (hover >= 0 && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))) stroke = true;
        if (stroke && hover >= 0 && down && !path.empty()) {
            float v = IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? level : 0;
            dirty |= freq[brush][hover] != v;
            freq[brush][hover] = v;
            freq[3 - brush][hover] = std::min(freq[3 - brush][hover], 1 - v);  // raise + call <= 1
        }
        if (!down) {
            if (dirty) save();
            stroke = dirty = false;
        }
        for (int b = 0; b < pf::NBUCKETS; b++) {
            Rectangle c = {PX + b % 13 * PC, PY + b / 13 * PC, PC - 2, PC - 2};
            float r = freq[2][b], k = freq[1][b], strip = overlay ? 9 : 0;
            DrawRectangleRec({c.x, c.y, c.width, c.height - strip}, ACT_COL[0]);
            DrawRectangleRec({c.x, c.y, c.width * r, c.height - strip}, ACT_COL[2]);
            DrawRectangleRec({c.x + c.width * r, c.y, c.width * k, c.height - strip}, ACT_COL[1]);
            if (overlay) {
                Rectangle s = {c.x, c.y + c.height - strip, c.width, strip};
                if (reach[b] < 1e-3) DrawRectangleRec(s, {38, 41, 48, 255});
                else
                    for (int a = 2; a >= 0; a--) {
                        float w = c.width * st.probs(node, b)[a];
                        DrawRectangleRec({s.x, s.y, w, s.height}, ColorBrightness(ACT_COL[a], -.35f));
                        s.x += w;
                    }
            }
            std::string n = pf::bucketName(b);
            text(bold, n, {c.x + c.width / 2 + 1, c.y + (c.height - strip) / 2 + 1}, 14, {0, 0, 0, 120});
            text(bold, n, {c.x + c.width / 2, c.y + (c.height - strip) / 2}, 14, WHITE);
        }
        if (hover >= 0) DrawRectangleLinesEx({PX + hover % 13 * PC - 2, PY + hover / 13 * PC - 2, PC + 2, PC + 2}, 3, WHITE);

        // right column: brush, overlay, widths, hovered hand, files
        float cx = PX + 13 * PC + 32, y = PY;
        DrawTextEx(font, "Brush action  (R / C)", {cx, y}, 15, 0, GREY);
        for (int a : {2, 1})
            if (legal(a) && button({cx + (2 - a) * 128.f, y + 20, 120, 36}, ACT_NAME[a], brush == a, 16)) brush = a;
        if (IsKeyPressed(KEY_R) && legal(2)) brush = 2;
        if (IsKeyPressed(KEY_C) && legal(1)) brush = 1;
        y += 72;
        DrawTextEx(font, "Frequency  (1-5)", {cx, y}, 15, 0, GREY);
        const float LV[] = {1, .75f, .5f, .25f, 0};
        for (int i = 0; i < 5; i++) {
            if (button({cx + i * 76.f, y + 20, 70, 36}, std::to_string(int(LV[i] * 100)) + "%", level == LV[i], 16)) level = LV[i];
            if (IsKeyPressed(KeyboardKey(KEY_ONE + i))) level = LV[i];
        }
        y += 72;
        if (button({cx, y, 200, 36}, overlay ? "Solver overlay: on (O)" : "Solver overlay: off (O)", overlay, 15) ||
            IsKeyPressed(KEY_O))
            overlay = !overlay;
        y += 56;

        // combo-weighted widths, as % of all 1326 combos (solver: reach-weighted)
        DrawTextEx(font, overlay ? "Width        chart     solver" : "Width (% of combos)", {cx, y}, 15, 0, GREY);
        y += 22;
        for (int a = 2; a >= 0; a--) {
            if (!legal(a)) continue;
            double ref = 0, sol = 0;
            for (int b = 0; b < pf::NBUCKETS; b++) {
                ref += pf::comboCount(b) * (a ? freq[a][b] : 1 - freq[1][b] - freq[2][b]);
                sol += pf::comboCount(b) * reach[b] * st.probs(node, b)[a];
            }
            char buf[64];
            std::snprintf(buf, sizeof buf, overlay ? "%5.1f%%   %5.1f%%" : "%5.1f%%", 100 * ref / pf::NCOMBOS, 100 * sol / pf::NCOMBOS);
            DrawRectangleRounded({cx, y + 3, 14, 14}, .3f, 4, ACT_COL[a]);
            DrawTextEx(font, ACT_NAME[a], {cx + 22, y}, 18, 0, {200, 204, 210, 255});
            DrawTextEx(bold, buf, {cx + 110, y}, 18, 0, WHITE);
            y += 24;
        }
        y += 12;
        if (hover >= 0) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s   chart R %.0f%%  C %.0f%%", pf::bucketName(hover).c_str(), 100 * freq[2][hover],
                          100 * freq[1][hover]);
            DrawTextEx(bold, buf, {cx, y}, 18, 0, WHITE);
            if (overlay) {
                auto& p = st.probs(node, hover);
                std::snprintf(buf, sizeof buf, "solver R %.0f%%  C %.0f%%  F %.0f%%%s", 100 * p[2], 100 * p[1], 100 * p[0],
                              reach[hover] < 1e-3 ? "  (never here)" : "");
                DrawTextEx(font, buf, {cx, y + 24}, 16, 0, {200, 204, 210, 255});
            }
        }
        y += 60;
        DrawTextEx(font, ("Files in " + name(dir) + "/  (click to open)").c_str(), {cx, y}, 15, 0, GREY);
        y += 22;
        for (size_t i = 0; i < files.size() && i < 12; i++) {
            Rectangle r = {cx + (i % 2) * 210.f, y + (i / 2) * 30.f, 202, 26};
            if (button(r, name(files[i]), files[i] == path, 14)) load(files[i]);
        }

        if (IsKeyPressed(KEY_I)) inspect = true;
        DrawTextEx(font, "Left-drag: paint   Right-drag: erase   Autosaves after each stroke   Fold = the rest   I: realization",
                   {PX, H - 30}, 14, 0, {110, 114, 122, 255});
        bool back = button({W - 200, H - 52, 176, 36}, "Back to table (Esc)", false, 15) || IsKeyPressed(KEY_ESCAPE);
        if (to && !stroke) go(*to);
        return !back;
    }
};

// ---- R inspector: how the realization model values each hand class at a flop ----

// Representative cards for a grid cell: one suit for suited, two suits for offsuit and pairs.
std::array<pf::Card, 2> cellCards(int b) {
    int row = b / 13, col = b % 13;
    uint8_t hi = 12 - std::min(row, col), lo = 12 - std::max(row, col);
    return {pf::Card{hi, 3}, pf::Card{lo, uint8_t(row < col ? 3 : 2)}};
}

// Diverging scale: neutral grey at `mid`, blue above, orange below, full colour at mid +- span.
Color diverge(double v, double mid, double span) {
    float t = std::clamp(float((v - mid) / span), -1.f, 1.f);
    Color n = {64, 68, 78, 255}, c = t > 0 ? Color{48, 118, 206, 255} : Color{214, 120, 40, 255};
    t = std::abs(t);
    return {uint8_t(n.r + (c.r - n.r) * t), uint8_t(n.g + (c.g - n.g) * t), uint8_t(n.b + (c.b - n.b) * t), 255};
}

struct Inspector {
    const pf::Strategy& st;
    bool ip = true, raiser = false, diff = false;  // diff: show aggressor R minus caller R
    bool reraised = false;                          // 3-bet+ pot (realization()'s ipCall/domPot)
    double spr = 18;
    bool sliding = false;
    struct Preset {
        std::string name;
        double spr;
        int ip = -1, raiser = -1, reraised = -1;  // -1 = leave as is
    };
    std::vector<Preset> presets = {{"Single-raised pot  18", 18, -1, -1, 0}, {"3-bet pot  5", 5, -1, -1, 1}, {"4-bet pot  1.3", 1.3, -1, -1, 1}};
    std::string from;  // spot it was opened from, "" if none

    static constexpr float IX = 24, IY = 150, IC = 44;
    static constexpr double LO = .5, HI = 30;  // slider range, log scale

    // Adds the actor's two outcomes at a spot as presets: calling, and raising and getting called.
    // ponytail: assumes heads-up against the last raiser (or the BB when unopened); multiway pots are
    // smaller per player, so treat these SPRs as upper bounds.
    void fromSpot(const std::vector<pf::Act>& hist) {
        pf::Hand h = handAt(st, hist);
        int me = h.toAct(), opp = h.lastRaiser() >= 0 ? h.lastRaiser() : me == pf::BB ? pf::SB : pf::BB;
        auto order = [](int p) { return p >= pf::SB ? p - pf::SB : p + 2; };  // postflop: SB first, BTN last
        bool meIP = order(me) > order(opp);
        double stack = st.config().stack;
        from = std::string(POS_NAME[me]) + " to act, heads-up vs " + POS_NAME[opp];
        auto legal = h.legal();
        auto has = [&](pf::Act a) { return std::find(legal.begin(), legal.end(), a) != legal.end(); };
        if (has(pf::Act::Call)) {
            double to = std::min(h.currentBet(), h.committed(me) + h.stack(me));
            double pot = h.pot() + to - h.committed(me);
            if (to < stack) presets.push_back({"If " + std::string(POS_NAME[me]) + " calls  " + fmt(std::round((stack - to) / pot * 10) / 10), (stack - to) / pot, meIP, 0, h.raises() >= 2});
        }
        if (has(pf::Act::Raise)) {
            double to = h.raiseTo(), pot = h.pot() + (to - h.committed(me)) + (to - h.committed(opp));
            if (to < stack) presets.push_back({"If the raise is called  " + fmt(std::round((stack - to) / pot * 10) / 10), (stack - to) / pot, meIP, 1, h.raises() >= 1});
        }
        auto& p = presets.back();
        if (presets.size() > 3) spr = p.spr, ip = p.ip, raiser = p.raiser, reraised = p.reraised;
    }

    double r(int b, bool agg) const {
        auto c = cellCards(b);
        return pf::realization(st.realization(), ip, agg, spr, c[0], c[1], reraised);
    }

    // One frame: input + drawing. Returns false to close.
    bool frame() {
        Vector2 m = GetMousePosition();
        DrawRectangleGradientV(0, 0, W, H, {28, 31, 37, 255}, {12, 13, 16, 255});
        DrawTextEx(bold, "Realization inspector", {IX, 18}, 26, 0, WHITE);
        DrawTextEx(font, "At a flop each player's pot share is R x equity, normalized. R < 1: the hand realizes less than its equity.",
                   {IX, 54}, 16, 0, GREY);
        if (!from.empty()) DrawTextEx(font, ("Opened from: " + from).c_str(), {IX, 76}, 16, 0, {200, 204, 210, 255});

        // controls
        float y = 104;
        if (button({IX, y, 70, 32}, "IP", ip, 15)) ip = true;
        if (button({IX + 76, y, 70, 32}, "OOP", !ip, 15)) ip = false;
        if (button({IX + 170, y, 100, 32}, "Caller", !raiser && !diff, 15)) raiser = diff = false;
        if (button({IX + 276, y, 110, 32}, "Aggressor", raiser && !diff, 15)) raiser = true, diff = false;
        if (button({IX + 392, y, 180, 32}, "Aggressor - caller", diff, 15)) diff = true;
        if (button({IX + 596, y, 120, 32}, "3-bet+ pot", reraised, 15)) reraised = !reraised;

        // grid
        int hover = -1;
        for (int b = 0; b < pf::NBUCKETS; b++) {
            Rectangle c = {IX + b % 13 * IC, IY + b / 13 * IC, IC - 2, IC - 2};
            if (CheckCollisionPointRec(m, c)) hover = b;
            double v = diff ? r(b, true) - r(b, false) : r(b, raiser);
            DrawRectangleRec(c, diff ? diverge(v, 0, .3) : diverge(v, 1, .5));
            char val[16];
            std::snprintf(val, sizeof val, diff ? "%+.2f" : "%.2f", v);
            text(bold, pf::bucketName(b), {c.x + c.width / 2, c.y + 14}, 13, WHITE);
            text(font, val, {c.x + c.width / 2, c.y + 30}, 12, {230, 232, 236, 255});
        }
        if (hover >= 0) DrawRectangleLinesEx({IX + hover % 13 * IC - 2, IY + hover / 13 * IC - 2, IC + 2, IC + 2}, 3, WHITE);

        // right column: SPR, presets, legend, hovered hand
        float cx = IX + 13 * IC + 32;
        y = IY;
        char buf[96];
        std::snprintf(buf, sizeof buf, "SPR  %.1f", spr);
        DrawTextEx(bold, buf, {cx, y}, 20, 0, WHITE);
        Rectangle track = {cx, y + 34, 360, 6};
        float t = float(std::log(spr / LO) / std::log(HI / LO));
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, {track.x - 8, track.y - 12, track.width + 16, 30})) sliding = true;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) sliding = false;
        if (sliding) t = std::clamp((m.x - track.x) / track.width, 0.f, 1.f), spr = LO * std::pow(HI / LO, t);
        DrawRectangleRounded(track, 1, 4, {52, 56, 64, 255});
        DrawCircleV({track.x + track.width * t, track.y + 3}, 9, {214, 158, 46, 255});
        DrawTextEx(font, "0.5", {track.x, track.y + 14}, 13, 0, GREY);
        DrawTextEx(font, "30", {track.x + track.width - 14, track.y + 14}, 13, 0, GREY);
        y += 70;
        for (auto& p : presets) {
            bool on = std::abs(spr - p.spr) < 1e-9 && (p.ip < 0 || p.ip == ip) && (p.raiser < 0 || p.raiser == raiser) &&
                      (p.reraised < 0 || p.reraised == reraised);
            if (button({cx, y, 260, 30}, p.name, on, 14)) {
                spr = p.spr;
                if (p.ip >= 0) ip = p.ip;
                if (p.raiser >= 0) raiser = p.raiser, diff = false;
                if (p.reraised >= 0) reraised = p.reraised;
            }
            y += 36;
        }
        y += 10;
        for (int i = 0; i < 180; i++)
            DrawRectangleRec({cx + i * 2.f, y, 2, 12}, diff ? diverge(-.3 + .6 * i / 179, 0, .3) : diverge(.5 + i / 179.0, 1, .5));
        DrawTextEx(font, diff ? "-0.3" : "0.5", {cx, y + 16}, 13, 0, GREY);
        DrawTextEx(font, diff ? "0" : "1.0", {cx + 172, y + 16}, 13, 0, GREY);
        DrawTextEx(font, diff ? "+0.3" : "1.5", {cx + 336, y + 16}, 13, 0, GREY);
        y += 48;
        if (hover >= 0) {
            DrawTextEx(bold, pf::bucketName(hover).c_str(), {cx, y}, 26, 0, WHITE);
            double rc = r(hover, false), ra = r(hover, true);
            std::snprintf(buf, sizeof buf, "caller %.3f   aggressor %.3f   (%+.3f)", rc, ra, ra - rc);
            DrawTextEx(font, buf, {cx, y + 34}, 17, 0, {200, 204, 210, 255});
            auto c = cellCards(hover);  // the model's inputs for this hand
            double sp = st.realization().spr, g = sp > 0 ? spr / (spr + sp) : 1;
            std::snprintf(buf, sizeof buf, "straights %d   domination %d   %s   depth g %.2f", pf::straights(c[0], c[1]),
                          pf::domination(c[0], c[1]), c[0].rank == c[1].rank ? "pair" : c[0].suit == c[1].suit ? "suited" : "offsuit", g);
            DrawTextEx(font, buf, {cx, y + 58}, 16, 0, GREY);
        } else {
            DrawTextEx(font, "Hover a hand for exact values", {cx, y + 8}, 16, 0, GREY);
        }

        DrawTextEx(font, "Uses the loaded strategy's realization model. Representative cards: suited = one suit, others = two suits.",
                   {IX, H - 30}, 14, 0, {110, 114, 122, 255});
        return !(button({W - 200, H - 52, 176, 36}, "Back (Esc)", false, 15) || IsKeyPressed(KEY_ESCAPE));
    }
};

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
    // Chart modes: the charts written over a chart-locked solve (uncharted spots best-respond to the charts)
    // if that file exists, else over the plain solve. Re-applying the charts gives the mask and painter edits.
    std::string app = GetApplicationDirectory();
    Mode modes[3] = {{"Solver", "", "", strat}, {"GTO", app + "../ref", app + "../data/strategy_gto.bin", strat},
                     {"PTO", app + "../ref/pto", app + "../data/strategy_pto.bin", strat}};
    auto loadCharts = [&](Mode& m) {  // keeps the previous strategy if the charts don't load
        pf::Strategy c = strat;
        bool solved = c.load(m.bin);  // unchanged on failure
        std::vector<uint8_t> l;
        std::string err;
        int n = pf::applyCharts(c, m.dir, &l, &err);
        if (n <= 0) return (void)std::fprintf(stderr, "%s: charts from %s not loaded: %s\n", m.name.c_str(), m.dir.c_str(), n < 0 ? err.c_str() : "no charts");
        m.st = std::move(c), m.locked = std::move(l);
        m.fallback = solved ? "no chart, chart-locked solve" : "no chart, solver fallback";
        std::fprintf(stderr, "%s: %d charts from %s over %s\n", m.name.c_str(), n, m.dir.c_str(), solved ? m.bin.c_str() : "the solve");
    };
    loadCharts(modes[1]);
    loadCharts(modes[2]);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    InitWindow(W, H, "Preflop");
    SetExitKey(KEY_NULL);  // Esc closes panels instead
    SetTargetFPS(60);
    // Layout is in W x H units; the window is 80% of the screen height at the same aspect ratio.
    const float scale = 0.8f * GetMonitorHeight(GetCurrentMonitor()) / H;
    SetWindowSize(int(W * scale), int(H * scale));
    SetWindowPosition((GetMonitorWidth(GetCurrentMonitor()) - int(W * scale)) / 2,
                      (GetMonitorHeight(GetCurrentMonitor()) - int(H * scale)) / 2);
    SetMouseScale(1 / scale, 1 / scale);  // GetMousePosition() in layout units
    const Camera2D zoom{{0, 0}, {0, 0}, 0, scale};

    auto loadFont = [](const char* path) {
        if (!FileExists(path)) return GetFontDefault();
        std::vector<int> cps;  // printable ASCII plus the "±" of the EV display
        for (int c = 32; c < 127; c++) cps.push_back(c);
        cps.push_back(0xB1);
        Font f = LoadFontEx(path, 64, cps.data(), int(cps.size()));
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
    int pick = 0, mode = 0;            // modes[] index picked with T / used by the current hand (switches at newHand)
    const pf::Strategy* cur = &strat;  // modes[mode].st: bots, rollouts, grading, ranges
    pf::Stats stats[3];                // per mode
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
        auto [ev, rs] = d.ev.get();
        d.rs = rs, d.lowSample = rs.ess < rc.minEss;
        d.fb = pf::grade(*cur, *d.sp, ev, d.act, {}, &d.rs);
        // On a charted hand the chart decides: correct iff it plays the choice at least 10% of the time.
        auto& c = d.sp->hand.cards[d.sp->user];
        if (modes[mode].charted(d.node, pf::bucket(c[0], c[1]))) d.fb->correct = d.fb->mix[int(d.act)] >= pf::GradeConfig{}.minFreq;
        if (!std::isnan(d.fb->evLoss)) stats[mode].add(*d.sp, *d.fb);
    };

    std::optional<pf::Act> next;  // bot's action, picked when its turn starts so the think time can depend on it
    auto startTurn = [&] {
        clock = 0;
        next.reset();
    };
    auto newHand = [&] {
        for (auto& d : log) collect(d, true);
        log.clear();
        mode = pick;
        cur = &modes[mode].st;
        rc.rc = cur->realization();
        pf::SpotFilter f = filter;
        if (f.type == pf::SpotType::NSPOT && f.pos < 0) f.pos = posOf(HERO, (btn + 1) % pf::NPOS);  // normal rotation
        auto sp = pf::deal(*cur, f, rng);
        notice = sp ? "" : "No such spot found, dealing any spot";
        if (!sp) sp = pf::deal(*cur, {}, rng);
        btn = (pf::BTN - sp->user + pf::NPOS) % pf::NPOS;  // puts the user's position in the hero seat
        hand.emplace(rng, cur->config());
        hand->cards = sp->hand.cards;
        replay.clear();
        for (auto& st : sp->path) replay.push_back(st.act);
        replayAt = 0;
        for (auto& l : label) l.clear();
        node = cur->root();
        view = -1;
        reviewed = false;
        startTurn();
    };
    auto act = [&](pf::Act a, bool graded) {
        int p = hand->toAct();
        bool allIn = a == pf::Act::Raise && hand->raiseTo() >= hand->committed(p) + hand->stack(p) - 1e-9;
        label[p] = a == pf::Act::Fold ? "Fold" : a == pf::Act::Call ? "Call" : allIn ? "All-in" : "Raise";
        Decision d{node, p, a, label[p], {}, {}, {}, {}, {}, false};
        std::vector<pf::Step> path;
        for (auto& e : log) {
            path.push_back({e.node, e.act});
            if (e.act != pf::Act::Fold) d.spot += (d.spot.empty() ? "" : ", ") + std::string(POS_NAME[e.pos]) + " " + e.did;
        }
        if (a != pf::Act::Fold)
            d.did += " " + fmt(a == pf::Act::Call ? std::min(hand->currentBet(), hand->committed(p) + hand->stack(p)) : hand->raiseTo());
        if (graded) {
            d.sp = pf::Spot{*hand, p, node, path, pf::classify(*cur, path, p)};
            d.ev = std::async(std::launch::async, [&s = *cur, &eq, rc, sp = *d.sp, seed = rng()] {
                std::mt19937 r(seed);  // each worker owns its rng
                pf::RolloutStats st;
                auto ev = pf::rollout(s, eq, sp, r, rc, &st);
                return std::pair{ev, st};
            });
        }
        log.push_back(std::move(d));
        node = cur->child(node, a);
        hand->apply(a);
        startTurn();
    };
    auto botPick = [&] {
        const auto& c = hand->cards[hand->toAct()];
        const auto& p = cur->probs(node, pf::bucket(c[0], c[1]));
        next = replayAt < replay.size() ? replay[replayAt++] : pf::Act(std::discrete_distribution<int>(p.begin(), p.end())(rng));
        // Human-like right-skewed think time: log-normal, median 1.3s, mostly 0.6-3s, capped like a time bank.
        // Clear folds (strategy folds the hand ~always) are usually pre-folded, fast-fold style.
        // Deliberately independent of hand strength/difficulty so timing gives no tells.
        if (*next == pf::Act::Fold && p[0] > .99f && uni(0, 1) < .7) thinkLen = .1;
        else thinkLen = std::clamp(std::lognormal_distribution<double>(std::log(1.3), .5)(rng), .3, 12.0);
    };

    newHand();
    std::optional<Painter> painter;  // reference painter screen; the table is paused while it's open
    std::optional<Inspector> inspector;  // R inspector screen

    while (!WindowShouldClose()) {
        if (inspector || painter) {  // full-screen tools; the table is paused underneath
            BeginDrawing();
            BeginMode2D(zoom);
            if (inspector) {
                if (!inspector->frame()) inspector.reset();
            } else if (!painter->frame()) {
                painter.reset();
                for (auto& d : log) collect(d, true);  // rollouts may read modes[1].st; let them finish first
                loadCharts(modes[1]);
            } else if (painter->inspect) {
                painter->inspect = false;
                inspector.emplace(strat);
                inspector->fromSpot(painter->hist);
            }
            EndMode2D();
            EndDrawing();
            continue;
        }
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
            if (IsKeyPressed(KEY_I)) {
                std::vector<pf::Act> hist;
                for (int i = 0; i < view; i++) hist.push_back(log[i].act);
                inspector.emplace(strat);
                inspector->fromSpot(hist);
            }
            if (close || (click && (CheckCollisionPointRec(mouse, CLOSE_BTN) || !CheckCollisionPointRec(mouse, PANEL)))) {
                view = -1;
                if (hand->done()) newHand();
            }
            turn = hand->toAct();
        } else if (IsKeyPressed(KEY_P)) {
            menu = 1;
        } else if (IsKeyPressed(KEY_S)) {
            menu = 2;
        } else if (IsKeyPressed(KEY_I)) {
            inspector.emplace(strat);
        } else if (IsKeyPressed(KEY_T)) {
            int was = pick;
            do pick = (pick + 1) % 3;
            while (pick && modes[pick].locked.empty());  // skip chart modes without charts
            if (pick == was) notice = "No charts loaded (see terminal)";
        } else if (IsKeyPressed(KEY_E)) {
            painter.emplace(strat, modes[1].dir);  // edits the GTO charts
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
            if (!next) botPick();
            if (clock >= (hand->folded(hero) ? .2 : thinkLen)) act(*next, false);  // no waiting once the user is out
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
        BeginMode2D(zoom);
        DrawRectangleGradientV(0, 0, W, H, {28, 31, 37, 255}, {12, 13, 16, 255});

        // table: dark rim, green felt, faint inner line
        DrawRectangleRounded({150, 135, 980, 450}, 1, 64, {44, 47, 54, 255});
        DrawRectangleRounded({160, 145, 960, 430}, 1, 64, {20, 22, 26, 255});
        DrawRectangleRounded({176, 161, 928, 398}, 1, 64, {24, 104, 72, 255});
        DrawRectangleRounded({300, 220, 680, 280}, 1, 64, {30, 118, 82, 255});
        DrawRectangleRoundedLinesEx({196, 181, 888, 358}, 1, 64, 2, {255, 255, 255, 22});
        if (!hand->done()) text(bold, "PREFLOP", {640, 400}, 34, {255, 255, 255, 24});
        double raked = hand->done() ? pf::rake(*hand) : 0;  // no flop, no drop
        text(bold, "Pot: " + fmt(hand->pot()) + (raked > 0 ? "   (rake " + fmt(raked) + ", " + fmt(hand->pot() - raked) + " after rake)" : ""),
             {640, 335}, 22, WHITE);

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
                         : std::to_string(alive) + (pf::allIn(*hand) ? " players all-in" : " players to the flop");
            DrawRectangleRounded({490, 360, 300, 40}, 1, 12, {0, 0, 0, 150});
            text(bold, msg, {640, 380}, 20, WHITE);
        }

        // top bar: spot picker, stats, current drill
        if (!menu && view < 0) {
            if (button({16, 16, 110, 34}, "Spots (P)", false, 15)) menu = 1;
            if (button({134, 16, 110, 34}, "Stats (S)", false, 15)) menu = 2;
            if (button({252, 16, 130, 34}, "Painter (E)", false, 15))
                painter.emplace(strat, modes[1].dir);  // edits the GTO charts
            if (button({390, 16, 100, 34}, "R (I)", false, 15)) inspector.emplace(strat);
        }
        {  // strategy badge, top right: the current hand's mode, and the next hand's if T changed it
            std::string m = modes[mode].name + (pick != mode ? " -> " + modes[pick].name : "") + "  (T)";
            const Color MODE_COL[] = {{52, 56, 64, 255}, {46, 110, 160, 255}, {120, 84, 160, 255}};
            float w = MeasureTextEx(bold, m.c_str(), 15, 0).x + 24;
            DrawRectangleRounded({W - 16 - w, 16, w, 34}, .5f, 8, MODE_COL[mode]);
            text(bold, m, {W - 16 - w / 2, 33}, 15, WHITE);
            bool heroTurn = !hand->done() && turn == hero && replayAt >= replay.size();
            if (heroTurn && mode && !modes[mode].charted(node, pf::bucket(hand->cards[hero][0], hand->cards[hero][1]))) {
                float fw = MeasureTextEx(bold, modes[mode].fallback.c_str(), 14, 0).x;
                text(bold, modes[mode].fallback, {W - 16 - fw / 2, 62}, 14, {230, 160, 60, 255});
            }
        }
        std::string drill = "Drill: " + std::string(pf::spotName(filter.type)) + ", " + POS_PICK[filter.pos + 1];
        DrawTextEx(font, (drill + (notice.empty() ? "" : "   (" + notice + ")")).c_str(), {502, 25}, 15, 0, GREY);
        DrawTextEx(font, "Click a player to see their range  |  H: your range", {16, H - 26}, 14, 0, {110, 114, 122, 255});

        if (view >= 0) {
            const Decision& dv = log[view];
            std::vector<pf::Act> hist;
            for (int i = 0; i < view; i++) hist.push_back(log[i].act);
            std::string title = std::string(SEAT_NAME[seatOf(dv.pos, btn)]) + " - " + POS_NAME[dv.pos];
            if (dv.sp) title += "  (" + std::string(pf::spotName(dv.sp->type)) + ")";
            rangePanel(modes[mode], dv, cur->reach(hist), title, dv.pos == hero ? pf::bucket(hand->cards[hero][0], hand->cards[hero][1]) : -1,
                       hand->done());
        }
        if (menu == 1 && pickerPanel(filter)) menu = 0;
        if (menu == 2 && statsPanel(stats[mode], ("Stats - " + modes[mode].name).c_str())) menu = 0;

        EndMode2D();
        EndDrawing();
    }
    CloseWindow();
}
