#include "League.h"
#include "Math.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace hk {

const char* posName(Pos p) {
    switch (p) {
        case Pos::C: return "C";
        case Pos::LW: return "LW";
        case Pos::RW: return "RW";
        case Pos::F: return "F";
        case Pos::D: return "D";
        case Pos::G: return "G";
    }
    return "?";
}

const std::vector<TeamInfo>& teamTable() {
    static const std::vector<TeamInfo> t = {
        {"ANA", "Anaheim", "Ducks", {252, 76, 2}, {40, 40, 40}},
        {"BOS", "Boston", "Bruins", {252, 181, 20}, {17, 17, 17}},
        {"BUF", "Buffalo", "Sabres", {0, 48, 135}, {252, 181, 20}},
        {"CGY", "Calgary", "Flames", {200, 16, 46}, {241, 190, 72}},
        {"CAR", "Carolina", "Hurricanes", {206, 17, 38}, {30, 30, 30}},
        {"CHI", "Chicago", "Blackhawks", {207, 10, 44}, {20, 20, 20}},
        {"COL", "Colorado", "Avalanche", {111, 38, 61}, {35, 97, 146}},
        {"CBJ", "Columbus", "Blue Jackets", {0, 38, 84}, {206, 17, 38}},
        {"DAL", "Dallas", "Stars", {0, 104, 71}, {143, 143, 140}},
        {"DET", "Detroit", "Red Wings", {206, 17, 38}, {255, 255, 255}},
        {"EDM", "Edmonton", "Oilers", {4, 30, 66}, {252, 76, 0}},
        {"FLA", "Florida", "Panthers", {200, 16, 46}, {4, 30, 66}},
        {"LAK", "Los Angeles", "Kings", {40, 40, 40}, {162, 170, 173}},
        {"MIN", "Minnesota", "Wild", {21, 71, 52}, {166, 25, 46}},
        {"MTL", "Montreal", "Canadiens", {175, 30, 45}, {25, 33, 104}},
        {"NSH", "Nashville", "Predators", {255, 184, 28}, {4, 30, 66}},
        {"NJD", "New Jersey", "Devils", {206, 17, 38}, {20, 20, 20}},
        {"NYI", "New York", "Islanders", {0, 83, 155}, {244, 125, 48}},
        {"NYR", "New York", "Rangers", {0, 56, 168}, {206, 17, 38}},
        {"OTT", "Ottawa", "Senators", {218, 26, 50}, {183, 146, 90}},
        {"PHI", "Philadelphia", "Flyers", {247, 73, 2}, {20, 20, 20}},
        {"PIT", "Pittsburgh", "Penguins", {30, 30, 30}, {252, 181, 20}},
        {"SJS", "San Jose", "Sharks", {0, 109, 117}, {234, 114, 0}},
        {"SEA", "Seattle", "Kraken", {0, 22, 40}, {153, 217, 217}},
        {"STL", "St. Louis", "Blues", {0, 47, 135}, {252, 181, 20}},
        {"TBL", "Tampa Bay", "Lightning", {0, 40, 104}, {255, 255, 255}},
        {"TOR", "Toronto", "Maple Leafs", {0, 32, 91}, {255, 255, 255}},
        {"UTA", "Utah", "Mammoth", {105, 179, 231}, {20, 20, 20}},
        {"VAN", "Vancouver", "Canucks", {0, 32, 91}, {0, 132, 61}},
        {"VGK", "Vegas", "Golden Knights", {185, 151, 91}, {51, 63, 72}},
        {"WSH", "Washington", "Capitals", {4, 30, 66}, {200, 16, 46}},
        {"WPG", "Winnipeg", "Jets", {4, 30, 66}, {124, 135, 142}},
    };
    return t;
}

int League::find(const std::string& abbr) const {
    for (size_t i = 0; i < teams.size(); ++i)
        if (abbr == teams[i].info.abbr) return int(i);
    return -1;
}

// ---------------------------------------------------------------- CSV ----

