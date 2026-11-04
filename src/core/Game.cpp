#include "Game.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace hk {

#ifdef HK_DIAG
Diag g_diag;
#endif

namespace {

constexpr float kStep = 1.f / 120.f;
constexpr float kSkaterR = 1.3f;
constexpr float kPuckR = 0.25f;

// Calibration knobs. tools/headless_sim checks league-wide output against NHL averages.
namespace tune {
constexpr float shootUrge = 0.05f;     // per carrier decision, scaled by shot quality and shot rate
constexpr float goalScale = 0.65f;     // multiplies P(goal | on net)
constexpr float hitRate = 0.12f;       // per second while an opponent is within reach of the carrier
constexpr float pokeRate = 0.22f;      // stick checks per second, same condition
constexpr float blockChance = 0.65f;   // when a shot passes within reach of a defender
constexpr float penaltyOnHit = 0.056f;
constexpr float penaltyOnPoke = 0.034f;
}  // namespace tune

const float kFShift[4] = {56.f, 48.f, 38.f, 28.f};  // forward line shift lengths (s): top lines play more
const float kDShift[3] = {60.f, 50.f, 40.f};

float n01(float rating) { return clamp01((rating - 40.f) / 59.f); }
float discipline(const Player& p) { return std::exp((70.f - p.r.discipline) / 15.f); }

// Keeps a circle of radius r inside the rounded-rectangle boards. Returns true on contact.
bool constrainRink(Vec2& p, Vec2& v, float r, float restitution) {
    using namespace rink;
    const float ix = kHalfLength - kCornerRadius, iy = kHalfWidth - kCornerRadius;
    bool hit = false;
    float ax = std::fabs(p.x), ay = std::fabs(p.y);
    if (ax > ix && ay > iy) {
        Vec2 c{std::copysign(ix, p.x), std::copysign(iy, p.y)};
        Vec2 d = p - c;
        float L = d.len(), lim = kCornerRadius - r;
        if (L > lim) {
            Vec2 n = d / L;
            p = c + n * lim;
            float vn = dot(v, n);
            if (vn > 0) v -= n * (vn * (1.f + restitution));
            hit = true;
        }
    } else {
        if (ax > kHalfLength - r) {
            p.x = std::copysign(kHalfLength - r, p.x);
            if (v.x * p.x > 0) v.x = -v.x * restitution;
            hit = true;
        }
        if (ay > kHalfWidth - r) {
            p.y = std::copysign(kHalfWidth - r, p.y);
            if (v.y * p.y > 0) v.y = -v.y * restitution;
            hit = true;
        }
    }
    return hit;
}

// Nets are solid boxes behind each goal line; only the front (the mouth) is open, and
// pucks crossing the mouth are handled by the goal logic before this runs.
void collideNet(Vec2& p, Vec2& v, Vec2 prev, float r, float sg, float restitution) {
    using namespace rink;
    float x = p.x * sg, px = prev.x * sg;
    float x0 = kGoalLineX - r, x1 = kGoalLineX + kNetDepth + r, y1 = kNetHalfWidth + r;
    if (x <= x0 || x >= x1 || std::fabs(p.y) >= y1) return;
    float vx = v.x * sg;
    if (px >= x1) { x = x1; if (vx < 0) vx = -vx * restitution; }
    else if (std::fabs(prev.y) >= y1) { p.y = std::copysign(y1, prev.y); if (v.y * p.y < 0) v.y = -v.y * restitution; }
    else if (px <= x0) { x = x0; if (vx > 0) vx = -vx * restitution; }
    else {  // already inside (spawned or pushed): leave by the shortest way
        float dFront = x - x0, dBack = x1 - x, dSide = y1 - std::fabs(p.y);
        if (dFront <= dBack && dFront <= dSide) x = x0;
        else if (dBack <= dSide) x = x1;
        else p.y = std::copysign(y1, p.y);
    }
    p.x = x * sg;
    v.x = vx * sg;
}

}  // namespace

// ================================================================ setup ===

Game::Game(const League& league, int homeTeam, int awayTeam, GameOptions opt)
    : league_(league), opt_(opt), rng_(opt.seed) {
    tg_[0].team = homeTeam;
    tg_[1].team = awayTeam;
    for (int s = 0; s < 2; ++s) {
        size_t n = team(s).players.size();
        tg_[s].stats.assign(n, {});
        tg_[s].energy.assign(n, 1.f);
        tg_[s].goalie = team(s).lines.goalies[0];
    }
    for (int i = 0; i < 12; ++i) {
        bodies_[i].team = i / 6;
        bodies_[i].slot = i % 6;
    }
    startPeriod();
}

void Game::advance(float seconds) {
    acc_ += seconds;
    while (acc_ >= kStep && phase_ != Phase::Final) {
        tick(kStep);
        acc_ -= kStep;
    }
    if (phase_ == Phase::Final) acc_ = 0;
}

void Game::playToEnd() {
    while (phase_ != Phase::Final) tick(kStep);
}

void Game::log(EventType t, int side, std::string text) {
    events_.push_back({period_, clock_, t, side, std::move(text)});
}

std::string Game::periodLabel() const {
    if (shootoutDone_) return "SO";
    switch (period_) {
        case 1: return "1st";
        case 2: return "2nd";
        case 3: return "3rd";
        default: return "OT";
    }
}

int Game::skatersFor(int side) const {
    int own = std::min<int>(2, int(tg_[side].box.size()));
    int opp = std::min<int>(2, int(tg_[1 - side].box.size()));
    if (period_ > 3) return own > 0 ? 3 : std::min(5, 3 + opp);
    return std::max(3, 5 - own);
}

std::string Game::strengthLabel(int side) const {
    int a = skatersFor(side), b = skatersFor(1 - side);
    return a > b ? "PP" : a < b ? "SH" : "";
}

std::vector<int> Game::activeSlots(int side) const {
    int n = skatersFor(side);
    bool shorthanded = n < skatersFor(1 - side);
    if (n >= 5) return {0, 1, 2, 3, 4};
    if (n == 4) return {0, 1, 3, 4};
    if (period_ > 3 && !shorthanded) return {0, 1, 3};  // 3-on-3: two forwards, one D
    return {0, 3, 4};                                     // penalty kill: center + pair
}

std::string Game::shortName(const Body& b) const { return pl(b).name; }

// ============================================================ lineups ===

void Game::assignLineup(int side, bool placeNew) {
    TeamGame& T = tg_[side];
    const Team& tm = team(side);
    std::vector<int> slots = activeSlots(side);
    std::vector<char> used(tm.players.size(), 0);
    for (const auto& p : T.box) used[p.roster] = 1;
    used[T.goalie] = 1;

    for (int slot = 0; slot < 5; ++slot) {
        int idx = side * 6 + slot;
        Body& b = bodies_[idx];
        bool want = std::find(slots.begin(), slots.end(), slot) != slots.end();
        if (!want) {
            if (b.active && puck_.carrier == idx) loosePuck(b.vel);
            b.active = false;
            continue;
        }
        int pick = -1;
        if (slot < 3) {
            for (int k = 0; k < 4 && pick < 0; ++k) {
                int r = tm.lines.fwd[(T.fLine + k) % 4][slot];
                if (!used[r]) pick = r;
            }
            for (int l = 0; l < 4 && pick < 0; ++l)
                for (int s2 = 0; s2 < 3 && pick < 0; ++s2)
                    if (!used[tm.lines.fwd[l][s2]]) pick = tm.lines.fwd[l][s2];
        } else {
            for (int k = 0; k < 3 && pick < 0; ++k) {
                int r = tm.lines.def[(T.dLine + k) % 3][slot - 3];
                if (!used[r]) pick = r;
            }
            for (int l = 0; l < 3 && pick < 0; ++l)
                for (int s2 = 0; s2 < 2 && pick < 0; ++s2)
                    if (!used[tm.lines.def[l][s2]]) pick = tm.lines.def[l][s2];
        }
        if (pick < 0) { b.active = false; continue; }
        used[pick] = 1;
        if (!b.active) {
            b.active = true;
            if (placeNew) {
                b.pos = Vec2(side == 0 ? -8.f : 8.f, rink::kHalfWidth - 2.5f);
                b.vel = {0, 0};
            }
            b.stun = b.ignorePuck = 0;
        }
        b.roster = pick;
    }
    Body& g = bodies_[side * 6 + 5];
    g.active = true;
    g.roster = T.goalie;
}

