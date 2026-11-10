// NHL Sim viewer: pick two teams, watch the match in 2D or 3D, read the box score.
//
// Controls
//   Menu:   Up/Down choose team, Left/Right switch Away/Home column, R random matchup, Enter start
//   Game:   Space pause, 1-5 speed (1x 2x 4x 8x 30x), N names, B box score, F finish instantly, Esc menu
//           V 2D/3D view, C cycle 3D camera (broadcast, wide, end zone), mouse wheel zoom (3D)
//   Final:  Enter or Esc back to menu
//
// Extra flags (handy for thumbnails / headless checks):
//   nhl_sim --data DIR --home EDM --away COL --screenshot out.png --after 600   (sim seconds)
//           --view 2d|3d|wide|end
//   nhl_sim --home EDM --away COL --script "2:v,4:shot,5:c,7:shot,9:z+3,10:shot,12:quit" [--script-out PREFIX]
//           plays a live game and fires game-screen actions at wall-clock seconds: v (2D/3D), c (camera),
//           n (names), z+N / z-N (mouse-wheel notches), s1-s5 (speed), shot (PREFIX_<k>.png), quit
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "core/Game.h"
#include "core/League.h"

using namespace hk;

namespace {

constexpr int kW = 1280, kH = 760;

const Color kIce{238, 243, 247, 255};
const Color kBg{18, 22, 30, 255};
const Color kPanel{28, 34, 46, 255};
const Color kPanelHi{40, 48, 64, 255};
const Color kText{232, 236, 242, 255};
const Color kDim{140, 150, 168, 255};
const Color kRed{200, 32, 44, 255};
const Color kBlue{30, 76, 170, 255};
const Color kCrease{150, 196, 236, 255};
const Color kGold{255, 196, 64, 255};

Color toColor(Color3 c, unsigned char a = 255) { return Color{c.r, c.g, c.b, a}; }

// The default raylib font is ASCII-only; fold common accented letters.
std::string ascii(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out += char(c); ++i; continue; }
        unsigned cp = 0;
        int n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
        cp = c & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
        for (int k = 1; k < n && i + size_t(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3F);
        i += size_t(n);
        struct M { unsigned cp; char a; };
        static const M map[] = {
            {0xE0, 'a'}, {0xE1, 'a'}, {0xE2, 'a'}, {0xE3, 'a'}, {0xE4, 'a'}, {0xE5, 'a'}, {0xE7, 'c'}, {0xE8, 'e'}, {0xE9, 'e'}, {0xEA, 'e'},
            {0xEB, 'e'}, {0xEC, 'i'}, {0xED, 'i'}, {0xEE, 'i'}, {0xEF, 'i'}, {0xF1, 'n'}, {0xF2, 'o'}, {0xF3, 'o'}, {0xF4, 'o'}, {0xF5, 'o'},
            {0xF6, 'o'}, {0xF8, 'o'}, {0xF9, 'u'}, {0xFA, 'u'}, {0xFB, 'u'}, {0xFC, 'u'}, {0xFD, 'y'}, {0xC4, 'A'}, {0xC5, 'A'}, {0xC9, 'E'},
            {0xD6, 'O'}, {0xD8, 'O'}, {0xDC, 'U'}, {0x10D, 'c'}, {0x10C, 'C'}, {0x107, 'c'}, {0x11B, 'e'}, {0x148, 'n'}, {0x159, 'r'},
            {0x161, 's'}, {0x160, 'S'}, {0x17E, 'z'}, {0x17D, 'Z'}, {0x142, 'l'}, {0x141, 'L'}, {0x15B, 's'}, {0x144, 'n'}, {0x16F, 'u'},
            {0x165, 't'}, {0x10F, 'd'}, {0x13E, 'l'}, {0x17C, 'z'}, {0x17A, 'z'}, {0x103, 'a'}, {0x219, 's'}, {0x21B, 't'}, {0x11F, 'g'},
        };
        char rep = '?';
        for (const auto& m : map) if (m.cp == cp) { rep = m.a; break; }
        out += rep;
    }
    return out;
}

std::string lastName(const std::string& full) {
    std::string a = ascii(full);
    if (a.rfind("Depth ", 0) == 0) return a.substr(6);
    auto sp = a.find(' ');
    return sp == std::string::npos ? a : a.substr(sp + 1);
}

std::string clockText(float s) {
    int t = int(std::ceil(std::max(0.f, s)));
    return TextFormat("%d:%02d", t / 60, t % 60);
}

void text(const std::string& s, float x, float y, int size, Color c) { DrawText(s.c_str(), int(x), int(y), size, c); }
void textRight(const std::string& s, float x, float y, int size, Color c) { DrawText(s.c_str(), int(x) - MeasureText(s.c_str(), size), int(y), size, c); }
void textCenter(const std::string& s, float x, float y, int size, Color c) { DrawText(s.c_str(), int(x) - MeasureText(s.c_str(), size) / 2, int(y), size, c); }

// ------------------------------------------------------------------ rink ----

struct View {
    float scale = 5.65f;
    Vector2 origin{kW / 2.f, 316.f};
    Vector2 S(Vec2 p) const { return {origin.x + p.x * scale, origin.y - p.y * scale}; }
    float L(float feet) const { return feet * scale; }
};