namespace {

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (q) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else if (c == '"') q = false;
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

struct CsvTable {
    std::map<std::string, int> col;
    std::vector<std::vector<std::string>> rows;
    bool ok = false;
    std::string get(const std::vector<std::string>& r, const char* name) const {
        auto it = col.find(name);
        if (it == col.end() || it->second >= int(r.size())) return {};
        return r[it->second];
    }
    int geti(const std::vector<std::string>& r, const char* name) const {
        std::string s = get(r, name);
        if (s.empty()) return 0;
        try { return std::stoi(s); } catch (...) { return 0; }
    }
    float getf(const std::vector<std::string>& r, const char* name) const {
        std::string s = get(r, name);
        if (s.empty()) return 0;
        try { return std::stof(s); } catch (...) { return 0; }
    }
};

CsvTable readCsv(const std::string& path) {
    CsvTable t;
    std::ifstream f(path);
    if (!f) return t;
    std::string line;
    bool header = false;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto cells = splitCsvLine(line);
        if (!header) {
            for (size_t i = 0; i < cells.size(); ++i) t.col[cells[i]] = int(i);
            header = true;
        } else {
            t.rows.push_back(std::move(cells));
        }
    }
    t.ok = header;
    return t;
}

float parseMinutes(const std::string& s) {  // "mm:ss" or decimal minutes
    auto c = s.find(':');
    try {
        if (c == std::string::npos) return s.empty() ? 0.f : std::stof(s);
        return std::stof(s.substr(0, c)) + std::stof(s.substr(c + 1)) / 60.f;
    } catch (...) { return 0.f; }
}

std::string canonTeam(std::string a) {
    for (auto& ch : a) ch = char(std::toupper(unsigned(ch)));
    if (a == "VEG") return "VGK";
    if (a == "ARI" || a == "UTH") return "UTA";
    if (a == "TB") return "TBL";
    if (a == "NJ") return "NJD";
    if (a == "LA") return "LAK";
    if (a == "SJ") return "SJS";
    return a;
}

Pos parsePos(const std::string& p) {
    if (p == "C") return Pos::C;
    if (p == "LW" || p == "L") return Pos::LW;
    if (p == "RW" || p == "R" || p == "W") return Pos::RW;
    if (p == "D") return Pos::D;
    if (p == "G") return Pos::G;
    return Pos::F;
}

// ----------------------------------------------------------- Ratings -----

// Baselines for a regular NHL skater, per 60 minutes of ice time.
struct Baseline { float g60, g60sd, a60, a60sd, s60, s60sd, blk60, blk60sd, hit60, hit60sd, pim60, pim60sd, toi, toisd, shp, shpsd; };
constexpr Baseline kFwd{0.75f, 0.35f, 1.05f, 0.45f, 7.0f, 2.2f, 1.4f, 0.9f, 4.0f, 3.0f, 1.1f, 0.8f, 15.5f, 2.6f, 0.105f, 0.030f};
constexpr Baseline kDef{0.20f, 0.15f, 0.80f, 0.35f, 4.5f, 1.6f, 4.3f, 1.8f, 4.0f, 3.0f, 1.0f, 0.8f, 20.0f, 2.8f, 0.050f, 0.020f};
constexpr float kPriorMinutes = 400.f;  // ~1/4 season of a 20-min player; early-season numbers get pulled toward average

float toRating(float z) { return std::clamp(70.f + 9.f * z, 40.f, 99.f); }