void Game::lineChanges(int side, bool atStoppage) {
    TeamGame& T = tg_[side];
    bool canChange = atStoppage;
    if (!canChange) {
        int cs = puck_.carrier >= 0 ? bodies_[puck_.carrier].team : -1;
        canChange = cs != side && puck_.pos.x * T.dir > -15.f;
    }
    float f = atStoppage ? 0.6f : 1.f;
    bool changed = false;
    // A fresh power play starts with the top unit.
    if (atStoppage && skatersFor(side) > skatersFor(1 - side) && (T.fLine > 1 || T.dLine > 1)) {
        T.fLine = 0; T.dLine = 0; T.fShift = 0; T.dShift = 0;
        changed = true;
    }
    if (canChange && T.fShift > f * kFShift[T.fLine]) { T.fLine = (T.fLine + 1) % 4; T.fShift = 0; changed = true; }
    if (canChange && T.dShift > f * kDShift[T.dLine]) { T.dLine = (T.dLine + 1) % 3; T.dShift = 0; changed = true; }
    if (changed || atStoppage) assignLineup(side, !atStoppage);
}

// ================================================================ flow ===

void Game::startPeriod() {
    ++period_;
    clock_ = period_ <= 3 ? 1200.f : 300.f;
    int homeDir = (period_ % 2 == 1) ? 1 : -1;
    tg_[0].dir = homeDir;
    tg_[1].dir = -homeDir;
    for (auto& T : tg_) { T.fLine = 0; T.dLine = 0; T.fShift = 0; T.dShift = 0; }
    for (auto& b : bodies_) b.active = false;
    log(EventType::PeriodStart, -1, periodLabel() + " period underway");
    beginFaceoff({0, 0});
    for (auto& b : bodies_)  // teleport for the opening draw
        if (b.active) { b.pos = b.target; b.vel = {0, 0}; }
}

void Game::endPeriod() {
    log(EventType::PeriodEnd, -1, "End of " + periodLabel() + (period_ <= 3 ? " period" : ""));
    bool tied = tg_[0].score == tg_[1].score;
    if (period_ < 3 || (period_ == 3 && tied && opt_.overtime)) {
        phase_ = Phase::Intermission;
        phaseTimer_ = period_ < 3 ? opt_.intermission : opt_.intermission * 0.5f;
        for (auto& T : tg_) std::fill(T.energy.begin(), T.energy.end(), 1.f);
        puck_ = Puck{};
        return;
    }
    if (period_ >= 4 && tied && opt_.shootout) runShootout();
    phase_ = Phase::Final;
    std::ostringstream os;
    os << "Final" << (shootoutDone_ ? "/SO" : period_ > 3 ? "/OT" : "") << ": " << team(1).info.abbr << " " << tg_[1].score
       << ", " << team(0).info.abbr << " " << tg_[0].score;
    log(EventType::Final, -1, os.str());
}

Vec2 Game::faceoffSpot(const Body& b, Vec2 dot) const {
    float s = float(tg_[b.team].dir);
    float back = -s;
    Vec2 p;
    switch (b.slot) {
        case 0: p = dot + Vec2(back * 2.2f, 0); break;
        case 1: p = dot + Vec2(back * 4.f, s * 14.f); break;
        case 2: p = dot + Vec2(back * 4.f, -s * 14.f); break;
        case 3: p = dot + Vec2(back * 18.f, s * 11.f); break;
        case 4: p = dot + Vec2(back * 18.f, -s * 11.f); break;
        default: return idealGoaliePos(b.team, dot);
    }
    Vec2 v;
    constrainRink(p, v, kSkaterR + 1.f, 0);
    for (float sg : {1.f, -1.f}) collideNet(p, v, p, kSkaterR + 0.5f, sg, 0);
    return p;
}

void Game::beginFaceoff(Vec2 dot) {
    phase_ = Phase::FaceoffSetup;
    phaseTimer_ = opt_.faceoffSetup;
    faceoffDot_ = dot;
    puck_ = Puck{};
    puck_.pos = dot;
    touches_.clear();
    for (int s = 0; s < 2; ++s) lineChanges(s, true);
    for (auto& b : bodies_) {
        if (!b.active) continue;
        b.target = faceoffSpot(b, dot);
        b.urgency = 1;
        b.stun = b.ignorePuck = 0;
        b.oneTimerUntil = -1;
    }
}

void Game::resolveFaceoff() {
    Body& ca = bodies_[0];
    Body& cb = bodies_[6];
    float fa = pl(ca).r.faceoff, fb = pl(cb).r.faceoff;
    float pa = std::clamp(0.5f + (fa - fb) * 0.012f, 0.25f, 0.75f);
    int w = rng_.chance(pa) ? 0 : 1;
    Body& wc = w == 0 ? ca : cb;
    Body& lc = w == 0 ? cb : ca;
    tg_[w].fow++;
    tg_[w].stats[wc.roster].fow++;
    tg_[1 - w].stats[lc.roster].fol++;

    // Winner draws the puck back toward a defenseman.
    int best = -1;
    float bd = 1e9f;
    for (int slot = 3; slot < 5; ++slot) {
        const Body& d = bodies_[w * 6 + slot];
        if (!d.active) continue;
        float dd = dist(d.pos, faceoffDot_);
        if (dd < bd) { bd = dd; best = w * 6 + slot; }
    }
    Vec2 aim = best >= 0 ? bodies_[best].pos : faceoffDot_ - Vec2(float(tg_[w].dir) * 10.f, 0);
    puck_.pos = faceoffDot_;
    puck_.vel = rotate((aim - faceoffDot_).norm(), rng_.normal(0, 0.25f)) * rng_.range(14.f, 22.f);
    puck_.lastTeam = w;
    puck_.passTarget = best;
    ca.ignorePuck = cb.ignorePuck = 0.25f;
    phase_ = Phase::Play;
}

void Game::whistle(Vec2 faceoffDot, float pause, const std::string& why) {
    phase_ = Phase::Stoppage;
    phaseTimer_ = pause;
    nextFaceoff_ = faceoffDot;
    if (puck_.carrier >= 0) {
        bodies_[puck_.carrier].ignorePuck = 1.f;
        puck_.carrier = -1;
    }
    puck_.shot.live = false;
    puck_.passTarget = -1;
    if (!why.empty()) log(EventType::Whistle, -1, why);
}

Vec2 Game::zoneDot(int defendingSide, float ySign) const {
    return {-float(tg_[defendingSide].dir) * rink::kZoneDotX, ySign * rink::kDotY};
}