void drawRink(const View& v, const Game* g) {
    using namespace rink;
    Rectangle r{v.origin.x - v.L(kHalfLength), v.origin.y - v.L(kHalfWidth), v.L(2 * kHalfLength), v.L(2 * kHalfWidth)};
    float round = kCornerRadius / kHalfWidth;
    DrawRectangleRounded({r.x - 6, r.y - 6, r.width + 12, r.height + 12}, round, 24, Color{60, 66, 78, 255});
    DrawRectangleRounded(r, round, 24, kIce);

    // Center line and blue lines.
    float gy = kHalfWidth;
    DrawRectangleV(v.S({-0.5f, gy}), {v.L(1.f), v.L(2 * gy)}, kRed);
    for (float sx : {-1.f, 1.f}) DrawRectangleV(v.S({sx * kBlueLineX - 0.5f, gy}), {v.L(1.f), v.L(2 * gy)}, kBlue);
    // Goal lines stop at the curved boards.
    float gl = (kHalfWidth - kCornerRadius) + std::sqrt(kCornerRadius * kCornerRadius - std::pow(kGoalLineX - (kHalfLength - kCornerRadius), 2.f));
    for (float sx : {-1.f, 1.f}) DrawLineEx(v.S({sx * kGoalLineX, gl}), v.S({sx * kGoalLineX, -gl}), 2.f, kRed);

    // Faceoff circles and dots.
    DrawRing(v.S({0, 0}), v.L(15.f) - 2, v.L(15.f), 0, 360, 64, kBlue);
    DrawCircleV(v.S({0, 0}), v.L(0.6f), kBlue);
    for (float sx : {-1.f, 1.f})
        for (float sy : {-1.f, 1.f}) {
            DrawRing(v.S({sx * kZoneDotX, sy * kDotY}), v.L(15.f) - 2, v.L(15.f), 0, 360, 64, kRed);
            DrawCircleV(v.S({sx * kZoneDotX, sy * kDotY}), v.L(1.f), kRed);
            DrawCircleV(v.S({sx * kNeutralDotX, sy * kDotY}), v.L(1.f), kRed);
        }

    // Creases and nets.
    for (float sx : {-1.f, 1.f}) {
        Vector2 c = v.S({sx * kGoalLineX, 0});
        float a0 = sx > 0 ? 90.f : -90.f;
        DrawCircleSector(c, v.L(kCreaseRadius), a0, a0 + 180.f, 32, Fade(kCrease, 0.7f));
        Rectangle net = sx > 0 ? Rectangle{c.x, c.y - v.L(kNetHalfWidth), v.L(kNetDepth), v.L(2 * kNetHalfWidth)}
                               : Rectangle{c.x - v.L(kNetDepth), c.y - v.L(kNetHalfWidth), v.L(kNetDepth), v.L(2 * kNetHalfWidth)};
        DrawRectangleRec(net, Color{255, 255, 255, 255});
        DrawRectangleLinesEx(net, 2.f, kRed);
    }

    // Home team mark at center ice.
    if (g) {
        const char* abbr = g->team(0).info.abbr;
        int fs = int(v.L(7.f));
        textCenter(abbr, v.origin.x, v.origin.y - fs / 2.f, fs, Fade(toColor(g->team(0).info.primary), 0.18f));
    }
    DrawRectangleRoundedLinesEx(r, round, 24, 3.f, Color{90, 98, 112, 255});
}

// ------------------------------------------------------------------ app -----

enum class Screen { Menu, Play, Final };

struct App {
    League league;
    Screen screen = Screen::Menu;
    int pick[2] = {0, 1};  // [0] home, [1] away
    int column = 1;        // menu focus: 1 = away, 0 = home
    std::unique_ptr<Game> game;
    float speed = 4.f;
    bool paused = false, names = true, showBox = false;
    size_t seenEvents = 0;
    float bannerTime = 0;
    std::string banner;
    Color bannerColor = kText;
    std::deque<Vec2> trail;
    View view;
    bool view3d = false;
    int cam = 0;  // 3D camera: 0 broadcast, 1 wide, 2 end zone
    float zoom = 1.f, endSide = 1.f;
    bool camSnap = true;
    Camera3D camera{};
};

void startGame(App& a) {
    GameOptions o;
    o.seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    o.intermission = 2.5f;
    a.game = std::make_unique<Game>(a.league, a.pick[0], a.pick[1], o);
    a.screen = Screen::Play;
    a.paused = false;
    a.showBox = false;
    a.seenEvents = 0;
    a.banner.clear();
    a.bannerTime = 0;
    a.trail.clear();
    a.camSnap = true;
}

// Home wears its primary color; away wears white with primary trim, like the NHL.
Color jerseyFill(const Game& g, int side) { return side == 0 ? toColor(g.team(0).info.primary) : Color{250, 250, 250, 255}; }
Color jerseyTrim(const Game& g, int side) { return side == 0 ? toColor(g.team(0).info.secondary) : toColor(g.team(1).info.primary); }

void drawBodies(const App& a) {
    const Game& g = *a.game;
    const View& v = a.view;
    const Puck& pk = g.puck();
    for (int i = 0; i < 12; ++i) {
        const Body& b = g.bodies()[size_t(i)];
        if (!b.active) continue;
        Vector2 p = v.S(b.pos);
        float rad = v.L(b.isGoalie() ? 1.9f : 1.5f);
        Color fill = jerseyFill(g, b.team), trim = jerseyTrim(g, b.team);
        if (pk.carrier == i) DrawCircleV(p, rad + 5, Fade(kGold, 0.55f));
        DrawCircleV(p, rad + 2, trim);
        DrawCircleV(p, rad, fill);
        Vector2 stick = v.S(b.pos + b.face * 2.3f);
        DrawLineEx(p, stick, 2.f, Color{40, 40, 40, 200});
        if (b.isGoalie()) textCenter("G", p.x, p.y - 5, 10, trim);
        if (b.stun > 0) DrawRing(p, rad + 3, rad + 5, 0, 360, 24, Fade(kRed, 0.8f));
        if (a.names) {
            std::string n = lastName(g.player(b.team, b.roster).name);
            int w = MeasureText(n.c_str(), 10);
            DrawRectangle(int(p.x) - w / 2 - 2, int(p.y - rad - 15), w + 4, 12, Fade(kBg, 0.55f));
            textCenter(n, p.x, p.y - rad - 14, 10, kText);
        }
    }
    // Puck and its trail.
    for (size_t k = 1; k < a.trail.size(); ++k)
        DrawLineEx(v.S(a.trail[k - 1]), v.S(a.trail[k]), 2.f, Fade(Color{20, 20, 20, 255}, float(k) / float(a.trail.size()) * 0.5f));
    DrawCircleV(v.S(pk.pos), 4.f, Color{10, 10, 10, 255});
}

// --------------------------------------------------------------- 3D view ----
// Same game state, drawn in 3D: sim (x, y) on the ice becomes world (x, height, -y), in feet.
// Flat things (ice, markings, shadows) are painted in order with the depth test off, which avoids
// z-fighting at broadcast distances; everything that stands up is drawn after with depth on.

const char* kCamNames[3] = {"BROADCAST", "WIDE", "END ZONE"};
constexpr float kViewTop = 60, kViewBottom = 572;  // the strip between the top bar and the bottom panel
constexpr float kViewMid = (kViewTop + kViewBottom) / 2, kViewHalf = (kViewBottom - kViewTop) / 2;

// Like BeginMode3D, but with an off-centre frustum so the camera looks at the middle of the visible strip
// (not the middle of the window) and fovy spans that strip.
void beginStripMode3D(const Camera3D& cam) {
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    double px = RL_CULL_DISTANCE_NEAR * std::tan(cam.fovy * 0.5 * DEG2RAD) / kViewHalf;  // near-plane size of one pixel
    rlFrustum(-kW / 2.0 * px, kW / 2.0 * px, -(kH - kViewMid) * px, kViewMid * px, RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(MatrixLookAt(cam.position, cam.target, cam.up)));
    rlEnableDepthTest();
}

// World point to screen pixels under beginStripMode3D; false when it's behind the camera.
bool stripToScreen(const Camera3D& cam, Vector3 wp, Vector2& out) {
    Vector3 v = Vector3Transform(wp, MatrixLookAt(cam.position, cam.target, cam.up));
    if (v.z > -0.1f) return false;
    float k = kViewHalf / std::tan(cam.fovy * 0.5f * DEG2RAD);
    out = {kW / 2.f + v.x / -v.z * k, kViewMid - v.y / -v.z * k};
    return true;
}