void rateSkater(Player& p) {
    const Baseline& b = p.pos == Pos::D ? kDef : kFwd;
    float mins = std::max(1.f, p.gp * p.toiPerGame);
    float w = mins / (mins + kPriorMinutes);
    auto rate60 = [&](float count, float mean) { return w * (count * 60.f / mins) + (1 - w) * mean; };

    float g60 = rate60(float(p.g), b.g60), a60 = rate60(float(p.a), b.a60), s60 = rate60(float(p.s), b.s60);
    float blk60 = rate60(float(p.blk), b.blk60), hit60 = rate60(float(p.hit), b.hit60), pim60 = rate60(float(p.pim), b.pim60);
    float p60 = g60 + a60;
    const float kShotPrior = 80.f;
    float shp = (p.g + b.shp * kShotPrior) / (p.s + kShotPrior);

    float zG = (g60 - b.g60) / b.g60sd, zA = (a60 - b.a60) / b.a60sd, zS = (s60 - b.s60) / b.s60sd;
    float zP = (p60 - (b.g60 + b.a60)) / (b.g60sd + b.a60sd) * 1.4f;
    float zBlk = (blk60 - b.blk60) / b.blk60sd, zHit = (hit60 - b.hit60) / b.hit60sd, zPim = (pim60 - b.pim60) / b.pim60sd;
    float zToi = p.gp > 0 ? (p.toiPerGame - b.toi) / b.toisd : -0.5f;
    float zShp = (shp - b.shp) / b.shpsd;
    float pmPerGame = p.gp > 0 ? w * (float(p.pm) / p.gp) : 0.f;
    float zPm = pmPerGame / 0.35f;
    float age = float(p.age);
    float ageAdj = age <= 27 ? 0.04f * (std::min(age, 27.f) - 22.f) : -0.10f * std::max(0.f, age - 29.f);

    SkaterRatings& r = p.r;
    r.shooting = toRating(0.55f * zShp + 0.45f * zG);
    r.shotRate = toRating(zS);
    r.passing = toRating(zA);
    r.handling = toRating(0.5f * zP + 0.5f * zA);
    r.skating = toRating(0.35f * zP + 0.25f * zToi + 0.15f * zG + ageAdj);
    r.checking = toRating(zHit);
    r.discipline = toRating(-zPim);
    if (p.pos == Pos::D) r.defense = toRating(0.35f * zBlk + 0.40f * zToi + 0.25f * zPm);
    else r.defense = toRating(0.40f * zBlk + 0.35f * zPm + 0.25f * zToi);

    int fo = p.fow + p.fol;
    if (fo >= 30) {
        float fop = (p.fow + 50.f) / (fo + 100.f);
        r.faceoff = std::clamp(70.f + 9.f * (fop - 0.5f) / 0.035f, 40.f, 99.f);
    } else {
        r.faceoff = 50.f;
    }

    if (p.pos == Pos::D)
        r.overall = 0.30f * r.defense + 0.20f * r.passing + 0.15f * r.skating + 0.10f * r.shooting + 0.15f * r.handling + 0.10f * r.checking;
    else
        r.overall = 0.27f * r.shooting + 0.25f * r.passing + 0.15f * r.handling + 0.15f * r.skating + 0.12f * r.defense + 0.06f * r.checking;
}

void rateGoalie(Player& p) {
    const float kLeagueSv = 0.900f, kShotPrior = 600.f;
    float sv = (p.sv + kLeagueSv * kShotPrior) / (p.sa + kShotPrior);
    p.gr.svPct = sv;
    p.gr.saving = std::clamp(70.f + 9.f * (sv - kLeagueSv) / 0.008f, 40.f, 99.f);
    p.gr.overall = p.gr.saving;
}

Player makeDepth(const std::string& team, Pos pos, int n, Rng& rng) {  // n: per-position counter
    Player p;
    p.team = team;
    p.pos = pos;
    p.generated = true;
    p.age = rng.irange(22, 32);
    p.name = std::string("Depth ") + (pos == Pos::G ? "G" : pos == Pos::D ? "D" : "F") + std::to_string(n);
    if (pos == Pos::G) {
        p.gr.svPct = 0.900f + rng.normal(-0.002f, 0.004f);
        p.gr.saving = std::clamp(70.f + 9.f * (p.gr.svPct - 0.900f) / 0.008f, 40.f, 99.f);
        p.gr.overall = p.gr.saving;
        return p;
    }
    auto rr = [&](float mean) { return toRating(rng.normal(mean, 0.35f)); };
    SkaterRatings& r = p.r;
    r.shooting = rr(-0.6f); r.shotRate = rr(-0.4f); r.passing = rr(-0.6f); r.handling = rr(-0.6f);
    r.skating = rr(-0.4f); r.defense = rr(-0.2f); r.checking = rr(0.f); r.discipline = rr(0.f);
    r.faceoff = pos == Pos::C ? toRating(rng.normal(-0.3f, 0.4f)) : 50.f;
    if (pos == Pos::D)
        r.overall = 0.30f * r.defense + 0.20f * r.passing + 0.15f * r.skating + 0.10f * r.shooting + 0.15f * r.handling + 0.10f * r.checking;
    else
        r.overall = 0.27f * r.shooting + 0.25f * r.passing + 0.15f * r.handling + 0.15f * r.skating + 0.12f * r.defense + 0.06f * r.checking;
    return p;
}