Vec2 Game::nearestDot(Vec2 p) const {
    using namespace rink;
    const Vec2 dots[9] = {{0, 0}, {kNeutralDotX, kDotY}, {kNeutralDotX, -kDotY}, {-kNeutralDotX, kDotY}, {-kNeutralDotX, -kDotY},
                          {kZoneDotX, kDotY}, {kZoneDotX, -kDotY}, {-kZoneDotX, kDotY}, {-kZoneDotX, -kDotY}};
    Vec2 best = dots[0];
    for (const Vec2& d : dots)
        if (dist(d, p) < dist(best, p)) best = d;
    return best;
}

// ================================================================ tick ===

void Game::tick(float h) {
    time_ += h;
    switch (phase_) {
        case Phase::Final: return;
        case Phase::Intermission:
            phaseTimer_ -= h;
            if (phaseTimer_ <= 0) startPeriod();
            return;
        case Phase::FaceoffSetup: {
            for (auto& b : bodies_) {
                if (!b.active) continue;
                if (b.isGoalie()) b.target = idealGoaliePos(b.team, faceoffDot_);
                Vec2 to = b.target - b.pos;
                float d = to.len();
                b.vel = to.norm() * std::min(d * 3.f, 55.f);
                b.pos += b.vel * h;
                Vec2 look = faceoffDot_ - b.pos;
                if (look.len() > 0.1f) b.face = look.norm();
            }
            phaseTimer_ -= h;
            if (phaseTimer_ <= 0) {
                for (auto& b : bodies_) if (b.active) { b.pos = b.target; b.vel = {0, 0}; }
                resolveFaceoff();
            }
            return;
        }
        case Phase::Stoppage: {
            for (auto& b : bodies_) {
                if (!b.active) continue;
                b.target = b.isGoalie() ? idealGoaliePos(b.team, puck_.pos) : b.pos;
                b.urgency = b.isGoalie() ? 0.5f : 0.f;
            }
            moveBodies(h);
            float sp = puck_.vel.len();
            if (sp > 0) puck_.vel = puck_.vel * (std::max(0.f, sp - 20.f * h) / sp);
            puck_.pos += puck_.vel * h;
            phaseTimer_ -= h;
            if (phaseTimer_ <= 0) beginFaceoff(nextFaceoff_);
            return;
        }
        case Phase::Play: break;
    }

    clock_ -= h;
    if (clock_ <= 0) {
        clock_ = 0;
        endPeriod();
        return;
    }

    // Penalty clocks.
    for (int s = 0; s < 2; ++s) {
        auto& box = tg_[s].box;
        bool changed = false;
        for (size_t i = 0; i < box.size();) {
            // Only the first two penalties tick at once (a third waits for a slot).
            if (i < 2) box[i].remaining -= h;
            if (box[i].remaining <= 0) {
                log(EventType::PenaltyExpired, s, player(s, box[i].roster).name + " returns to the ice");
                box.erase(box.begin() + long(i));
                changed = true;
            } else ++i;
        }
        if (changed) { assignLineup(0, true); assignLineup(1, true); }
    }

    for (int s = 0; s < 2; ++s) {
        tg_[s].fShift += h;
        tg_[s].dShift += h;
        lineChanges(s, false);
    }

    teamAI(0, h);
    teamAI(1, h);
    moveBodies(h);
    updatePuck(h);
    if (phase_ != Phase::Play) return;
    puckControl(h);
    if (phase_ != Phase::Play) return;
    pressure(h);
    if (phase_ != Phase::Play) return;

    // Ice time and fatigue.
    for (int s = 0; s < 2; ++s) {
        TeamGame& T = tg_[s];
        std::vector<char> on(T.energy.size(), 0);
        for (int slot = 0; slot < 6; ++slot) {
            const Body& b = bodies_[s * 6 + slot];
            if (!b.active) continue;
            on[b.roster] = 1;
            T.stats[b.roster].toi += h;
            if (slot < 5) {
                float sp = b.vel.len() / std::max(1.f, maxSpeed(b));
                T.energy[b.roster] = std::max(0.05f, T.energy[b.roster] - h * (0.004f + 0.009f * sp * sp));
            }
        }
        for (size_t r = 0; r < on.size(); ++r)
            if (!on[r]) T.energy[r] = std::min(1.f, T.energy[r] + 0.006f * h);
    }

    // A puck nobody can reach for a while gets whistled dead.
    if (puck_.carrier < 0 && puck_.vel.len() < 3.f) {
        puck_.idle += h;
        if (puck_.idle > 5.f) whistle(nearestDot(puck_.pos), opt_.whistlePause, "");
    } else {
        puck_.idle = 0;
    }
}

// ============================================================== motion ===

float Game::maxSpeed(const Body& b) const {
    if (b.isGoalie()) return 9.f + 7.f * n01(pl(b).gr.saving);
    const Player& p = pl(b);
    float e = tg_[b.team].energy[b.roster];
    float v = (22.5f + 8.5f * n01(p.r.skating)) * (0.82f + 0.18f * e);
    if (puck_.carrier >= 0 && &bodies_[puck_.carrier] == &b) v *= 0.92f;
    if (b.stun > 0) v *= 0.35f;
    return v;
}

void Game::moveBodies(float h) {
    for (auto& b : bodies_) {
        if (!b.active) continue;
        float ms = maxSpeed(b);
        Vec2 to = b.target - b.pos;
        float d = to.len();
        float want = ms * b.urgency;
        if (d < 8.f) want *= d / 8.f;
        Vec2 desired = to.norm() * want;
        float acc = b.isGoalie() ? 70.f : 24.f + 10.f * n01(pl(b).r.skating);
        if (b.stun > 0) acc *= 0.3f;
        b.vel += (desired - b.vel).clampLen(acc * h);
        b.vel = b.vel.clampLen(ms * 1.05f);
        b.pos += b.vel * h;

        float sp = b.vel.len();
        if (b.isGoalie()) {
            Vec2 look = puck_.pos - b.pos;
            if (look.len() > 0.1f) b.face = look.norm();
        } else if (sp > 2.f) {
            b.face = (b.face * 0.85f + (b.vel / sp) * 0.15f).norm();
        } else {
            Vec2 look = puck_.pos - b.pos;
            if (look.len() > 0.1f) b.face = (b.face * 0.9f + look.norm() * 0.1f).norm();
        }
        b.stun = std::max(0.f, b.stun - h);
        b.ignorePuck = std::max(0.f, b.ignorePuck - h);
        b.controlCooldown = std::max(0.f, b.controlCooldown - h);
    }
    // Skaters push apart.
    for (int i = 0; i < 12; ++i) {
        Body& a = bodies_[i];
        if (!a.active) continue;
        for (int j = i + 1; j < 12; ++j) {
            Body& c = bodies_[j];
            if (!c.active) continue;
            Vec2 d = c.pos - a.pos;
            float L = d.len(), minD = 2.f * kSkaterR;
            if (L < minD && L > 1e-4f) {
                Vec2 n = d / L;
                float push = (minD - L) * 0.5f;
                if (a.isGoalie()) { c.pos += n * (push * 2.f); }
                else if (c.isGoalie()) { a.pos -= n * (push * 2.f); }
                else { a.pos -= n * push; c.pos += n * push; }
            }
        }
    }
    for (auto& b : bodies_) {
        if (!b.active) continue;
        Vec2 prev = b.pos - b.vel * h;
        constrainRink(b.pos, b.vel, kSkaterR, 0.1f);
        for (float sg : {1.f, -1.f}) collideNet(b.pos, b.vel, prev, kSkaterR, sg, 0.f);
    }
}

// ================================================================= AI ===