Vector3 W(Vec2 p, float h = 0) { return {p.x, h, -p.y}; }

void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Color col) {
    DrawTriangle3D(a, b, c, col);
    DrawTriangle3D(a, c, d, col);
}

void stripe(Vec2 a, Vec2 b, float w, Color c) {
    Vec2 q = (b - a).norm().perp() * (w / 2);
    quad(W(a + q), W(b + q), W(b - q), W(a - q), c);
}

void ring(Vec2 c, float r, float w, Color col, float a0 = 0, float a1 = 360, int segs = 48) {
    for (int k = 0; k < segs; ++k) {
        float t0 = (a0 + (a1 - a0) * k / segs) * DEG2RAD, t1 = (a0 + (a1 - a0) * (k + 1) / segs) * DEG2RAD;
        Vec2 u0(std::cos(t0), std::sin(t0)), u1(std::cos(t1), std::sin(t1));
        quad(W(c + u0 * (r - w)), W(c + u1 * (r - w)), W(c + u1 * r), W(c + u0 * r), col);
    }
}

void disc(Vec2 c, float r, Color col, float a0 = 0, float a1 = 360, int segs = 24) {
    for (int k = 0; k < segs; ++k) {
        float t0 = (a0 + (a1 - a0) * k / segs) * DEG2RAD, t1 = (a0 + (a1 - a0) * (k + 1) / segs) * DEG2RAD;
        DrawTriangle3D(W(c), W(c + Vec2(std::cos(t0), std::sin(t0)) * r), W(c + Vec2(std::cos(t1), std::sin(t1)) * r), col);
    }
}

// The rink outline (rounded rectangle), pushed `out` feet outward; counter-clockwise, not closed.
std::vector<Vec2> outline(float out, int segs = 12) {
    using namespace rink;
    const float ix = kHalfLength - kCornerRadius, iy = kHalfWidth - kCornerRadius, r = kCornerRadius + out;
    const float cx[4] = {ix, -ix, -ix, ix}, cy[4] = {iy, iy, -iy, -iy};
    std::vector<Vec2> p;
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k <= segs; ++k) {
            float a = (90.f * c + 90.f * k / segs) * DEG2RAD;
            p.push_back({cx[c] + r * std::cos(a), cy[c] + r * std::sin(a)});
        }
    return p;
}

// A band between two outlines at given heights (a stand row's tread, a riser, the board cap).
void band(const std::vector<Vec2>& in, const std::vector<Vec2>& out, float hIn, float hOut, Color c) {
    for (size_t i = 0; i < in.size(); ++i) {
        size_t j = (i + 1) % in.size();
        quad(W(in[i], hIn), W(in[j], hIn), W(out[j], hOut), W(out[i], hOut), c);
    }
}

void wall(const std::vector<Vec2>& line, float h0, float h1, Color c) {
    for (size_t i = 0; i < line.size(); ++i) {
        size_t j = (i + 1) % line.size();
        quad(W(line[i], h0), W(line[j], h0), W(line[j], h1), W(line[i], h1), c);
    }
}

struct Fan { Vector3 pos; Vec2 along; Color c; };
constexpr int kRows = 12;
constexpr float kRowDepth = 4.f, kRowRise = 2.6f, kStandGap = 3.f;

const std::vector<Fan>& crowd() {
    static std::vector<Fan> fans;
    if (!fans.empty()) return fans;
    static const Color shirts[] = {{200, 40, 50, 255}, {235, 235, 235, 255}, {30, 60, 140, 255}, {60, 60, 70, 255},
                                   {240, 180, 40, 255}, {40, 120, 70, 255}, {150, 150, 160, 255}, {20, 20, 26, 255}};
    uint32_t h = 12345;
    for (int row = 0; row < kRows; ++row) {
        auto line = outline(kStandGap + row * kRowDepth + kRowDepth * 0.5f, 16);
        float height = 4.f + row * kRowRise;
        for (size_t i = 0; i < line.size(); ++i) {
            Vec2 a = line[i], b = line[(i + 1) % line.size()];
            Vec2 d = b - a;
            int n = int(d.len() / 2.4f);
            for (int k = 0; k < n; ++k) {
                h = h * 1664525u + 1013904223u;
                if ((h >> 28) < 2) continue;  // empty seat
                fans.push_back({W(a + d * ((k + 0.5f) / n), height), d.norm(), shirts[(h >> 8) % 8]});
            }
        }
    }
    return fans;
}

void drawArena(const Game* g) {
    using namespace rink;
    static const auto ice = outline(0);
    // Flat pass: floor, ice, markings. Painted in order, no depth test.
    DrawPlane({0, -0.2f, 0}, {600, 500}, Color{14, 17, 24, 255});
    for (size_t i = 0; i < ice.size(); ++i) DrawTriangle3D(W({0, 0}), W(ice[i]), W(ice[(i + 1) % ice.size()]), kIce);
    if (g) disc({0, 0}, 4.5f, Fade(toColor(g->team(0).info.primary), 0.35f));
    float gy = kHalfWidth - 0.2f;
    stripe({0, -gy}, {0, gy}, 1.f, kRed);
    for (float sx : {-1.f, 1.f}) stripe({sx * kBlueLineX, -gy}, {sx * kBlueLineX, gy}, 1.f, kBlue);
    float gl = (kHalfWidth - kCornerRadius) + std::sqrt(kCornerRadius * kCornerRadius - std::pow(kGoalLineX - (kHalfLength - kCornerRadius), 2.f));
    for (float sx : {-1.f, 1.f}) stripe({sx * kGoalLineX, -gl}, {sx * kGoalLineX, gl}, 0.35f, kRed);
    ring({0, 0}, 15.f, 0.35f, kBlue);
    disc({0, 0}, 0.6f, kBlue);
    for (float sx : {-1.f, 1.f}) {
        for (float sy : {-1.f, 1.f}) {
            ring({sx * kZoneDotX, sy * kDotY}, 15.f, 0.35f, kRed);
            disc({sx * kZoneDotX, sy * kDotY}, 1.f, kRed);
            disc({sx * kNeutralDotX, sy * kDotY}, 1.f, kRed);
        }
        float a0 = sx > 0 ? 90.f : -90.f;
        disc({sx * kGoalLineX, 0}, kCreaseRadius, Fade(kCrease, 0.8f), a0, a0 + 180.f);
        ring({sx * kGoalLineX, 0}, kCreaseRadius, 0.25f, kRed, a0, a0 + 180.f, 24);
    }
}