void buildLines(Team& t, Rng& rng) {
    auto& P = t.players;
    std::vector<int> fwd, def, gk;
    for (int i = 0; i < int(P.size()); ++i) {
        if (P[i].pos == Pos::G) gk.push_back(i);
        else if (P[i].pos == Pos::D) def.push_back(i);
        else fwd.push_back(i);
    }
    int nf = 1, nd = 1, ng = 1;
    while (fwd.size() < 12) { P.push_back(makeDepth(t.info.abbr, fwd.size() % 3 == 0 ? Pos::C : Pos::F, nf++, rng)); fwd.push_back(int(P.size()) - 1); }
    while (def.size() < 6) { P.push_back(makeDepth(t.info.abbr, Pos::D, nd++, rng)); def.push_back(int(P.size()) - 1); }
    while (gk.size() < 2) { P.push_back(makeDepth(t.info.abbr, Pos::G, ng++, rng)); gk.push_back(int(P.size()) - 1); }

    auto byOverall = [&](int a, int b) { return P[a].r.overall > P[b].r.overall; };
    std::sort(fwd.begin(), fwd.end(), byOverall);
    std::sort(def.begin(), def.end(), byOverall);
    fwd.resize(std::min<size_t>(fwd.size(), 12));  // top 12 dress; the rest are scratches

    std::vector<bool> used(P.size(), false);
    auto takeBest = [&](auto pred) -> int {
        for (int i : fwd) if (!used[i] && pred(P[i])) { used[i] = true; return i; }
        for (int i : fwd) if (!used[i]) { used[i] = true; return i; }
        return fwd.front();
    };
    for (int l = 0; l < 4; ++l) {
        int c = takeBest([](const Player& p) { return p.pos == Pos::C || (p.fow + p.fol) >= 4 * std::max(1, p.gp) || (p.generated && p.r.faceoff > 50); });
        t.lines.fwd[l][0] = c;
    }
    for (int l = 0; l < 4; ++l) {
        t.lines.fwd[l][1] = takeBest([](const Player& p) { return p.pos == Pos::LW || p.pos == Pos::F; });
        t.lines.fwd[l][2] = takeBest([](const Player&) { return true; });
    }
    for (int d = 0; d < 3; ++d) { t.lines.def[d][0] = def[d * 2]; t.lines.def[d][1] = def[d * 2 + 1]; }

    std::sort(gk.begin(), gk.end(), [&](int a, int b) {
        if (P[a].generated != P[b].generated) return !P[a].generated;
        if (P[a].gp != P[b].gp) return P[a].gp > P[b].gp;
        return P[a].gr.overall > P[b].gr.overall;
    });
    t.lines.goalies = {gk[0], gk[1]};

    // Team strength summaries (ice-time weighted).
    const float fw[4] = {0.38f, 0.30f, 0.20f, 0.12f}, dw[3] = {0.42f, 0.35f, 0.23f};
    float off = 0, dfn = 0;
    for (int l = 0; l < 4; ++l)
        for (int s = 0; s < 3; ++s) {
            const auto& r = P[t.lines.fwd[l][s]].r;
            off += fw[l] / 3.f * (0.4f * r.shooting + 0.35f * r.passing + 0.25f * r.handling);
            dfn += fw[l] / 3.f * 0.4f * r.defense;
        }
    for (int d = 0; d < 3; ++d)
        for (int s = 0; s < 2; ++s) {
            const auto& r = P[t.lines.def[d][s]].r;
            dfn += dw[d] / 2.f * 0.6f * r.defense;
            off += 0.f;
        }
    t.offense = off;
    t.defense = dfn;
    t.goaltending = P[t.lines.goalies[0]].gr.overall;
    t.overall = 0.4f * t.offense + 0.3f * t.defense + 0.3f * t.goaltending;

    // Raw style index (skill minus physicality); loadLeague spreads it across the league.
    float skill = 0, phys = 0;
    for (int l = 0; l < 4; ++l)
        for (int s = 0; s < 3; ++s) {
            const auto& r = P[t.lines.fwd[l][s]].r;
            skill += fw[l] / 3.f * (0.4f * r.handling + 0.35f * r.passing + 0.25f * r.skating);
            phys += fw[l] / 3.f * r.checking;
        }
    for (int d = 0; d < 3; ++d)
        for (int s = 0; s < 2; ++s) {
            const auto& r = P[t.lines.def[d][s]].r;
            skill += dw[d] / 2.f * (0.5f * r.passing + 0.3f * r.handling + 0.2f * r.skating);
            phys += dw[d] / 2.f * r.checking;
        }
    t.style = 0.5f * skill - 0.25f * phys;  // both sums are weighted twice (forwards + D)
}

}  // namespace

