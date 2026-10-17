// GGPoker-style 6-max preflop table. Random bots with human-like think times.
#include "raylib.h"
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

int posOf(int seat, int btn) { return (seat - btn + pf::BTN + pf::NPOS) % pf::NPOS; }
int seatOf(int pos, int btn) { return (pos - pf::BTN + btn + pf::NPOS) % pf::NPOS; }

struct Button {
    Rectangle r;
    pf::Act act;
    const char* title;
    std::string amount;
    Color col;
    KeyboardKey key;
};

}  // namespace

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    InitWindow(W, H, "Preflop");
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

    auto startTurn = [&] {
        clock = 0;
        // ponytail: flat think-time distribution with 10% tanks; per-spot timing if it ever matters
        thinkLen = uni(0, 1) < .1 ? uni(4, 6) : uni(.5, 3);
    };
    auto newHand = [&] {
        btn = (btn + 1) % pf::NPOS;
        hand.emplace(rng);
        for (auto& l : label) l.clear();
        startTurn();
    };
    auto act = [&](pf::Act a) {
        int p = hand->toAct();
        bool allIn = a == pf::Act::Raise && hand->raiseTo() >= hand->committed(p) + hand->stack(p) - 1e-9;
        label[p] = a == pf::Act::Fold ? "Fold" : a == pf::Act::Call ? "Call" : allIn ? "All-in" : "Raise";
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
        clock += GetFrameTime();
        int hero = posOf(HERO, btn);
        int turn = hand->toAct();

        // --- update ---
        std::vector<Button> buttons;
        if (hand->done()) {
            if (clock > 2.0) newHand();
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

        if (hand->done()) {
            int alive = 0, winner = -1;
            for (int p = 0; p < pf::NPOS; p++)
                if (!hand->folded(p)) alive++, winner = p;
            std::string msg = alive == 1 ? std::string(SEAT_NAME[seatOf(winner, btn)]) + " wins " + fmt(hand->pot()) + " BB"
                                         : std::to_string(alive) + " players to the flop";
            DrawRectangleRounded({490, 360, 300, 40}, 1, 12, {0, 0, 0, 150});
            text(bold, msg, {640, 380}, 20, WHITE);
        }

        EndDrawing();
    }
    CloseWindow();
}