void drawBoardsAndStands() {
    static const auto ice = outline(0), cap = outline(0.6f);
    wall(ice, 0, 0.7f, Color{230, 190, 60, 255});  // kick plate
    wall(ice, 0.7f, 3.3f, Color{246, 246, 242, 255});
    wall(ice, 3.3f, 3.5f, Color{60, 66, 78, 255});
    band(ice, cap, 3.5f, 3.5f, Color{70, 76, 90, 255});
    wall(cap, 0, 3.5f, Color{40, 44, 54, 255});
    // Stands: stepped rows rising away from the glass.
    float h = 0;
    for (int row = 0; row < kRows; ++row) {
        auto in = outline(kStandGap + row * kRowDepth, 16), out = outline(kStandGap + (row + 1) * kRowDepth, 16);
        float top = 4.f + row * kRowRise;
        wall(in, h, top, Color{30, 34, 44, 255});
        band(in, out, top, top, row % 2 ? Color{44, 50, 64, 255} : Color{52, 58, 74, 255});
        h = top;
    }
    for (const Fan& f : crowd()) {
        Vec2 s = f.along * 0.5f;
        Vector3 b0{f.pos.x - s.x, f.pos.y, f.pos.z + s.y}, b1{f.pos.x + s.x, f.pos.y, f.pos.z - s.y};
        quad(b0, b1, {b1.x, b1.y + 1.6f, b1.z}, {b0.x, b0.y + 1.6f, b0.z}, f.c);
        DrawCube({f.pos.x, f.pos.y + 2.0f, f.pos.z}, 0.6f, 0.6f, 0.6f, Color{214, 170, 140, 255});
    }
}

void drawGlass() {
    static const auto ice = outline(0);
    wall(ice, 3.5f, 8.5f, Color{190, 225, 245, 38});
}

void drawNets() {
    using namespace rink;
    const Color post = kRed, mesh = Color{245, 245, 245, 150};
    for (float sx : {-1.f, 1.f}) {
        float gx = sx * kGoalLineX, bx = sx * (kGoalLineX + kNetDepth);
        float y = kNetHalfWidth, h = 4.f;
        DrawCylinderEx(W({gx, y}), W({gx, y}, h), 0.15f, 0.15f, 8, post);
        DrawCylinderEx(W({gx, -y}), W({gx, -y}, h), 0.15f, 0.15f, 8, post);
        DrawCylinderEx(W({gx, y}, h), W({gx, -y}, h), 0.15f, 0.15f, 8, post);
        DrawCylinderEx(W({gx, y}), W({bx, y}), 0.1f, 0.1f, 6, post);
        DrawCylinderEx(W({gx, -y}), W({bx, -y}), 0.1f, 0.1f, 6, post);
        DrawCylinderEx(W({bx, y}), W({bx, -y}), 0.1f, 0.1f, 6, post);
        // Mesh: back, sides and roof sloping down from the crossbar.
        quad(W({bx, y}), W({bx, -y}), W({bx, -y}, 1.2f), W({bx, y}, 1.2f), mesh);
        quad(W({bx, y}, 1.2f), W({bx, -y}, 1.2f), W({gx, -y}, h), W({gx, y}, h), mesh);
        for (float sy : {-1.f, 1.f}) {
            DrawTriangle3D(W({gx, sy * y}), W({bx, sy * y}), W({gx, sy * y}, h), mesh);
            DrawTriangle3D(W({bx, sy * y}), W({bx, sy * y}, 1.2f), W({gx, sy * y}, h), mesh);
        }
    }
}

Color shade(Color c, float f) { return Color{(unsigned char)(c.r * f), (unsigned char)(c.g * f), (unsigned char)(c.b * f), c.a}; }

void drawPlayer3D(const Game& g, const Body& b) {
    Color fill = jerseyFill(g, b.team), trim = jerseyTrim(g, b.team);
    Color pants = shade(toColor(g.team(b.team).info.primary), 0.5f);
    Vec2 f = b.face.len() > 0.1f ? b.face.norm() : Vec2(1, 0), s = f.perp();
    Vec2 p = b.pos;
    if (b.isGoalie()) {
        for (float k : {-1.f, 1.f}) DrawCylinderEx(W(p + s * (k * 0.75f), 0.1f), W(p + s * (k * 0.75f) + f * 0.2f, 2.3f), 0.55f, 0.5f, 8, Color{236, 236, 230, 255});
        DrawCylinderEx(W(p, 2.0f), W(p, 4.1f), 1.25f, 1.35f, 12, fill);
        DrawCylinderEx(W(p, 3.0f), W(p, 3.35f), 1.33f, 1.37f, 12, trim);
        DrawSphere(W(p + f * 0.15f, 4.75f), 0.62f, Color{236, 236, 230, 255});
        DrawSphere(W(p + f * 0.35f, 4.75f), 0.45f, trim);
        DrawCube(W(p + f * 0.9f + s * 1.5f, 2.9f), 0.5f, 1.4f, 0.5f, Color{236, 236, 230, 255});  // blocker
        DrawSphere(W(p - s * 1.4f + f * 0.6f, 3.0f), 0.65f, trim);  // catcher
        DrawCylinderEx(W(p + f * 0.9f, 2.6f), W(p + f * 2.2f, 0.15f), 0.07f, 0.07f, 4, Color{30, 30, 30, 255});
        DrawCylinderEx(W(p + f * 2.2f - s * 0.6f, 0.15f), W(p + f * 2.2f + s * 0.6f, 0.15f), 0.14f, 0.14f, 4, Color{30, 30, 30, 255});
        return;
    }
    for (float k : {-1.f, 1.f}) DrawCylinderEx(W(p + s * (k * 0.38f), 0.1f), W(p + s * (k * 0.32f), 2.6f), 0.32f, 0.36f, 8, pants);
    DrawCylinderEx(W(p, 2.4f), W(p, 4.5f), 0.85f, 1.0f, 12, fill);
    DrawCylinderEx(W(p, 3.15f), W(p, 3.45f), 0.93f, 0.97f, 12, trim);
    DrawSphere(W(p, 4.55f), 1.0f, fill);
    DrawSphere(W(p, 5.65f), 0.5f, trim);
    // Stick from the hands down to the blade on the ice.
    Vec2 hands = p + f * 0.9f + s * 0.4f, heel = p + f * 2.2f;
    DrawCylinderEx(W(hands, 3.3f), W(heel, 0.15f), 0.07f, 0.07f, 4, Color{35, 30, 28, 255});
    DrawCylinderEx(W(heel, 0.15f), W(heel - s * 0.8f, 0.15f), 0.1f, 0.1f, 4, Color{35, 30, 28, 255});
}

