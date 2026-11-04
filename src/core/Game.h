#pragma once
#include <array>
#include <string>
#include <vector>

#include "League.h"
#include "Math.h"

namespace hk {

#ifdef HK_DIAG
struct Diag { long n[2]{}; double q[2]{}, posF[2]{}, pg[2]{}; double dist = 0; long hist[7]{}; long mods[5]{}; };
extern Diag g_diag;
#endif

// Rink geometry in feet. Origin at center ice, x along the length, y across.
namespace rink {
constexpr float kHalfLength = 100.f;
constexpr float kHalfWidth = 42.5f;
constexpr float kCornerRadius = 28.f;
constexpr float kGoalLineX = 89.f;
constexpr float kBlueLineX = 25.f;
constexpr float kNetHalfWidth = 3.f;
constexpr float kNetDepth = 3.67f;
constexpr float kCreaseRadius = 6.f;
constexpr float kZoneDotX = 69.f;
constexpr float kNeutralDotX = 20.f;
constexpr float kDotY = 22.f;
}  // namespace rink

enum class Phase { FaceoffSetup, Play, Stoppage, Intermission, Final };

enum class EventType { PeriodStart, PeriodEnd, Faceoff, Shot, Goal, Block, Miss, Hit, Takeaway, Penalty, PenaltyExpired, Whistle, Shootout, Final };

struct GameEvent {
    int period;
    float clock;  // seconds remaining in the period
    EventType type;
    int team;  // 0 = home, 1 = away, -1 = none
    std::string text;
};

struct PlayerLine {
    int g = 0, a = 0, sog = 0, attempts = 0, hits = 0, blk = 0, pim = 0, fow = 0, fol = 0, takeaways = 0, pm = 0;
    float toi = 0;               // seconds
    int sa = 0, sv = 0, ga = 0;  // goalies
};

struct Penalty {
    int roster;
    float remaining;
    std::string infraction;
};

struct TeamGame {
    int team = 0;  // index into League::teams
    int dir = 1;   // +1 attacks the +x net this period
    int score = 0, sog = 0, attempts = 0, hits = 0, blocks = 0, fow = 0, ppOpp = 0, ppGoals = 0, pim = 0, takeaways = 0;
    int soGoals = 0, soAttempts = 0;
    int goalie = 0;  // roster index of the goalie in net
    std::vector<PlayerLine> stats;  // by roster index
    std::vector<float> energy;      // by roster index, 0..1
    int fLine = 0, dLine = 0;
    float fShift = 0, dShift = 0;
    std::vector<Penalty> box;
    float lastSave = -100;  // game time of this team's goalie's last rebound save
};

// One on-ice slot: 0 C, 1 LW, 2 RW, 3 LD, 4 RD, 5 G. Bodies are indexed team*6 + slot.
struct Body {
    int team = 0, slot = 0, roster = -1;
    bool active = false;
    Vec2 pos, vel, face{1, 0};
    Vec2 target;
    float urgency = 1;  // fraction of top speed wanted
    float stun = 0, ignorePuck = 0, think = 0, controlCooldown = 0, possessedFor = 0, oneTimerUntil = -1;
    bool isGoalie() const { return slot == 5; }
};

struct ShotInfo {
    bool live = false;
    bool onNet = false;
    bool resolved = false;
    int shooter = -1;  // body index
    int team = -1;
    float quality = 0;  // probability of a goal on an average goalie, before positioning
    Vec2 origin;
    std::array<bool, 12> blockChecked{};
};

struct Puck {
    Vec2 pos, vel;
    int carrier = -1;  // body index
    bool goalBound = false;
    int lastTeam = -1;
    int passTarget = -1;
    Vec2 passFrom;
    ShotInfo shot;
    float idle = 0;
};

struct GameOptions {
    bool overtime = true;
    bool shootout = true;
    float intermission = 0.f;  // wall-clock-ish pause between periods (sim seconds, not game clock)
    float goalPause = 3.f;
    float whistlePause = 1.2f;
    float faceoffSetup = 1.0f;
    uint64_t seed = 1;
};

class Game {
public:
    Game(const League& league, int homeTeam, int awayTeam, GameOptions opt = {});