League loadLeague(const std::string& dataDir, uint64_t seed) {
    League L;
    Rng rng(seed);
    for (const auto& ti : teamTable()) { Team t; t.info = ti; L.teams.push_back(std::move(t)); }

    int skaters = 0, goalies = 0, unknownTeam = 0;
    CsvTable sk = readCsv(dataDir + "/players.csv");
    if (sk.ok) {
        for (const auto& row : sk.rows) {
            Player p;
            p.team = canonTeam(sk.get(row, "team"));
            int ti = L.find(p.team);
            if (ti < 0) { ++unknownTeam; continue; }  // e.g. TOT rows for traded players
            p.name = sk.get(row, "name");
            p.pos = parsePos(sk.get(row, "pos"));
            if (p.pos == Pos::G) continue;
            p.age = sk.geti(row, "age");
            if (p.age <= 0) p.age = 27;
            p.gp = sk.geti(row, "gp"); p.g = sk.geti(row, "g"); p.a = sk.geti(row, "a"); p.pts = sk.geti(row, "pts");
            p.pm = sk.geti(row, "pm"); p.pim = sk.geti(row, "pim"); p.s = sk.geti(row, "s");
            p.blk = sk.geti(row, "blk"); p.hit = sk.geti(row, "hit"); p.fow = sk.geti(row, "fow"); p.fol = sk.geti(row, "fol");
            p.toiPerGame = parseMinutes(sk.get(row, "atoi"));
            if (p.toiPerGame <= 0) p.toiPerGame = p.pos == Pos::D ? 19.f : 15.f;
            rateSkater(p);
            L.teams[ti].players.push_back(std::move(p));
            ++skaters;
        }
    }
    CsvTable gk = readCsv(dataDir + "/goalies.csv");
    if (gk.ok) {
        for (const auto& row : gk.rows) {
            Player p;
            p.team = canonTeam(gk.get(row, "team"));
            int ti = L.find(p.team);
            if (ti < 0) { ++unknownTeam; continue; }
            p.name = gk.get(row, "name");
            p.pos = Pos::G;
            p.age = gk.geti(row, "age");
            p.gp = gk.geti(row, "gp"); p.sa = gk.geti(row, "sa"); p.sv = gk.geti(row, "sv"); p.ga = gk.geti(row, "ga");
            if (p.sv == 0 && p.sa > 0) p.sv = p.sa - p.ga;
            p.goalieMinutes = gk.getf(row, "toi_min");
            rateGoalie(p);
            L.teams[ti].players.push_back(std::move(p));
            ++goalies;
        }
    }

    int generated = 0;
    for (auto& t : L.teams) {
        t.realPlayers = int(t.players.size());
        buildLines(t, rng);
        for (const auto& p : t.players) generated += p.generated ? 1 : 0;
    }
    // Spread the raw style index to 0.15-0.85 by z-score, so every league has both kinds of team.
    {
        double m = 0, v = 0;
        for (const auto& t : L.teams) m += t.style;
        m /= double(L.teams.size());
        for (const auto& t : L.teams) v += (t.style - m) * (t.style - m);
        float sd = float(std::sqrt(v / double(L.teams.size())));
        for (auto& t : L.teams) t.style = std::clamp(0.5f + 0.17f * (t.style - float(m)) / std::max(sd, 0.01f), 0.15f, 0.85f);
    }

    std::ostringstream os;
    os << "Loaded " << skaters << " skaters and " << goalies << " goalies from " << dataDir
       << "; generated " << generated << " depth players to fill lineups";
    if (unknownTeam) os << "; skipped " << unknownTeam << " rows with unknown team";
    if (!sk.ok) os << " (players.csv not found)";
    if (!gk.ok) os << " (no goalies.csv: run tools/fetch_nhl_stats.py for real goalies)";
    L.summary = os.str();
    return L;
}

}  // namespace hk