void draw3D(const App& a) {
    const Game& g = *a.game;
    const Puck& pk = g.puck();
    beginStripMode3D(a.camera);
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthTest();
    drawArena(&g);
    for (size_t k = 1; k < a.trail.size(); ++k)
        stripe(a.trail[k - 1], a.trail[k], 0.3f, Fade(Color{20, 20, 20, 255}, float(k) / float(a.trail.size()) * 0.45f));
    for (int i = 0; i < 12; ++i) {
        const Body& b = g.bodies()[size_t(i)];
        if (!b.active) continue;
        disc(b.pos + Vec2(0.5f, -0.4f), b.isGoalie() ? 1.9f : 1.5f, Color{0, 0, 0, 50}, 0, 360, 16);
        if (pk.carrier == i) ring(b.pos, 2.4f, 0.35f, Fade(kGold, 0.85f), 0, 360, 24);
        if (b.stun > 0) ring(b.pos, 2.0f, 0.3f, Fade(kRed, 0.8f), 0, 360, 24);
    }
    disc(pk.pos + Vec2(0.15f, -0.1f), 0.5f, Color{0, 0, 0, 60}, 0, 360, 10);
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    drawBoardsAndStands();
    drawNets();
    for (int i = 0; i < 12; ++i)
        if (g.bodies()[size_t(i)].active) drawPlayer3D(g, g.bodies()[size_t(i)]);
    // The puck is drawn oversized (a real one is 3 in across) so it reads from the stands.
    DrawCylinder(W(pk.pos, 0.02f), 0.45f, 0.45f, 0.22f, 12, Color{12, 12, 12, 255});
    drawGlass();
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    EndMode3D();

    if (a.names) {
        for (int i = 0; i < 12; ++i) {
            const Body& b = g.bodies()[size_t(i)];
            Vector2 sp;
            if (!b.active || !stripToScreen(a.camera, W(b.pos, b.isGoalie() ? 6.0f : 6.8f), sp)) continue;
            if (sp.y < kViewTop + 10 || sp.y > kViewBottom - 6 || sp.x < 0 || sp.x > kW) continue;
            std::string n = lastName(g.player(b.team, b.roster).name);
            int w = MeasureText(n.c_str(), 10);
            DrawRectangle(int(sp.x) - w / 2 - 2, int(sp.y) - 12, w + 4, 12, Fade(kBg, 0.55f));
            textCenter(n, sp.x, sp.y - 11, 10, kText);
        }
    }
    std::string cam = std::string(kCamNames[a.cam]) + " CAM";
    DrawRectangle(10, 66, MeasureText(cam.c_str(), 10) + 12, 16, Fade(kBg, 0.75f));
    text(cam, 16, 69, 10, kDim);
}

// Eases the camera toward where the current mode wants it; snap jumps straight there.
void updateCamera(App& a, float dt, bool snap) {
    const Game& g = *a.game;
    Vec2 p = g.puck().pos;
    Vector3 pos, tgt;
    switch (a.cam) {
        case 0: {  // broadcast: high in the stands on the near side, following the play along the boards
            float x = std::clamp(p.x * 0.85f, -58.f, 58.f);
            tgt = {x, 0, std::clamp(-p.y * 0.25f, -6.f, 6.f) + 3.f};
            pos = {x, 66.f, tgt.z + 108.f};
            break;
        }
        case 1:  // wide: the whole rink from above center
            tgt = {0, 0, 6.f};
            pos = {0, 150.f, 112.f};
            break;
        default: {  // end zone: above the glass behind the net at the end where the puck is
            if (p.x * a.endSide < -30.f) { a.endSide = -a.endSide; snap = true; }  // cut to the other end, like TV
            float s = a.endSide;
            tgt = {s * 58.f, 0, -p.y * 0.4f};
            pos = {s * 136.f, 44.f, -p.y * 0.15f};
            break;
        }
    }
    // Zooming in narrows the lens, so aim further toward the puck or the shot fills up with empty ice.
    float aim = std::clamp((1.f - a.zoom) / 0.45f, 0.f, 1.f) * 0.85f;
    tgt = {lerp(tgt.x, p.x, aim), tgt.y, lerp(tgt.z, -p.y, aim)};
    // Faster sims move the puck further per frame, so the camera eases faster to keep up.
    float k = snap ? 1.f : 1.f - std::exp(-dt * 2.5f * std::sqrt(std::max(1.f, a.speed / 4.f)));
    auto ease = [k](Vector3& v, Vector3 to) { v = {v.x + (to.x - v.x) * k, v.y + (to.y - v.y) * k, v.z + (to.z - v.z) * k}; };
    ease(a.camera.position, pos);
    ease(a.camera.target, tgt);
    a.camera.up = {0, 1, 0};
    a.camera.fovy = 26.f * a.zoom;  // across the visible strip; zoom is a lens zoom, the camera stays put
    a.camera.projection = CAMERA_PERSPECTIVE;
}

void drawTopBar(const App& a) {
    const Game& g = *a.game;
    DrawRectangle(0, 0, kW, 60, kPanel);
    auto teamBlock = [&](int side, float x, bool right) {
        const Team& t = g.team(side);
        const TeamGame& T = g.side(side);
        Color c = toColor(t.info.primary);
        float bx = right ? x - 230 : x;
        DrawRectangle(int(bx), 8, 230, 44, kPanelHi);
        DrawRectangle(int(right ? x - 8 : bx), 8, 8, 44, c);
        std::string name = std::string(t.info.city) + " " + t.info.name;
        if (right) {
            textRight(t.info.abbr, x - 70, 12, 26, kText);
            textRight(ascii(name), x - 70, 38, 10, kDim);
            textRight(TextFormat("%d", T.score), x - 18, 12, 32, kText);
        } else {
            text(t.info.abbr, bx + 70, 12, 26, kText);
            text(ascii(name), bx + 70, 38, 10, kDim);
            text(TextFormat("%d", T.score), bx + 18, 12, 32, kText);
        }
        std::string st = g.strengthLabel(side);
        if (!st.empty()) {
            float rem = 0;
            for (const auto& p : g.side(1 - side).box) rem = std::max(rem, p.remaining);
            for (const auto& p : g.side(side).box) rem = std::max(rem, p.remaining);
            std::string lab = st + " " + clockText(rem);
            Color lc = st == "PP" ? kGold : kDim;
            if (right) textRight(lab, bx - 10, 22, 16, lc);
            else text(lab, bx + 240, 22, 16, lc);
        }
        // Current game plan (team style, or the situational mode late in a close game).
        std::string plan = g.planLabel(side);
        if (!plan.empty()) {
            if (right) textRight(plan, bx - 10, 42, 10, kDim);
            else text(plan, bx + 240, 42, 10, kDim);
        }
    };
    teamBlock(1, 16, false);
    teamBlock(0, kW - 16, true);

    // Period, clock and shots in the middle.
    std::string per = g.finished() ? "FINAL" : g.phase() == Phase::Intermission ? "INT" : g.periodLabel();
    textCenter(per, kW / 2.f, 8, 16, kDim);
    textCenter(clockText(g.clock()), kW / 2.f, 24, 30, kText);
    textCenter(TextFormat("SOG  %d - %d", g.side(1).sog, g.side(0).sog), kW / 2.f - 160, 22, 16, kDim);
    std::string sp = a.paused ? "PAUSED" : TextFormat("%gx", a.speed);
    textCenter(sp, kW / 2.f + 160, 22, 16, a.paused ? kGold : kDim);
}