float Game::nearestOpponent(int side, Vec2 p, int* who) const {
    float best = 1e9f;
    int bi = -1;
    for (int slot = 0; slot < 5; ++slot) {
        int i = (1 - side) * 6 + slot;
        const Body& o = bodies_[i];
        if (!o.active) continue;
        float d = dist(o.pos, p);
        if (d < best) { best = d; bi = i; }
    }
    if (who) *who = bi;
    return best;
}

float Game::baseShotQuality(int side, Vec2 p) const {
    float s = float(tg_[side].dir);
    Vec2 net = attackNet(side);
    float front = (net.x - p.x) * s;
    if (front < 1.f) return 0.f;
    float d = dist(p, net);
    if (d > 62.f) return 0.f;
    float ang = std::atan2(std::fabs(p.y), front);
    if (ang > 1.3f) return 0.f;
    return (0.38f * std::exp(-d / 15.f) + 0.025f) * (0.55f + 0.45f * std::cos(ang));
}

Vec2 Game::idealGoaliePos(int side, Vec2 P) const {
    Vec2 net = ownNet(side);
    float s = float(tg_[side].dir);
    Vec2 d = P - net;
    float depth = std::clamp(d.len() * 0.10f, 1.2f, 4.0f);
    Vec2 t = net + d.norm() * depth;
    if ((t.x - net.x) * s < 0.6f) t.x = net.x + s * 0.6f;
    t.y = std::clamp(t.y, -3.6f, 3.6f);
    return t;
}

void Game::goalieTarget(Body& g, float) {
    g.target = idealGoaliePos(g.team, puck_.pos + puck_.vel * 0.05f);
    g.urgency = 1.f;
}

void Game::teamAI(int side, float h) {
    goalieTarget(bodies_[side * 6 + 5], h);
    int carrier = puck_.carrier;
    int cs = carrier >= 0 ? bodies_[carrier].team : -1;

    if (cs == side) {
        Body& c = bodies_[carrier];
        c.think -= h;
        if (c.think <= 0) carrierThink(c);
        offenseTargets(side, puck_.pos, puck_.carrier);
        return;
    }
    if (cs == 1 - side) {
        defenseTargets(side, carrier, -1);
        return;
    }

    // Loose puck: one chaser (the intended receiver if it's our pass), the rest hold structure.
    int chaser = -1;
    if (puck_.passTarget >= 0 && bodies_[puck_.passTarget].team == side && bodies_[puck_.passTarget].active) {
        chaser = puck_.passTarget;
    } else {
        Vec2 pred = puck_.pos + puck_.vel * 0.3f;
        float best = 1e9f;
        for (int slot = 0; slot < 5; ++slot) {
            const Body& b = bodies_[side * 6 + slot];
            if (!b.active || b.stun > 0) continue;
            float d = dist(b.pos, pred) - (b.ignorePuck > 0 ? 0.f : 0.5f);
            if (d < best) { best = d; chaser = side * 6 + slot; }
        }
    }
    if (chaser >= 0) {
        Body& c = bodies_[chaser];
        Vec2 t;
        float sp2 = puck_.vel.len2();
        if (sp2 > 1.f) {  // meet the puck on its path
            float tt = std::max(0.f, dot(c.pos - puck_.pos, puck_.vel) / sp2);
            tt = std::min(tt, 1.2f);
            t = puck_.pos + puck_.vel * tt;
            if (dist(c.pos, t) > dist(c.pos, puck_.pos)) t = puck_.pos + puck_.vel * 0.25f;
        } else {
            t = puck_.pos;
        }
        Vec2 v;
        constrainRink(t, v, kSkaterR, 0);
        c.target = t;
        c.urgency = 1.f;
    }
    if (puck_.lastTeam == side) {
        offenseTargets(side, puck_.pos, chaser);
    } else {
        defenseTargets(side, -1, chaser);
    }
}

void Game::offenseTargets(int side, Vec2 P, int skip) {
    float s = float(tg_[side].dir);
    float px = P.x * s;
    std::vector<int> fw, dm;
    for (int slot = 0; slot < 5; ++slot) {
        int i = side * 6 + slot;
        if (!bodies_[i].active || i == skip) continue;
        (slot < 3 ? fw : dm).push_back(i);
    }
    auto wob = [&](const Body& b) { return std::sin(time_ * 0.8f + float(b.slot) * 1.7f + float(side)) * 2.5f; };

    if (px > rink::kBlueLineX) {
        float strong = P.y >= 0 ? 1.f : -1.f;
        bool carrierIsF = skip >= 0 && bodies_[skip].slot < 3 && bodies_[skip].team == side;
        Vec2 netFront(s * 83.5f, -strong * 3.5f), slotHigh(s * 64.f, -strong * 4.f);
        Vec2 lowStrong(s * 80.f, strong * 23.f), weakCircle(s * 70.f, -strong * 19.f);
        std::vector<Vec2> spots = carrierIsF ? std::vector<Vec2>{netFront, weakCircle, slotHigh}
                                             : std::vector<Vec2>{netFront, lowStrong, slotHigh};
        std::vector<char> taken(fw.size(), 0);
        for (const Vec2& sp : spots) {
            int bi = -1;
            float bd = 1e9f;
            for (size_t k = 0; k < fw.size(); ++k)
                if (!taken[k] && dist(bodies_[fw[k]].pos, sp) < bd) { bd = dist(bodies_[fw[k]].pos, sp); bi = int(k); }
            if (bi < 0) break;
            taken[size_t(bi)] = 1;
            Body& b = bodies_[fw[size_t(bi)]];
            b.target = sp + Vec2(0, wob(b));
            b.urgency = 0.8f;
        }
        bool carrierIsD = skip >= 0 && bodies_[skip].slot >= 3 && bodies_[skip].team == side;
        for (int i : dm) {
            Body& b = bodies_[i];
            float lane = b.slot == 3 ? s : -s;
            Vec2 pt(s * 31.f, lane * (lane == strong ? 20.f : 14.f));
            if (dm.size() == 1 || carrierIsD) pt.y = carrierIsD ? -strong * 6.f : strong * 6.f;
            b.target = pt + Vec2(0, wob(b) * 0.6f);
            b.urgency = 0.7f;
        }
    } else {
        float ahead = std::clamp(px + 24.f, -55.f, 48.f);
        for (int i : fw) {
            Body& b = bodies_[i];
            if (b.slot == 0) b.target = Vec2(s * (ahead - 6.f), P.y * 0.25f);
            else b.target = Vec2(s * ahead, (b.slot == 1 ? s : -s) * 24.f);
            b.target.y += wob(b);
            b.urgency = 0.95f;
        }
        float back = std::max(px - 16.f, -80.f);
        for (int i : dm) {
            Body& b = bodies_[i];
            b.target = Vec2(s * back, (b.slot == 3 ? s : -s) * 14.f);
            if (dm.size() == 1) b.target.y = 0;
            b.urgency = 0.9f;
        }
    }
}

