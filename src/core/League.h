#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hk {

enum class Pos { C, LW, RW, F, D, G };
const char* posName(Pos p);
inline bool isForward(Pos p) { return p == Pos::C || p == Pos::LW || p == Pos::RW || p == Pos::F; }

// All ratings are on a 40-99 scale, 70 = league-average regular for the position.
struct SkaterRatings {
    float skating = 70;     // proxy: scoring pace, ice time and age curve (no tracking data in box scores)
    float shooting = 70;    // finishing: shrunk shooting % + goals/60
    float shotRate = 70;    // tendency to shoot: shots/60
    float passing = 70;     // assists/60
    float handling = 70;    // points/60 + assists/60
    float defense = 70;     // blocks/60, +/-, ice time (D weighted on TOI)
    float checking = 70;    // hits/60
    float faceoff = 50;     // shrunk FO%; 50 when the player rarely takes draws
    float discipline = 70;  // inverse PIM/60
    float overall = 70;
};

struct GoalieRatings {
    float saving = 70;   // shrunk save %
    float overall = 70;
    float svPct = 0.900f;  // shrunk save % used directly by the sim
};

struct Player {
    std::string name;
    std::string team;
    Pos pos = Pos::F;
    int age = 27;
    bool generated = false;  // depth player created to fill a roster gap
    // skater box-score stats (season to date)
    int gp = 0, g = 0, a = 0, pts = 0, pm = 0, pim = 0, s = 0, blk = 0, hit = 0, fow = 0, fol = 0;
    float toiPerGame = 0;  // minutes
    // goalie stats
    int sa = 0, sv = 0, ga = 0;
    float goalieMinutes = 0;
    SkaterRatings r;
    GoalieRatings gr;
    bool isGoalie() const { return pos == Pos::G; }
};

struct Color3 { uint8_t r, g, b; };

struct TeamInfo {
    const char* abbr;
    const char* city;
    const char* name;
    Color3 primary;
    Color3 secondary;
};
const std::vector<TeamInfo>& teamTable();

// Indices into Team::players.
struct Lineup {
    std::array<std::array<int, 3>, 4> fwd{};  // [line][C, LW, RW]
    std::array<std::array<int, 2>, 3> def{};  // [pair][LD, RD]
    std::array<int, 2> goalies{};             // starter, backup
};

struct Team {
    TeamInfo info;
    std::vector<Player> players;
    Lineup lines;
    float offense = 70, defense = 70, goaltending = 70, overall = 70;
    // Playing style, 0 = dump-and-chase / heavy forecheck, 1 = puck possession (controlled entries, more passing).
    // From the ice-time weighted skill (handling, passing, skating) vs physical (checking) mix, spread across the league.
    float style = 0.5f;
    int realPlayers = 0;  // players that came from the stats files
};

struct League {
    std::vector<Team> teams;
    std::string summary;  // human-readable load report
    int find(const std::string& abbr) const;
};

// Loads <dataDir>/players.csv and (optionally) <dataDir>/goalies.csv, computes ratings,
// fills roster gaps with generated depth players, and builds lines for all 32 teams.
League loadLeague(const std::string& dataDir, uint64_t seed = 2026);

}  // namespace hk