void drawBottom(const App& a) {
    const Game& g = *a.game;
    float y0 = 572;
    DrawRectangle(0, int(y0), kW, kH - int(y0), kPanel);
    const char* slotName[6] = {"C", "LW", "RW", "LD", "RD", "G"};
    for (int col = 0; col < 2; ++col) {
        int side = col == 0 ? 1 : 0;
        float x = 16 + col * 310.f;
        DrawRectangle(int(x), int(y0) + 10, 6, 16, jerseyFill(g, side));
        DrawRectangleLines(int(x), int(y0) + 10, 6, 16, jerseyTrim(g, side));
        text(std::string(g.team(side).info.abbr) + " on ice", x + 12, y0 + 10, 16, kText);
        int row = 0;
        for (int slot = 0; slot < 6; ++slot) {
            const Body& b = g.bodies()[size_t(side * 6 + slot)];
            if (!b.active) continue;
            float yy = y0 + 34 + row * 22.f;
            const Player& p = g.player(side, b.roster);
            text(slotName[slot], x, yy, 12, kDim);
            text(ascii(p.name), x + 28, yy, 14, p.generated ? kDim : kText);
            if (slot < 5) {
                float e = g.side(side).energy[size_t(b.roster)];
                DrawRectangle(int(x) + 210, int(yy) + 3, 70, 8, kPanelHi);
                DrawRectangle(int(x) + 210, int(yy) + 3, int(70 * e), 8, e > 0.5f ? Color{80, 190, 120, 255} : e > 0.25f ? kGold : kRed);
            } else {
                const auto& L = g.side(side).stats[size_t(b.roster)];
                text(TextFormat("%d/%d", L.sv, L.sa), x + 210, yy, 12, kDim);
            }
            ++row;
        }
        for (const auto& pen : g.side(side).box) {
            float yy = y0 + 34 + row * 22.f;
            if (yy > kH - 20) break;
            text(TextFormat("BOX %s %s", lastName(g.player(side, pen.roster).name).c_str(), clockText(pen.remaining).c_str()), x + 28, yy, 12, kRed);
            ++row;
        }
    }
    // Event feed.
    float fx = 650;
    text("Game log", fx, y0 + 10, 16, kText);
    textRight("Space pause  1-5 speed  N names  B box  F finish  V 2D/3D  C camera  Esc menu", kW - 16, y0 + 12, 10, kDim);
    std::vector<const GameEvent*> shown;
    for (auto it = g.events().rbegin(); it != g.events().rend() && shown.size() < 7; ++it) {
        EventType t = it->type;
        if (t == EventType::Goal || t == EventType::Penalty || t == EventType::PeriodEnd || t == EventType::PeriodStart || t == EventType::Final ||
            t == EventType::Shootout || t == EventType::PenaltyExpired || t == EventType::Icing)
            shown.push_back(&*it);
    }
    float yy = y0 + 36;
    for (const GameEvent* e : shown) {
        Color c = e->type == EventType::Goal ? kGold : e->type == EventType::Penalty ? Color{240, 120, 120, 255} : kDim;
        std::string per = e->period <= 3 ? TextFormat("P%d", e->period) : "OT";
        text(per + " " + clockText(e->clock), fx, yy, 12, kDim);
        text(ascii(e->text), fx + 70, yy, 13, c);
        yy += 22;
    }
}

struct BoxRow { int roster; const Player* p; const PlayerLine* l; };

void drawBoxScore(const Game& g, float y0) {
    for (int col = 0; col < 2; ++col) {
        int side = col == 0 ? 1 : 0;
        float x = 24 + col * 624.f;
        const TeamGame& T = g.side(side);
        DrawRectangle(int(x), int(y0), 600, 30, toColor(g.team(side).info.primary));
        text(TextFormat("%s %s  %d", g.team(side).info.city, g.team(side).info.name, T.score), x + 10, y0 + 6, 18, kText);
        textRight(TextFormat("SOG %d  PP %d/%d  FOW %d  HIT %d  BLK %d", T.sog, T.ppGoals, T.ppOpp, T.fow, T.hits, T.blocks), x + 590, y0 + 9, 12, kText);
        std::vector<BoxRow> rows;
        for (size_t r = 0; r < T.stats.size(); ++r) {
            const Player& p = g.player(side, int(r));
            if (T.stats[r].toi <= 0 || p.isGoalie()) continue;
            rows.push_back({int(r), &p, &T.stats[r]});
        }
        std::sort(rows.begin(), rows.end(), [](const BoxRow& a, const BoxRow& b) {
            int pa = a.l->g * 2 + a.l->a, pb = b.l->g * 2 + b.l->a;
            return pa != pb ? pa > pb : a.l->toi > b.l->toi;
        });
        float yy = y0 + 38;
        const char* hdr[] = {"G", "A", "P", "SOG", "+/-", "HIT", "BLK", "PIM", "TOI"};
        text("Skater", x + 6, yy, 12, kDim);
        for (int k = 0; k < 9; ++k) textRight(hdr[k], x + 300 + k * 33.f, yy, 12, kDim);
        yy += 18;
        for (const auto& r : rows) {
            const PlayerLine& L = *r.l;
            text(ascii(r.p->name), x + 6, yy, 12, r.p->generated ? kDim : kText);
            int vals[8] = {L.g, L.a, L.g + L.a, L.sog, L.pm, L.hits, L.blk, L.pim};
            for (int k = 0; k < 8; ++k) textRight(k == 4 ? TextFormat("%+d", vals[k]) : TextFormat("%d", vals[k]), x + 300 + k * 33.f, yy, 12, kText);
            textRight(TextFormat("%d:%02d", int(L.toi) / 60, int(L.toi) % 60), x + 300 + 8 * 33.f, yy, 12, kText);
            yy += 15;
        }
        const PlayerLine& G = T.stats[size_t(T.goalie)];
        yy += 4;
        text(TextFormat("G  %s   SA %d  SV %d  GA %d  SV%% %.3f", ascii(g.player(side, T.goalie).name).c_str(), G.sa, G.sv, G.ga,
                        G.sa ? double(G.sv) / G.sa : 0.0),
             x + 6, yy, 13, kText);
    }
}