void Game::defenseTargets(int side, int carrier, int chaser) {
    float s = float(tg_[side].dir);
    Vec2 own = ownNet(side);
    std::vector<int> mine;
    for (int slot = 0; slot < 5; ++slot) {
        int i = side * 6 + slot;
        if (bodies_[i].active && i != chaser) mine.push_back(i);
    }
    if (carrier >= 0) {
        const Body& c = bodies_[carrier];
        auto it = std::min_element(mine.begin(), mine.end(), [&](int a, int b) { return dist(bodies_[a].pos, c.pos) < dist(bodies_[b].pos, c.pos); });
        if (it != mine.end()) {
            Body& p = bodies_[*it];
            p.target = c.pos + (own - c.pos).norm() * 2.0f + c.vel * 0.15f;
            p.urgency = 1.f;
            mine.erase(it);
        }
    }
    std::vector<int> opps;
    for (int slot = 0; slot < 5; ++slot) {
        int i = (1 - side) * 6 + slot;
        if (bodies_[i].active && i != carrier) opps.push_back(i);
    }
    std::sort(opps.begin(), opps.end(), [&](int a, int b) { return dist(bodies_[a].pos, own) < dist(bodies_[b].pos, own); });
    float puckX = puck_.pos.x * s;
    for (int oi : opps) {
        if (mine.empty()) break;
        const Body& o = bodies_[oi];
        auto it = std::min_element(mine.begin(), mine.end(), [&](int a, int b) { return dist(bodies_[a].pos, o.pos) < dist(bodies_[b].pos, o.pos); });
        Body& m = bodies_[*it];
        mine.erase(it);
        Vec2 toNet = own - o.pos;
        Vec2 t = o.pos + toNet.norm() * std::clamp(toNet.len() * 0.15f, 1.6f, 6.f);
        if (m.slot >= 3) {  // defensemen stay goal-side of the puck
            float maxX = std::max(puckX - 6.f, -86.f);
            if (t.x * s > maxX) t.x = maxX * s;
        }
        m.target = t;
        m.urgency = o.pos.x * s < -rink::kBlueLineX ? 1.f : 0.85f;
    }
    for (int i : mine) {  // extra skaters guard the slot
        bodies_[i].target = Vec2(own.x + s * 14.f, 0);
        bodies_[i].urgency = 0.7f;
    }
}

void Game::carrierThink(Body& b) {
    int side = b.team;
    int bi = int(&b - &bodies_[0]);
    float s = float(tg_[side].dir);
    const Player& P = pl(b);
    Vec2 net = attackNet(side);
    float px = b.pos.x * s;
    int presser = -1;
    float pressureD = nearestOpponent(side, b.pos, &presser);
    bool inOZ = px > rink::kBlueLineX, inDZ = px < -rink::kBlueLineX;
    b.think = rng_.range(0.14f, 0.26f);

    // Shoot?
    float q = baseShotQuality(side, b.pos);
    if (q > 0 && b.possessedFor > 0.12f) {
        float urge = tune::shootUrge * std::pow(q / 0.08f, 0.5f) * std::pow(P.r.shotRate / 70.f, 1.6f);
        if (pressureD < 5.f) urge *= 1.3f;
        if (time_ < b.oneTimerUntil) urge *= 1.8f;
        if (time_ - tg_[1 - side].lastSave < 2.f) urge *= 2.f;
        if (b.slot >= 3 && dist(b.pos, net) > 40.f) urge *= 1.6f;  // point shots
        if (skatersFor(side) > skatersFor(1 - side)) urge *= 1.3f;
        if (rng_.chance(std::min(urge, 0.85f))) { shoot(b); return; }
    }

    // Pass?
    float myVal = (inOZ ? q * 9.f : 0.f) + 0.025f * std::min(pressureD, 15.f) + (px > 0 ? 0.1f : 0.f);
    int best = -1;
    float bestV = -1e9f;
    Vec2 bestAim;
    for (int slot = 0; slot < 5; ++slot) {
        int mi = side * 6 + slot;
        const Body& m = bodies_[mi];
        if (!m.active || mi == bi) continue;
        Vec2 aim = m.pos + m.vel * 0.45f;
        Vec2 v0;
        constrainRink(aim, v0, 2.f, 0);
        float pd = dist(b.pos, aim);
        if (pd < 8.f || pd > 85.f) continue;
        float lane = 99.f;
        for (int o = 0; o < 6; ++o) {
            const Body& ob = bodies_[(1 - side) * 6 + o];
            if (ob.active) lane = std::min(lane, distToSegment(ob.pos, b.pos, aim));
        }
        if (lane < 1.6f) continue;
        float risk = std::exp(-(lane - 1.6f) / 2.6f);
        float open = nearestOpponent(side, aim);
        float adv = ((aim.x - b.pos.x) * s) / 45.f;
        float score = aim.x * s > rink::kBlueLineX ? baseShotQuality(side, aim) * 5.f : 0.f;
        if (aim.x * s > rink::kBlueLineX && std::fabs(aim.y - b.pos.y) > 14.f && score > 0.4f) score *= 1.35f;
        float v = adv + score + 0.03f * std::min(open, 15.f) - 0.25f * std::max(0.f, 5.f - open) - 1.4f * risk;
        if (v > bestV) { bestV = v; best = mi; bestAim = aim; }
    }
    if (best >= 0 && b.possessedFor > 0.25f) {
        float margin = bestV - myVal;
        float pPass = 1.f / (1.f + std::exp(-(margin - 0.15f) * 6.f));
        if (pressureD < 4.f) pPass = std::min(1.f, pPass * 1.5f + 0.1f);
        pPass *= 0.6f + 0.4f * n01(P.r.passing);
        if (rng_.chance(pPass)) {
            float sp = std::clamp(dist(b.pos, bestAim) * 0.9f + 28.f, 35.f, 78.f);
            pass(b, best, sp, bestAim);
            return;
        }
    }

    // Dump it in, or clear it out.
    if (!inOZ && px > -12.f && px < 24.f && pressureD < 9.f && rng_.chance(0.25f)) {
        chip(b, Vec2(s * 94.f, (rng_.chance(0.5f) ? 1.f : -1.f) * 30.f), 62.f);
        return;
    }
    if (inDZ && pressureD < 4.f && rng_.chance(0.3f)) {
        chip(b, Vec2(s * 5.f, (b.pos.y >= 0 ? 1.f : -1.f) * 40.f), 66.f);
        return;
    }

    // Carry.
    Vec2 t;
    if (!inOZ) {
        float lane = b.slot == 0 ? 0.f : ((b.slot == 1 || b.slot == 3) ? s : -s) * 20.f;
        t = Vec2(b.pos.x + s * 30.f, lerp(b.pos.y, lane, 0.35f));
    } else {
        // Drive the net only through an open lane; otherwise work the perimeter (cycle low, or up high).
        float d = dist(b.pos, net);
        Vec2 slotPt(s * 74.f, b.pos.y * 0.3f);
        float laneBlock = 99.f;
        for (int o = 0; o < 5; ++o) {
            const Body& ob = bodies_[(1 - side) * 6 + o];
            if (ob.active) laneBlock = std::min(laneBlock, distToSegment(ob.pos, b.pos, slotPt));
        }
        float side_ = b.pos.y >= 0 ? 1.f : -1.f;
        if (laneBlock > 6.f) t = d > 28.f ? Vec2(s * 76.f, b.pos.y * 0.55f) : Vec2(s * 80.f, b.pos.y * 0.35f);
        else if (px > 70.f) t = Vec2(s * 52.f, side_ * 30.f);
        else t = Vec2(s * 85.f, side_ * 27.f);
    }
    for (int o = 0; o < 5; ++o) {
        const Body& ob = bodies_[(1 - side) * 6 + o];
        if (!ob.active) continue;
        Vec2 d = b.pos - ob.pos;
        float L = d.len();
        if (L < 10.f && L > 0.01f) t += d / L * ((10.f - L) * 0.9f);
    }
    t.x = std::clamp(t.x, -86.f, 86.f);
    t.y = std::clamp(t.y, -38.f, 38.f);
    b.target = t;
    b.urgency = 1.f;
}

// ======================================================== puck actions ===

