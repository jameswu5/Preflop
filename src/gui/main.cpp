// GGPoker-style 6-max preflop table. Random bots with human-like think times.
// Click a player (or press H for yourself) to see the strategy grid at their last decision.
#include "raylib.h"
#include "solver/cards.h"
#include "solver/strategy.h"
#include "trainer/game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    std::string did;   // "Raise 2.5", "Call 2.5", "Fold", "All-in 100"
    std::string spot;  // non-fold actions before it, e.g. "UTG Raise 2.5, HJ Call 2.5"
};

// Grid/legend colours per pf::Act (Fold, Call, Raise).
const Color ACT_COL[] = {{58, 98, 150, 255}, {46, 140, 92, 255}, {196, 82, 40, 255}};
const char* ACT_NAME[] = {"Fold", "Call", "Raise"};
constexpr Rectangle PANEL = {190, 60, 900, 680};
constexpr Rectangle CLOSE_BTN = {PANEL.x + PANEL.width - 224, PANEL.y + PANEL.height - 72, 200, 48};
constexpr float CELL = 42, GX = PANEL.x + 24, GY = PANEL.y + 88;

int combos(int b) { return b / 13 == b % 13 ? 6 : b / 13 < b % 13 ? 4 : 12; }

void actRows(const std::array<float, 3>& p, float x, float y) {  // "Raise 33.3%" lines, aggressive first
    for (int a = 2; a >= 0; a--, y += 24) {
        DrawRectangleRounded({x, y + 3, 14, 14}, .3f, 4, ACT_COL[a]);
        DrawTextEx(font, ACT_NAME[a], {x + 22, y}, 18, 0, {200, 204, 210, 255});
        char b[16];
        std::snprintf(b, sizeof b, "%.1f%%", p[a] * 100);
        DrawTextEx(bold, b, {x + 110, y}, 18, 0, WHITE);
    }
}