void drawFinal(const App& a) {
    const Game& g = *a.game;
    ClearBackground(kBg);
    std::string head = TextFormat("FINAL%s   %s %d  -  %s %d", g.wentToShootout() ? "/SO" : g.wentToOvertime() ? "/OT" : "", g.team(1).info.abbr,
                                  g.side(1).score, g.team(0).info.abbr, g.side(0).score);
    textCenter(head, kW / 2.f, 14, 30, kText);

    // Three stars: goals, assists, shots, and goalie saves above expected.
    struct Star { int side, roster; float score; };
    std::vector<Star> stars;
    for (int s = 0; s < 2; ++s) {
        const TeamGame& T = g.side(s);
        for (size_t r = 0; r < T.stats.size(); ++r) {
            const PlayerLine& L = T.stats[r];
            if (L.toi <= 0) continue;
            float sc = g.player(s, int(r)).isGoalie() ? (L.sv - 0.9f * L.sa) * 1.6f + (L.ga == 0 && L.sa > 15 ? 3.f : 0.f)
                                                      : L.g * 3.f + L.a * 2.f + L.sog * 0.2f + L.pm * 0.3f + L.hits * 0.05f;
            if (T.score > g.side(1 - s).score) sc += 0.5f;
            stars.push_back({s, int(r), sc});
        }
    }
    std::sort(stars.begin(), stars.end(), [](const Star& x, const Star& y) { return x.score > y.score; });
    float sx = kW / 2.f - 360;
    for (int k = 0; k < 3 && k < int(stars.size()); ++k) {
        const Star& st = stars[size_t(k)];
        const Player& p = g.player(st.side, st.roster);
        const PlayerLine& L = g.side(st.side).stats[size_t(st.roster)];
        std::string line = p.isGoalie() ? TextFormat("%d saves", L.sv) : TextFormat("%dG %dA, %d SOG", L.g, L.a, L.sog);
        float x = sx + k * 240.f;
        DrawRectangle(int(x), 54, 230, 46, kPanel);
        text(TextFormat("%d", k + 1), x + 10, 60, 28, kGold);
        text(TextFormat("%s (%s)", ascii(p.name).c_str(), g.team(st.side).info.abbr), x + 40, 60, 14, kText);
        text(line, x + 40, 80, 12, kDim);
    }
    drawBoxScore(g, 112);
    textCenter("Enter: new game", kW / 2.f, kH - 22, 14, kDim);
}

void drawMenu(App& a) {
    ClearBackground(kBg);
    textCenter("NHL SIM", kW / 2.f, 18, 36, kText);
    textCenter("Pick the away and home teams.  Ratings come from 2026-27 stats.", kW / 2.f, 60, 14, kDim);

    std::vector<int> order(a.league.teams.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    const char* titles[2] = {"HOME", "AWAY"};
    for (int col : {1, 0}) {
        float x = col == 1 ? 140.f : 700.f;
        bool focus = a.column == col;
        text(titles[col], x, 92, 20, focus ? kGold : kDim);
        text("OVR  OFF  DEF   G", x + 250, 98, 12, kDim);
        for (size_t k = 0; k < order.size(); ++k) {
            int ti = order[k];
            const Team& t = a.league.teams[size_t(ti)];
            float yy = 120 + float(k) * 18.f;
            bool sel = a.pick[col] == ti;
            Rectangle row{x - 6, yy - 2, 440, 18};
            if (CheckCollisionPointRec(GetMousePosition(), row) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { a.pick[col] = ti; a.column = col; }
            if (sel) DrawRectangleRec(row, focus ? kPanelHi : kPanel);
            DrawRectangle(int(x), int(yy), 10, 14, toColor(t.info.primary));
            text(TextFormat("%s  %s %s", t.info.abbr, t.info.city, t.info.name), x + 16, yy, 14, sel ? kText : kDim);
            text(TextFormat("%4.0f %4.0f %4.0f %4.0f", t.overall, t.offense, t.defense, t.goaltending), x + 250, yy, 14, sel ? kText : kDim);
        }
    }
    textCenter(TextFormat("%s  at  %s", a.league.teams[size_t(a.pick[1])].info.abbr, a.league.teams[size_t(a.pick[0])].info.abbr), kW / 2.f, 712, 22,
               kGold);
    textCenter("Up/Down choose   Left/Right switch column   R random   Enter start", kW / 2.f, 738, 12, kDim);
    std::string sum = ascii(a.league.summary);
    if (sum.size() > 150) sum = sum.substr(0, 147) + "...";
    text(sum, 12, kH - 16, 10, Color{100, 108, 124, 255});

    int n = int(a.league.teams.size());
    if (IsKeyPressed(KEY_LEFT)) a.column = 1;
    if (IsKeyPressed(KEY_RIGHT)) a.column = 0;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) a.pick[a.column] = (a.pick[a.column] + 1) % n;
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) a.pick[a.column] = (a.pick[a.column] + n - 1) % n;
    if (IsKeyPressed(KEY_R)) {
        a.pick[0] = GetRandomValue(0, n - 1);
        do a.pick[1] = GetRandomValue(0, n - 1); while (a.pick[1] == a.pick[0]);
    }
    if (IsKeyPressed(KEY_ENTER) && a.pick[0] != a.pick[1]) startGame(a);
}

// Game-screen actions shared by the keyboard and --script.
void toggleView(App& a) { a.view3d = !a.view3d; a.camSnap = true; }
void nextCamera(App& a) { a.cam = (a.cam + 1) % 3; a.zoom = 1.f; }
void zoomBy(App& a, float wheel) {
    const float tightest[3] = {0.55f, 0.75f, 0.8f};  // the wide and end-zone cameras sit far back; a tight lens there shows empty ice
    a.zoom = std::clamp(a.zoom * (1.f - 0.1f * wheel), tightest[a.cam], 1.6f);
}

void updateGame(App& a, float dt) {
    Game& g = *a.game;
    if (IsKeyPressed(KEY_SPACE)) a.paused = !a.paused;
    const float speeds[5] = {1, 2, 4, 8, 30};
    for (int k = 0; k < 5; ++k) if (IsKeyPressed(KEY_ONE + k)) a.speed = speeds[k];
    if (IsKeyPressed(KEY_N)) a.names = !a.names;
    if (IsKeyPressed(KEY_B)) a.showBox = !a.showBox;
    if (IsKeyPressed(KEY_F)) g.playToEnd();
    if (IsKeyPressed(KEY_ESCAPE)) { a.screen = Screen::Menu; return; }
    if (IsKeyPressed(KEY_V)) toggleView(a);
    if (IsKeyPressed(KEY_C)) nextCamera(a);
    if (a.view3d) zoomBy(a, GetMouseWheelMove());

    if (!a.paused) g.advance(std::min(dt, 0.05f) * a.speed);

    // Banner for new goals, penalties and icings (shown in real time, independent of sim speed).
    for (; a.seenEvents < g.events().size(); ++a.seenEvents) {
        const GameEvent& e = g.events()[a.seenEvents];
        if (e.type == EventType::Goal) { a.banner = ascii(e.text); a.bannerTime = 2.5f; a.bannerColor = kGold; }
        else if (e.type == EventType::Penalty) { a.banner = "PENALTY  " + ascii(e.text); a.bannerTime = 1.8f; a.bannerColor = Color{240, 120, 120, 255}; }
        else if (e.type == EventType::Icing && a.bannerTime <= 0) { a.banner = ascii(e.text); a.bannerTime = 1.0f; a.bannerColor = kDim; }
    }
    a.bannerTime = std::max(0.f, a.bannerTime - dt);

    if (g.puck().carrier < 0 && g.phase() == Phase::Play) a.trail.push_back(g.puck().pos);
    else a.trail.clear();
    while (a.trail.size() > 14) a.trail.pop_front();

    if (a.view3d) { updateCamera(a, dt, a.camSnap); a.camSnap = false; }

    if (g.finished() && a.bannerTime <= 0) a.screen = Screen::Final;
}