void Game::gainControl(Body& b) {
    int bi = int(&b - &bodies_[0]);
    bool wasTarget = puck_.passTarget == bi;
    puck_.carrier = bi;
    puck_.passTarget = -1;
    puck_.shot.live = false;
    puck_.goalBound = false;
    puck_.idle = 0;
    puck_.lastTeam = b.team;
    b.possessedFor = 0;
    b.think = rng_.range(0.08f, 0.2f);
    if (touches_.empty() || touches_.back().team != b.team) touches_.clear();
    if (touches_.empty() || touches_.back().roster != b.roster) touches_.push_back({b.team, b.roster});
    if (touches_.size() > 6) touches_.erase(touches_.begin());
    float s = float(tg_[b.team].dir);
    if (wasTarget && b.pos.x * s > rink::kBlueLineX && dist(b.pos, attackNet(b.team)) < 40.f && std::fabs(puck_.passFrom.y - b.pos.y) > 14.f)
        b.oneTimerUntil = time_ + 0.4f;
}

void Game::loosePuck(Vec2 vel) {
    if (puck_.carrier >= 0) {
        Body& c = bodies_[puck_.carrier];
        c.ignorePuck = 0.25f;
        puck_.lastTeam = c.team;
    }
    puck_.carrier = -1;
    puck_.vel = vel;
    puck_.passTarget = -1;
}

void Game::shoot(Body& b) {
    int side = b.team;
    float s = float(tg_[side].dir);
    const Player& P = pl(b);
    Vec2 net = attackNet(side);
    float d = dist(b.pos, net);
    float z = (P.r.shooting - 70.f) / 9.f;
    float q = std::max(0.01f, baseShotQuality(side, b.pos)) * std::exp(0.2f * z);
    bool oneTimer = time_ < b.oneTimerUntil;
    if (oneTimer) q *= 1.35f;
    if (time_ - tg_[1 - side].lastSave < 2.f && d < 25.f) q *= 1.7f;  // rebound
    bool screened = false;
    int between = 0;
    for (int i = 0; i < 12; ++i) {
        const Body& o = bodies_[i];
        if (!o.active || &o == &b || o.isGoalie()) continue;
        float lane = distToSegment(o.pos, b.pos, net);
        if (lane < 2.5f && dist(o.pos, net) < 14.f) screened = true;
        if (o.team != side && (o.pos.x - b.pos.x) * s > 0 && lane < 6.f) ++between;
    }
#ifdef HK_DIAG
    g_diag.mods[0] += oneTimer; g_diag.mods[1] += (time_ - tg_[1 - side].lastSave < 2.f && d < 25.f); g_diag.mods[2] += screened; g_diag.mods[3] += (between == 0 && d < 40.f); g_diag.mods[4]++;
#endif
    if (screened) q *= 1.15f;
    if (skatersFor(side) > skatersFor(1 - side)) q *= 1.3f;  // man advantage: seams the 2D model doesn't capture
    if (between == 0 && d < 40.f) q *= 1.25f;  // nobody back: rush chance

    float pOn = std::clamp(0.64f + 0.05f * z - 0.0025f * (d - 25.f) - (oneTimer ? 0.06f : 0.f), 0.4f, 0.88f);
    bool onNet = rng_.chance(pOn);
    float ty = onNet ? rng_.range(-2.6f, 2.6f) : (rng_.chance(0.5f) ? 1.f : -1.f) * rng_.range(3.5f, 9.f);
    Vec2 aim(net.x, ty);
    float speed = 68.f + 30.f * n01(P.r.shooting) + (d > 45.f ? 12.f : 0.f) + rng_.normal(0, 5);
    Vec2 dirv = (aim - b.pos).norm();

    puck_.carrier = -1;
    puck_.pos = b.pos + dirv * 1.8f;
    puck_.vel = dirv * speed;
    puck_.passTarget = -1;
    puck_.lastTeam = side;
    ShotInfo& sh = puck_.shot;
    sh = ShotInfo{};
    sh.live = true;
    sh.onNet = onNet;
    sh.shooter = int(&b - &bodies_[0]);
    sh.team = side;
    sh.quality = q;
    sh.origin = b.pos;
    b.ignorePuck = 0.3f;
    b.oneTimerUntil = -1;
    tg_[side].attempts++;
    tg_[side].stats[b.roster].attempts++;
}

void Game::pass(Body& from, int toBody, float speed, Vec2 aim) {
    float err = (0.02f + 0.07f * (1.f - n01(pl(from).r.passing))) * rng_.normal(0, 1);
    Vec2 dirv = rotate((aim - from.pos).norm(), err);
    puck_.carrier = -1;
    puck_.pos = from.pos + dirv * 1.8f;
    puck_.vel = dirv * speed + from.vel * 0.25f;
    puck_.passTarget = toBody;
    puck_.passFrom = from.pos;
    puck_.shot.live = false;
    puck_.lastTeam = from.team;
    from.ignorePuck = 0.3f;
}

void Game::chip(Body& from, Vec2 aim, float speed) {
    Vec2 dirv = rotate((aim - from.pos).norm(), rng_.normal(0, 0.06f));
    puck_.carrier = -1;
    puck_.pos = from.pos + dirv * 1.8f;
    puck_.vel = dirv * speed;
    puck_.passTarget = -1;
    puck_.shot.live = false;
    puck_.lastTeam = from.team;
    from.ignorePuck = 0.4f;
}

// ======================================================= puck physics ===

void Game::updatePuck(float h) {
    Puck& p = puck_;
    if (p.carrier >= 0) {
        Body& c = bodies_[p.carrier];
        c.possessedFor += h;
        p.pos = c.pos + c.face * 1.7f;
        p.vel = c.vel;
        Vec2 v = p.vel;
        constrainRink(p.pos, v, kPuckR, 0);
        for (float sg : {1.f, -1.f}) collideNet(p.pos, v, c.pos, kPuckR, sg, 0);
        return;
    }

    Vec2 prev = p.pos;
    float sp = p.vel.len();
    if (sp > 0) {
        float nsp = std::max(0.f, sp - (5.f + 0.15f * sp) * h);
        p.vel = p.vel * (nsp / sp);
    }
    p.pos += p.vel * h;

    // Crossing a goal line through the mouth.
    for (float sg : {1.f, -1.f}) {
        if (!(prev.x * sg < rink::kGoalLineX && p.pos.x * sg >= rink::kGoalLineX)) continue;
        float t = (sg * rink::kGoalLineX - prev.x) / (p.pos.x - prev.x);
        float yc = prev.y + (p.pos.y - prev.y) * t;
        if (std::fabs(yc) >= rink::kNetHalfWidth - kPuckR) continue;
        int defending = tg_[0].dir == -int(sg) ? 0 : 1;
        if (!p.goalBound) {
            resolveAtNet(defending, true);
            if (phase_ != Phase::Play) return;
        }
        if (p.goalBound) { scoreGoal(1 - defending); return; }
        prev = p.pos;  // saved: resolveAtNet moved the puck out front
    }

    // A shot on net arriving at the goalie.
    if (p.shot.live && p.shot.onNet && !p.shot.resolved) {
        int defending = 1 - p.shot.team;
        float s = float(tg_[defending].dir);
        float toLine = (p.pos.x - ownNet(defending).x) * s;
        if (toLine < 2.5f && toLine > -0.5f) {
            resolveAtNet(defending, false);
            if (phase_ != Phase::Play) return;
        }
    }
    // Missed the net, or died out.
    if (p.shot.live && !p.shot.onNet) {
        Vec2 net = attackNet(p.shot.team);
        if ((p.pos.x - net.x) * float(tg_[p.shot.team].dir) > 0) p.shot.live = false;
    }
    if (p.shot.live && !p.goalBound && p.vel.len() < 25.f) p.shot.live = false;

    float before = p.vel.len();
    if (constrainRink(p.pos, p.vel, kPuckR, 0.55f) && before > 70.f && rng_.chance(0.10f)) {
        whistle(nearestDot(p.pos), opt_.whistlePause, "Puck out of play");
        return;
    }
    if (!p.goalBound)
        for (float sg : {1.f, -1.f}) collideNet(p.pos, p.vel, prev, kPuckR, sg, 0.4f);

    // Shot blocks.
    if (p.shot.live && !p.shot.resolved) {
        int def = 1 - p.shot.team;
        for (int slot = 0; slot < 5; ++slot) {
            int i = def * 6 + slot;
            Body& b = bodies_[i];
            if (!b.active || p.shot.blockChecked[size_t(i)]) continue;
            if (dist(b.pos, p.pos) > 2.6f) continue;
            p.shot.blockChecked[size_t(i)] = true;
            if (dist(p.pos, p.shot.origin) < 4.f) continue;
            float c = tune::blockChance * (0.75f + 0.5f * n01(pl(b).r.defense));
            if (rng_.chance(c)) {
                tg_[def].blocks++;
                tg_[def].stats[b.roster].blk++;
                float spd = p.vel.len();
                p.vel = rotate((-p.vel).norm(), rng_.range(-1.2f, 1.2f)) * (spd * rng_.range(0.12f, 0.3f));
                p.shot.live = false;
                p.lastTeam = def;
                break;
            }
        }
    }
    if (p.passTarget >= 0 && p.vel.len() < 10.f) p.passTarget = -1;
}