    // Advances the simulation by `seconds` of sim time using fixed 1/120 s steps.
    void advance(float seconds);
    void playToEnd();

    bool finished() const { return phase_ == Phase::Final; }
    Phase phase() const { return phase_; }
    int period() const { return period_; }
    float clock() const { return clock_; }
    float time() const { return time_; }
    bool wentToShootout() const { return shootoutDone_; }
    bool wentToOvertime() const { return period_ > 3; }

    const std::array<Body, 12>& bodies() const { return bodies_; }
    const Puck& puck() const { return puck_; }
    const TeamGame& side(int i) const { return tg_[i]; }
    const Team& team(int i) const { return league_.teams[tg_[i].team]; }
    const Player& player(int side, int roster) const { return team(side).players[roster]; }
    const std::vector<GameEvent>& events() const { return events_; }
    int skatersFor(int side) const;
    std::string periodLabel() const;
    std::string strengthLabel(int side) const;  // "PP", "SH" or ""

private:
    const League& league_;
    GameOptions opt_;
    Rng rng_;
    std::array<TeamGame, 2> tg_;
    std::array<Body, 12> bodies_;
    Puck puck_;
    std::vector<GameEvent> events_;
    Phase phase_ = Phase::Intermission;
    int period_ = 0;
    float clock_ = 0, time_ = 0, phaseTimer_ = 0, acc_ = 0;
    Vec2 faceoffDot_;
    Vec2 nextFaceoff_;
    bool shootoutDone_ = false;
    struct Touch { int team, roster; };
    std::vector<Touch> touches_;

    // flow
    void tick(float h);
    void startPeriod();
    void endPeriod();
    void beginFaceoff(Vec2 dot);
    void resolveFaceoff();
    void whistle(Vec2 faceoffDot, float pause, const std::string& why);
    void runShootout();
    void log(EventType t, int team, std::string text);

    // lineup
    void assignLineup(int side, bool placeNew);
    void lineChanges(int side, bool atStoppage);
    std::vector<int> activeSlots(int side) const;

    // play
    void teamAI(int side, float h);
    void carrierThink(Body& b);
    void offenseTargets(int side, Vec2 puckPos, int carrier);
    Vec2 faceoffSpot(const Body& b, Vec2 dot) const;
    void defenseTargets(int side, int carrier, int chaser);
    void goalieTarget(Body& g, float h);
    void moveBodies(float h);
    void updatePuck(float h);
    void puckControl(float h);
    void pressure(float h);
    void shoot(Body& b);
    void pass(Body& from, int toBody, float speed, Vec2 aimPoint);
    void chip(Body& from, Vec2 aim, float speed);
    void gainControl(Body& b);
    void loosePuck(Vec2 vel);
    void resolveAtNet(int defendingSide, bool atLine);
    void scoreGoal(int scoringSide);
    void callPenalty(int side, int roster, const char* infraction);

    // helpers
    Vec2 attackNet(int side) const { return {tg_[side].dir * rink::kGoalLineX, 0}; }
    Vec2 ownNet(int side) const { return {-tg_[side].dir * rink::kGoalLineX, 0}; }
    const Player& pl(const Body& b) const { return player(b.team, b.roster); }
    float maxSpeed(const Body& b) const;
    float baseShotQuality(int side, Vec2 p) const;
    Vec2 idealGoaliePos(int side, Vec2 puckPos) const;
    Vec2 nearestDot(Vec2 p) const;
    Vec2 zoneDot(int defendingSide, float ySign) const;
    float nearestOpponent(int side, Vec2 p, int* who = nullptr) const;
    std::string shortName(const Body& b) const;
};

}  // namespace hk