void drawGame(const App& a) {
    ClearBackground(kBg);
    if (a.view3d) draw3D(a);
    else { drawRink(a.view, a.game.get()); drawBodies(a); }
    drawTopBar(a);
    drawBottom(a);
    const Game& g = *a.game;
    if (g.phase() == Phase::Intermission && !g.finished()) {
        DrawRectangle(kW / 2 - 200, 280, 400, 60, Fade(kBg, 0.85f));
        textCenter(TextFormat("Intermission  -  end of %s", g.period() == 1 ? "1st" : g.period() == 2 ? "2nd" : "3rd"), kW / 2.f, 300, 20, kText);
    }
    if (a.bannerTime > 0 && !a.banner.empty()) {
        int w = MeasureText(a.banner.c_str(), 22) + 40;
        DrawRectangle(kW / 2 - w / 2, 92, w, 40, Fade(kBg, 0.88f));
        DrawRectangleLines(kW / 2 - w / 2, 92, w, 40, a.bannerColor);
        textCenter(a.banner, kW / 2.f, 101, 22, a.bannerColor);
    }
    if (a.showBox) {
        DrawRectangle(0, 60, kW, kH - 60, Fade(kBg, 0.95f));
        drawBoxScore(g, 76);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string dataDir = "data", shot, home, away, view = "2d", script, scriptOut = "script";
    float after = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string k = argv[i];
        if (k == "--data") dataDir = argv[++i];
        else if (k == "--screenshot") shot = argv[++i];
        else if (k == "--after") after = float(std::atof(argv[++i]));
        else if (k == "--home") home = argv[++i];
        else if (k == "--away") away = argv[++i];
        else if (k == "--view") view = argv[++i];
        else if (k == "--script") script = argv[++i];
        else if (k == "--script-out") scriptOut = argv[++i];
    }

    // Find the stats: --data, ./data, or data/ next to the executable (one or two levels up for IDE builds).
    if (!FileExists((dataDir + "/players.csv").c_str())) {
        std::string app = GetApplicationDirectory();
        for (const char* rel : {"data", "../data", "../../data"})
            if (FileExists((app + rel + "/players.csv").c_str())) { dataDir = app + rel; break; }
    }

    App a;
    a.league = loadLeague(dataDir);
    TraceLog(LOG_INFO, "%s", a.league.summary.c_str());
    if (!home.empty()) { int h = a.league.find(home); if (h >= 0) a.pick[0] = h; }
    if (!away.empty()) { int w = a.league.find(away); if (w >= 0) a.pick[1] = w; }

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(kW, kH, "NHL Sim");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    if (!shot.empty()) {  // render one frame of a game at a given sim time, save it, exit
        startGame(a);
        a.game->advance(after);
        a.speed = 4.f;
        a.view3d = view != "2d";
        a.cam = view == "wide" ? 1 : view == "end" ? 2 : 0;
        if (a.view3d) updateCamera(a, 0, true);
        if (a.game->finished()) a.screen = Screen::Final;
        for (int f = 0; f < 2; ++f) {
            BeginDrawing();
            if (a.screen == Screen::Final) drawFinal(a); else drawGame(a);
            EndDrawing();
        }
        Image img = LoadImageFromScreen();
        ExportImage(img, shot.c_str());
        UnloadImage(img);
        CloseWindow();
        return 0;
    }

    // --script: a live game with timed actions (see the header comment).
    struct Step { float t; std::string act; };
    std::vector<Step> steps;
    for (size_t p = 0; p < script.size();) {
        size_t e = script.find(',', p);
        std::string tok = script.substr(p, e == std::string::npos ? std::string::npos : e - p);
        size_t c = tok.find(':');
        if (c != std::string::npos) steps.push_back({float(std::atof(tok.substr(0, c).c_str())), tok.substr(c + 1)});
        p = e == std::string::npos ? script.size() : e + 1;
    }
    size_t nextStep = 0;
    int shots = 0;
    float wall = 0, frameSum = 0;
    int frames = 0;
    if (!steps.empty()) startGame(a);

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        bool takeShot = false, quit = false;
        wall += dt;
        frameSum += dt;
        ++frames;
        for (; nextStep < steps.size() && steps[nextStep].t <= wall; ++nextStep) {
            const std::string& act = steps[nextStep].act;
            if (act == "v") toggleView(a);
            else if (act == "c") nextCamera(a);
            else if (act == "n") a.names = !a.names;
            else if (act[0] == 'z') zoomBy(a, float(std::atof(act.c_str() + 1)));
            else if (act[0] == 's' && act.size() == 2) a.speed = std::array<float, 5>{1, 2, 4, 8, 30}[size_t(std::clamp(act[1] - '1', 0, 4))];
            else if (act == "shot") takeShot = true;
            else if (act == "quit") quit = true;
            const Vector3& cp = a.camera.position;
            TraceLog(LOG_INFO, "SCRIPT t=%.2f %-5s view=%s cam=%s zoom=%.2f campos=(%.1f,%.1f,%.1f) puck=(%.1f,%.1f) %s %s avgfps=%.0f", wall,
                     act.c_str(), a.view3d ? "3d" : "2d", kCamNames[a.cam], a.zoom, cp.x, cp.y, cp.z, a.game->puck().pos.x, a.game->puck().pos.y,
                     a.game->periodLabel().c_str(), clockText(a.game->clock()).c_str(), frames / std::max(frameSum, 1e-3f));
            if (act == "shot") { frameSum = 0; frames = 0; }
        }
        if (quit) break;
        if (a.screen == Screen::Play) updateGame(a, dt);
        else if (a.screen == Screen::Final && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE))) a.screen = Screen::Menu;

        BeginDrawing();
        switch (a.screen) {
            case Screen::Menu: drawMenu(a); break;
            case Screen::Play: drawGame(a); break;
            case Screen::Final: drawFinal(a); break;
        }
        if (takeShot) {
            rlDrawRenderBatchActive();
            Image img = LoadImageFromScreen();
            ExportImage(img, TextFormat("%s_%d.png", scriptOut.c_str(), ++shots));
            UnloadImage(img);
        }
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