void Game::resolveAtNet(int defending, bool atLine) {
    Puck& p = puck_;
    ShotInfo& sh = p.shot;
    Body& g = bodies_[defending * 6 + 5];
    const Player& gp = pl(g);
    int att = 1 - defending;
    TeamGame& D = tg_[defending];
    TeamGame& O = tg_[att];
    bool isShot = sh.live && sh.onNet && sh.team == att && !sh.resolved;

    float q = isShot ? sh.quality : 0.05f;
    Vec2 ideal = idealGoaliePos(defending, isShot ? sh.origin : p.pos);
    float dev = dist(g.pos, ideal);
    float posF = std::clamp(1.f + 0.22f * std::max(0.f, dev - 1.f), 1.f, 2.4f);
    float goalieF = (1.f - gp.gr.svPct) / 0.100f;
    float pg = std::clamp(q * posF * goalieF * tune::goalScale, 0.004f, 0.9f);
    sh.resolved = true;
    sh.live = false;
#ifdef HK_DIAG
    g_diag.n[isShot]++; g_diag.q[isShot] += q; g_diag.posF[isShot] += posF; g_diag.pg[isShot] += pg;
    if (isShot) { float dd = dist(sh.origin, ownNet(defending)); g_diag.dist += dd; g_diag.hist[std::min(6, int(dd / 10))]++; }
#endif

    bool countsAsShot = isShot || atLine;
    if (countsAsShot) {
        O.sog++;
        D.stats[g.roster].sa++;
        int shooter = isShot ? bodies_[sh.shooter].roster : (!touches_.empty() && touches_.back().team == att ? touches_.back().roster : -1);
        if (shooter >= 0) O.stats[shooter].sog++;
    }
    if (rng_.chance(pg)) {
        p.goalBound = true;
        return;
    }
    if (countsAsShot) {
        D.stats[g.roster].sv++;
        log(EventType::Shot, att, "Save " + gp.name + (isShot ? " on " + pl(bodies_[sh.shooter]).name : ""));
    }

    Vec2 net = ownNet(defending);
    float s = float(D.dir);
    bool crowded = nearestOpponent(defending, p.pos) < 14.f;
    float freezeP = 0.42f + 0.12f * n01(gp.gr.saving);
    if (rng_.chance(freezeP) || (p.vel.len() < 25.f && crowded)) {
        p.vel = {0, 0};
        p.pos = g.pos + Vec2(s * 1.2f, 0);
        whistle(zoneDot(defending, p.pos.y >= 0 ? 1.f : -1.f), opt_.whistlePause, "");
        return;
    }
    // Rebound.
    Vec2 out = rotate(Vec2(s, 0), rng_.range(-1.1f, 1.1f));
    p.pos = Vec2(net.x + s * 2.2f, std::clamp(p.pos.y, -4.f, 4.f));
    p.vel = out * rng_.range(10.f, 32.f);
    p.goalBound = false;
    p.passTarget = -1;
    p.lastTeam = defending;
    D.lastSave = time_;
    g.ignorePuck = 0.6f;
}

void Game::scoreGoal(int side) {
    TeamGame& O = tg_[side];
    TeamGame& D = tg_[1 - side];
    O.score++;

    int scorer = -1;
    std::vector<int> assists;
    if (!touches_.empty() && touches_.back().team == side) {
        scorer = touches_.back().roster;
        for (int i = int(touches_.size()) - 2; i >= 0 && assists.size() < 2; --i) {
            int r = touches_[size_t(i)].roster;
            if (r != scorer && std::find(assists.begin(), assists.end(), r) == assists.end()) assists.push_back(r);
        }
    } else {  // own-goal style bounce: credit the nearest attacker
        float best = 1e9f;
        for (int slot = 0; slot < 5; ++slot) {
            const Body& b = bodies_[side * 6 + slot];
            if (b.active && dist(b.pos, puck_.pos) < best) { best = dist(b.pos, puck_.pos); scorer = b.roster; }
        }
    }
    if (scorer >= 0) O.stats[scorer].g++;
    for (int a : assists) O.stats[a].a++;
    D.stats[D.goalie].ga++;

    bool pp = skatersFor(side) > skatersFor(1 - side);
    bool sh = skatersFor(side) < skatersFor(1 - side);
    if (pp) {
        O.ppGoals++;
        if (!D.box.empty()) {
            auto it = std::min_element(D.box.begin(), D.box.end(), [](const Penalty& a, const Penalty& b) { return a.remaining < b.remaining; });
            log(EventType::PenaltyExpired, 1 - side, player(1 - side, it->roster).name + " returns to the ice");
            D.box.erase(it);
        }
    } else {
        for (int slot = 0; slot < 5; ++slot) {
            const Body& a = bodies_[side * 6 + slot];
            const Body& b = bodies_[(1 - side) * 6 + slot];
            if (a.active) O.stats[a.roster].pm++;
            if (b.active) D.stats[b.roster].pm--;
        }
    }

    std::ostringstream os;
    os << "GOAL " << team(side).info.abbr << "  ";
    if (scorer >= 0) os << player(side, scorer).name << " (" << O.stats[scorer].g << ")";
    if (!assists.empty()) {
        os << "  from ";
        for (size_t i = 0; i < assists.size(); ++i) os << (i ? ", " : "") << player(side, assists[i]).name;
    } else {
        os << "  unassisted";
    }
    if (pp) os << "  [PP]";
    if (sh) os << "  [SH]";
    log(EventType::Goal, side, os.str());

    puck_.vel = {0, 0};
    puck_.goalBound = false;
    puck_.carrier = -1;
    whistle({0, 0}, opt_.goalPause, "");
    if (period_ > 3) {  // sudden death
        phase_ = Phase::Final;
        std::ostringstream f;
        f << "Final/OT: " << team(1).info.abbr << " " << tg_[1].score << ", " << team(0).info.abbr << " " << tg_[0].score;
        log(EventType::Final, -1, f.str());
    }
}