// 13x13 strategy grid for one decision. highlight = hero's bucket, -1 for bots (cards hidden).
void rangePanel(const pf::Strategy& st, const Decision& d, const std::string& who, int highlight, bool nextHand) {
    DrawRectangle(0, 0, W, H, {0, 0, 0, 140});
    DrawRectangleRounded(PANEL, .03f, 12, {24, 26, 31, 252});
    DrawTextEx(bold, (who + " - " + POS_NAME[d.pos]).c_str(), {GX, PANEL.y + 20}, 26, 0, WHITE);
    DrawTextEx(font, (d.spot.empty() ? "Unopened pot" : d.spot).c_str(), {GX, PANEL.y + 54}, 17, 0, {150, 154, 162, 255});

    Vector2 m = GetMousePosition();
    int hover = -1;
    for (int b = 0; b < pf::NBUCKETS; b++) {
        Rectangle c = {GX + b % 13 * CELL, GY + b / 13 * CELL, CELL - 2, CELL - 2};
        if (CheckCollisionPointRec(m, c)) hover = b;
        auto& p = st.probs(d.node, b);
        float x = c.x;
        for (int a = 2; a >= 0; a--) {  // raise | call | fold, left to right
            DrawRectangleRec({x, c.y, c.width * p[a], c.height}, ACT_COL[a]);
            x += c.width * p[a];
        }
        std::string n = pf::bucketName(b);
        text(bold, n, {c.x + c.width / 2 + 1, c.y + c.height / 2 + 1}, 14, {0, 0, 0, 120});
        text(bold, n, {c.x + c.width / 2, c.y + c.height / 2}, 14, WHITE);
    }
    auto outline = [](int b, Color col) {
        DrawRectangleLinesEx({GX + b % 13 * CELL - 2, GY + b / 13 * CELL - 2, CELL + 2, CELL + 2}, 3, col);
    };
    if (highlight >= 0) outline(highlight, {240, 196, 64, 255});
    if (hover >= 0) outline(hover, WHITE);

    // right column: action taken, hovered (or own) hand, range widths by combos
    float x = GX + 13 * CELL + 28, y = GY;
    DrawTextEx(font, "Action", {x, y}, 15, 0, {130, 134, 142, 255});
    DrawTextEx(bold, d.did.c_str(), {x, y + 18}, 22, 0, WHITE);
    y += 66;
    int shown = hover >= 0 ? hover : highlight;
    if (shown >= 0) {
        DrawTextEx(font, hover >= 0 ? "Hand" : "Your hand", {x, y}, 15, 0, {130, 134, 142, 255});
        DrawTextEx(bold, pf::bucketName(shown).c_str(), {x, y + 18}, 26, 0, WHITE);
        actRows(st.probs(d.node, shown), x, y + 54);
    } else {
        DrawTextEx(font, "Hover a hand for its mix", {x, y + 18}, 16, 0, {130, 134, 142, 255});
    }
    y += 150;
    std::array<float, 3> width{};
    for (int b = 0; b < pf::NBUCKETS; b++)
        for (int a = 0; a < 3; a++) width[a] += combos(b) * st.probs(d.node, b)[a] / float(pf::NCOMBOS);
    DrawTextEx(font, "Whole range", {x, y}, 15, 0, {130, 134, 142, 255});
    float bx = x;
    for (int a = 2; a >= 0; a--) {
        DrawRectangleRec({bx, y + 24, 260 * width[a], 12}, ACT_COL[a]);
        bx += 260 * width[a];
    }
    actRows(width, x, y + 48);

    bool hov = CheckCollisionPointRec(m, CLOSE_BTN);
    DrawRectangleRounded(CLOSE_BTN, .3f, 12, hov ? Color{70, 74, 84, 255} : Color{52, 56, 64, 255});
    text(bold, nextHand ? "Next hand  (Space)" : "Close  (Esc)", {CLOSE_BTN.x + 100, CLOSE_BTN.y + 24}, 18, WHITE);
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
    pf::Strategy strat;  // uniform dummy until a solved file is passed: preflop <strategy file>
    if (argc > 1 && !strat.load(argv[1])) std::fprintf(stderr, "could not load %s, using uniform strategy\n", argv[1]);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    InitWindow(W, H, "Preflop");
    SetExitKey(KEY_NULL);  // Esc closes the range panel instead
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
    int node = 0;                // strategy node of the current state, -1 once off the tree
    std::vector<Decision> log;   // this hand's decisions
    int view = -1;               // index into log shown in the range panel, -1 = closed
    bool reviewed = false;       // hero's end-of-hand feedback already shown

    auto lastBy = [&](int pos) {
        for (int i = int(log.size()) - 1; i >= 0; i--)
            if (log[i].pos == pos && log[i].node >= 0) return i;
        return -1;
    };

    auto startTurn = [&] {
        clock = 0;
        // ponytail: flat think-time distribution with 8% tanks; per-spot timing if it ever matters
        thinkLen = uni(0, 1) < .08 ? uni(2.5, 4) : uni(.3, 1.5);
    };
    auto newHand = [&] {
        btn = (btn + 1) % pf::NPOS;
        hand.emplace(rng, strat.config());
        for (auto& l : label) l.clear();
        node = strat.root();
        log.clear();
        view = -1;
        reviewed = false;
        startTurn();
    };
    auto act = [&](pf::Act a) {
        int p = hand->toAct();
        bool allIn = a == pf::Act::Raise && hand->raiseTo() >= hand->committed(p) + hand->stack(p) - 1e-9;
        label[p] = a == pf::Act::Fold ? "Fold" : a == pf::Act::Call ? "Call" : allIn ? "All-in" : "Raise";
        std::string spot;
        for (auto& d : log)
            if (d.did != "Fold") spot += (spot.empty() ? "" : ", ") + std::string(POS_NAME[d.pos]) + " " + d.did;
        std::string amt = a == pf::Act::Fold ? "" : " " + fmt(a == pf::Act::Call ? std::min(hand->currentBet(), hand->committed(p) + hand->stack(p)) : hand->raiseTo());
        log.push_back({node, p, label[p] + amt, spot});
        node = node >= 0 ? strat.child(node, a) : -1;
        hand->apply(a);
        startTurn();
    };
    auto botAct = [&] {
        auto legal = hand->legal();
        std::vector<double> w;
        for (auto a : legal) w.push_back(a == pf::Act::Fold ? 60 : a == pf::Act::Call ? 25 : 15);
        act(legal[std::discrete_distribution<int>(w.begin(), w.end())(rng)]);
    };

    newHand();

    while (!WindowShouldClose()) {
        int hero = posOf(HERO, btn);
        int turn = hand->toAct();
        Vector2 mouse = GetMousePosition();
        bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

        // --- update ---
        std::vector<Button> buttons;
        if (view >= 0) {  // panel open: game paused
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ESCAPE) ||
                (click && (CheckCollisionPointRec(mouse, CLOSE_BTN) || !CheckCollisionPointRec(mouse, PANEL)))) {
                view = -1;
                if (hand->done()) newHand();
            }
            turn = hand->toAct();
        } else if (int s = click ? seatAt(mouse) : -1; s >= 0 && lastBy(posOf(s, btn)) >= 0) {
            view = lastBy(posOf(s, btn));
        } else if (IsKeyPressed(KEY_H) && lastBy(hero) >= 0) {
            view = lastBy(hero);
        } else if (clock += GetFrameTime(); hand->done()) {
            if (!reviewed && lastBy(hero) >= 0) {  // hero's feedback before the next hand
                view = lastBy(hero);
                reviewed = true;
            } else if (clock > 2.0) {
                newHand();
            }
        } else if (turn != hero) {
            if (clock >= thinkLen) botAct();
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
                if (IsKeyPressed(b.key) ||
                    (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), b.r))) {
                    act(b.act);
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
            bool hover = CheckCollisionPointRec(GetMousePosition(), b.r);
            DrawRectangleRounded(b.r, .3f, 12, hover ? ColorBrightness(b.col, .15f) : b.col);
            if (b.amount.empty()) {
                text(bold, b.title, {b.r.x + b.r.width / 2, b.r.y + b.r.height / 2}, 22, WHITE);
            } else {
                text(font, b.title, {b.r.x + b.r.width / 2, b.r.y + 22}, 17, {255, 255, 255, 220});
                text(bold, b.amount, {b.r.x + b.r.width / 2, b.r.y + 44}, 22, WHITE);
            }
        }
        if (!buttons.empty()) text(font, "F / C / R", {1260 - 3 * 150 / 2.f - 12, 790}, 13, {130, 134, 142, 255});

        if (hand->done() && view < 0) {
            int alive = 0, winner = -1;
            for (int p = 0; p < pf::NPOS; p++)
                if (!hand->folded(p)) alive++, winner = p;
            std::string msg = alive == 1 ? std::string(SEAT_NAME[seatOf(winner, btn)]) + " wins " + fmt(hand->pot()) + " BB"
                                         : std::to_string(alive) + " players to the flop";
            DrawRectangleRounded({490, 360, 300, 40}, 1, 12, {0, 0, 0, 150});
            text(bold, msg, {640, 380}, 20, WHITE);
        }

        DrawTextEx(font, "Click a player to see their range  |  H: your range", {16, H - 26}, 14, 0, {110, 114, 122, 255});
        if (view >= 0) {
            const Decision& d = log[view];
            bool mine = d.pos == hero;
            rangePanel(strat, d, SEAT_NAME[seatOf(d.pos, btn)], mine ? pf::bucket(hand->cards[hero][0], hand->cards[hero][1]) : -1,
                       hand->done());
        }

        EndDrawing();
    }
    CloseWindow();
}