void Game::callPenalty(int side, int roster, const char* infraction) {
    TeamGame& T = tg_[side];
    T.pim += 2;
    T.stats[roster].pim += 2;
    T.box.push_back({roster, 120.f, infraction});
    tg_[1 - side].ppOpp++;
    log(EventType::Penalty, side, player(side, roster).name + " (" + team(side).info.abbr + ")  2 min  " + infraction);
    whistle(zoneDot(side, puck_.pos.y >= 0 ? 1.f : -1.f), opt_.whistlePause, "");
}

void Game::puckControl(float) {
    if (puck_.carrier >= 0 || puck_.goalBound) return;

    // Goalies smother or play slow pucks near them.
    for (int side = 0; side < 2; ++side) {
        Body& g = bodies_[side * 6 + 5];
        if (g.ignorePuck > 0 || puck_.shot.live) continue;
        if (dist(g.pos, puck_.pos) > 2.4f || puck_.vel.len() > 22.f) continue;
        if (nearestOpponent(side, puck_.pos) < 10.f) {
            whistle(zoneDot(side, puck_.pos.y >= 0 ? 1.f : -1.f), opt_.whistlePause, "");
            return;
        }
        int mate = -1;
        float best = 1e9f;
        for (int slot = 0; slot < 5; ++slot) {
            const Body& b = bodies_[side * 6 + slot];
            if (b.active && dist(b.pos, g.pos) < best) { best = dist(b.pos, g.pos); mate = side * 6 + slot; }
        }
        if (mate >= 0) {
            Vec2 aim = bodies_[mate].pos;
            Vec2 dirv = (aim - g.pos).norm();
            puck_.pos = g.pos + dirv * 1.8f;
            puck_.vel = dirv * 40.f;
            puck_.passTarget = mate;
            puck_.passFrom = g.pos;
            puck_.lastTeam = side;
            g.ignorePuck = 1.f;
            return;
        }
    }

    int start = rng_.irange(0, 11);
    for (int k = 0; k < 12; ++k) {
        int i = (start + k) % 12;
        Body& b = bodies_[i];
        if (!b.active || b.isGoalie() || b.stun > 0 || b.ignorePuck > 0 || b.controlCooldown > 0) continue;
        if (dist(b.pos + b.face * 0.8f, puck_.pos) > 2.6f) continue;
        b.controlCooldown = 0.1f;
        float rel = (puck_.vel - b.vel).len();
        if (puck_.shot.live && puck_.shot.team != b.team && rel > 40.f) continue;  // blocks handled in flight
        float pc = (0.55f + 0.4f * n01(pl(b).r.handling)) * std::clamp(1.2f - rel / 55.f, 0.12f, 1.f);
        if (puck_.passTarget == i) pc = std::min(0.97f, pc + 0.25f);
        if (nearestOpponent(b.team, b.pos) < 3.f) pc *= 0.65f;  // contested reception
        if (rng_.chance(pc)) {
            if (puck_.lastTeam >= 0 && puck_.lastTeam != b.team && puck_.passTarget >= 0) {
                tg_[b.team].takeaways++;
                tg_[b.team].stats[b.roster].takeaways++;
            }
            gainControl(b);
            return;
        }
    }
}

void Game::pressure(float h) {
    if (puck_.carrier < 0) return;
    Body& c = bodies_[puck_.carrier];
    const Player& cp = pl(c);
    int os = 1 - c.team;
    for (int slot = 0; slot < 5; ++slot) {
        Body& o = bodies_[os * 6 + slot];
        if (!o.active || o.stun > 0) continue;
        if (dist(o.pos, c.pos) > 3.4f) continue;
        const Player& op = pl(o);
        float hitR = tune::hitRate * (0.35f + 1.1f * n01(op.r.checking));
        float pokeR = tune::pokeRate * (0.5f + 0.8f * n01(op.r.defense)) * (1.25f - 0.6f * n01(cp.r.handling));
        if (rng_.rate(hitR, h)) {
            tg_[os].hits++;
            tg_[os].stats[o.roster].hits++;
            c.stun = 0.6f;
            Vec2 kick = rotate(Vec2(1, 0), rng_.range(0.f, 2.f * kPi)) * rng_.range(4.f, 10.f);
            Vec2 v = c.vel * 0.5f + kick;
            c.vel *= 0.3f;
            o.vel *= 0.6f;
            loosePuck(v);
            if (rng_.chance(tune::penaltyOnHit * discipline(op))) {
                static const char* kHit[] = {"Roughing", "Boarding", "Interference", "Charging"};
                callPenalty(os, o.roster, kHit[rng_.irange(0, 3)]);
            }
            return;
        }
        if (rng_.rate(pokeR, h)) {
            if (rng_.chance(tune::penaltyOnPoke * discipline(op))) {
                static const char* kStick[] = {"Hooking", "Tripping", "Slashing", "Holding"};
                callPenalty(os, o.roster, kStick[rng_.irange(0, 3)]);
                return;
            }
            tg_[os].takeaways++;
            tg_[os].stats[o.roster].takeaways++;
            c.controlCooldown = 0.4f;
            if (rng_.chance(0.45f)) {
                c.ignorePuck = 0.4f;
                puck_.carrier = -1;
                gainControl(o);
            } else {
                loosePuck(c.vel * 0.4f + (o.pos - c.pos).perp().norm() * rng_.range(-8.f, 8.f));
            }
            return;
        }
    }
}

// ============================================================ shootout ===

void Game::runShootout() {
    log(EventType::Shootout, -1, "Shootout");
    std::array<std::vector<int>, 2> order;
    for (int s = 0; s < 2; ++s) {
        const Team& t = team(s);
        for (const auto& line : t.lines.fwd) for (int r : line) order[size_t(s)].push_back(r);
        for (const auto& pair : t.lines.def) for (int r : pair) order[size_t(s)].push_back(r);
        std::stable_sort(order[size_t(s)].begin(), order[size_t(s)].end(), [&](int a, int b) {
            const auto& pa = t.players[size_t(a)].r;
            const auto& pb = t.players[size_t(b)].r;
            return pa.shooting + pa.handling > pb.shooting + pb.handling;
        });
    }
    int made[2] = {0, 0}, taken[2] = {0, 0};
    auto attempt = [&](int s) {
        const auto& ord = order[size_t(s)];
        const Player& sh = player(s, ord[size_t(taken[s]) % ord.size()]);
        const Player& g = player(1 - s, tg_[1 - s].goalie);
        float pz = 0.25f * (sh.r.shooting - 70.f) / 9.f + 0.15f * (sh.r.handling - 70.f) / 9.f;
        float p = std::clamp(0.31f * std::exp(pz) * (1.f - g.gr.svPct) / 0.1f, 0.1f, 0.6f);
        bool goal = rng_.chance(p);
        made[s] += goal;
        taken[s]++;
        tg_[s].soAttempts++;
        tg_[s].soGoals += goal;
        log(EventType::Shootout, s, sh.name + (goal ? " scores" : " stopped by " + g.name));
    };
    for (int r = 0; r < 3; ++r) {
        for (int s : {1, 0}) {
            attempt(s);
            int left0 = 3 - taken[0], left1 = 3 - taken[1];
            if (made[0] + left0 < made[1] || made[1] + left1 < made[0]) goto done;
        }
    }
    while (made[0] == made[1]) { attempt(1); attempt(0); }
done:
    int w = made[0] > made[1] ? 0 : 1;
    tg_[w].score++;
    shootoutDone_ = true;
}

}  // namespace hk
