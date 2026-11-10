// Headless batch runner: plays many games without graphics and compares league-wide
// output with NHL averages. Also prints team ratings and an optional season table.
//
//   headless_sim [data_dir] [games] [--season] [--ratings] [--box HOME AWAY] [--check]
//
// --check exits non-zero when league averages fall outside broad sanity ranges (used by CI).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/Game.h"
#include "core/League.h"

using namespace hk;

static void printBox(const Game& g) {
    for (const auto& e : g.events())
        if (e.type == EventType::Goal || e.type == EventType::Penalty || e.type == EventType::PeriodEnd || e.type == EventType::Final ||
            e.type == EventType::Shootout) {
            int t = int(std::ceil(e.clock));
            std::printf("  P%d %2d:%02d  %s\n", e.period, t / 60, t % 60, e.text.c_str());
        }
    for (int s : {1, 0}) {
        const auto& T = g.side(s);
        std::printf("\n%s %s  SOG %d  PP %d/%d  FO %d  Hits %d  Blk %d\n", g.team(s).info.city, g.team(s).info.name, T.sog, T.ppGoals, T.ppOpp,
                    T.fow, T.hits, T.blocks);
        std::printf("  %-26s %3s %3s %3s %3s %4s %3s %6s\n", "Player", "G", "A", "SOG", "+/-", "HIT", "PIM", "TOI");
        for (size_t r = 0; r < T.stats.size(); ++r) {
            const auto& L = T.stats[r];
            const auto& P = g.player(s, int(r));
            if (L.toi <= 0 || P.isGoalie()) continue;
            std::printf("  %-26s %3d %3d %3d %+3d %4d %3d %3d:%02d\n", P.name.c_str(), L.g, L.a, L.sog, L.pm, L.hits, L.pim, int(L.toi) / 60,
                        int(L.toi) % 60);
        }
        const auto& G = T.stats[size_t(T.goalie)];
        std::printf("  G %-24s SA %d  SV %d  GA %d\n", g.player(s, T.goalie).name.c_str(), G.sa, G.sv, G.ga);
    }
}

int main(int argc, char** argv) {
    std::string dataDir = "data";
    int games = 500;
    bool season = false, ratings = false, check = false;
    std::string boxHome, boxAway;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--season")) season = true;
        else if (!std::strcmp(argv[i], "--ratings")) ratings = true;
        else if (!std::strcmp(argv[i], "--check")) check = true;
        else if (!std::strcmp(argv[i], "--box") && i + 2 < argc) { boxHome = argv[i + 1]; boxAway = argv[i + 2]; i += 2; }
        else pos.push_back(argv[i]);
    }
    if (!pos.empty()) dataDir = pos[0];
    if (pos.size() > 1) games = std::max(1, std::atoi(pos[1].c_str()));

    League L = loadLeague(dataDir);
    std::printf("%s\n", L.summary.c_str());

    if (ratings) {
        std::vector<int> idx(L.teams.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = int(i);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return L.teams[size_t(a)].overall > L.teams[size_t(b)].overall; });
        std::printf("\n%-4s %5s %5s %5s %5s %5s  %s\n", "Team", "OVR", "OFF", "DEF", "G", "STYLE", "Top player");
        for (int i : idx) {
            const Team& t = L.teams[size_t(i)];
            const Player& top = t.players[size_t(t.lines.fwd[0][0])];
            std::printf("%-4s %5.1f %5.1f %5.1f %5.1f %5.2f  %s (%.0f)  real players: %d\n", t.info.abbr, t.overall, t.offense, t.defense, t.goaltending, t.style,
                        top.name.c_str(), top.r.overall, t.realPlayers);
        }
    }

    if (!boxHome.empty()) {
        int h = L.find(boxHome), a = L.find(boxAway);
        if (h < 0 || a < 0) { std::fprintf(stderr, "Unknown team\n"); return 1; }
        GameOptions o;
        o.seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        Game g(L, h, a, o);
        g.playToEnd();
        printBox(g);
        return 0;
    }

    struct Rec { int gp = 0, w = 0, l = 0, otl = 0, gf = 0, ga = 0; };
    std::vector<Rec> rec(L.teams.size());
    long entC = 0, entP = 0, entD = 0, passes = 0, passesDone = 0;
    long goals = 0, sog = 0, att = 0, hits = 0, blocks = 0, ppOpp = 0, ppG = 0, icing = 0, ot = 0, so = 0, saves = 0, shotsAgainst = 0;
    auto t0 = std::chrono::steady_clock::now();
    int n = season ? int(L.teams.size()) * 41 : games;
    for (int i = 0; i < n; ++i) {
        int h, a;
        if (season) { h = i % int(L.teams.size()); a = (h + 1 + (i / int(L.teams.size())) % (int(L.teams.size()) - 1)) % int(L.teams.size()); }
        else { h = i % 32; a = (h + 1 + (i * 7) % 31) % 32; }
        GameOptions o;
        o.seed = uint64_t(i) * 7919 + 17;
        Game g(L, h, a, o);
        g.playToEnd();
        for (int s = 0; s < 2; ++s) {
            const auto& T = g.side(s);
            goals += T.score - (g.wentToShootout() && T.score > g.side(1 - s).score ? 1 : 0);
            entC += T.entCarry; entP += T.entPass; entD += T.entDump; passes += T.passes; passesDone += T.passesDone;
            sog += T.sog; att += T.attempts; hits += T.hits; blocks += T.blocks; ppOpp += T.ppOpp; ppG += T.ppGoals; icing += T.icings;
            const auto& G = T.stats[size_t(T.goalie)];
            saves += G.sv; shotsAgainst += G.sa;
        }
        ot += g.wentToOvertime();
        so += g.wentToShootout();
        int ti[2] = {h, a};
        for (int s = 0; s < 2; ++s) {
            Rec& r = rec[size_t(ti[s])];
            int mine = g.side(s).score, theirs = g.side(1 - s).score;
            r.gp++; r.gf += mine; r.ga += theirs;
            if (mine > theirs) r.w++;
            else if (g.wentToOvertime()) r.otl++;
            else r.l++;
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double tg = 2.0 * n;
    std::printf("\n%d games in %.1fs (%.0f games/s)\n", n, secs, n / secs);
    std::printf("%-26s %8s %10s\n", "per team per game", "sim", "NHL ~");
    std::printf("%-26s %8.2f %10s\n", "goals (excl. SO winner)", goals / tg, "3.0-3.1");
    std::printf("%-26s %8.1f %10s\n", "shots on goal", sog / tg, "28-29");
    std::printf("%-26s %8.1f %10s\n", "shot attempts (all)", att / tg, "55-60");
    std::printf("%-26s %8.1f %10s\n", "blocked shots", blocks / tg, "13-15");
    std::printf("%-26s %8.1f %10s\n", "hits", hits / tg, "20-24");
    std::printf("%-26s %8.2f %10s\n", "power plays", ppOpp / tg, "2.8-3.2");
    std::printf("%-26s %7.1f%% %10s\n", "power-play %", ppOpp ? 100.0 * ppG / ppOpp : 0.0, "20-22%");
    std::printf("%-26s %8.2f %10s\n", "icings", icing / tg, "");
    std::printf("%-26s %8.3f %10s\n", "save %", shotsAgainst ? double(saves) / shotsAgainst : 0.0, ".900-.905");
    std::printf("%-26s %7.1f%% %10s\n", "games to OT", 100.0 * ot / n, "22-25%");
    std::printf("%-26s %7.1f%% %10s\n", "games to shootout", 100.0 * so / n, "8-10%");
    long ent = std::max(1L, entC + entP + entD);
    std::printf("%-26s %8.1f %10s\n", "zone entries", ent / tg, "");
    std::printf("%-26s %7.0f%% %10s\n", "  carried in", 100.0 * entC / ent, "");
    std::printf("%-26s %7.0f%% %10s\n", "  passed in", 100.0 * entP / ent, "");
    std::printf("%-26s %7.0f%% %10s\n", "  dumped/chipped in", 100.0 * entD / ent, "~45-55%");
    std::printf("%-26s %8.1f %10s\n", "passes", passes / tg, "");
    std::printf("%-26s %7.0f%% %10s\n", "  completed", passes ? 100.0 * passesDone / passes : 0.0, "");

#ifdef HK_DIAG
    for (int k = 0; k < 2; ++k)
        if (g_diag.n[k]) std::printf("diag %s: n/game %.1f  q %.3f  posF %.2f  pGoal %.3f\n", k ? "shot" : "non-shot", g_diag.n[k] / double(n),
                                     g_diag.q[k] / g_diag.n[k], g_diag.posF[k] / g_diag.n[k], g_diag.pg[k] / g_diag.n[k]);
    if (g_diag.n[1]) {
        std::printf("diag shot dist avg %.1f ft; by 10ft bins:", g_diag.dist / g_diag.n[1]);
        for (long v : g_diag.hist) std::printf(" %.0f%%", 100.0 * v / g_diag.n[1]);
        std::printf("\ndiag mods (share of attempts): one-timer %.2f rebound %.2f screen %.2f rush %.2f\n", double(g_diag.mods[0]) / g_diag.mods[4],
                    double(g_diag.mods[1]) / g_diag.mods[4], double(g_diag.mods[2]) / g_diag.mods[4], double(g_diag.mods[3]) / g_diag.mods[4]);
    }
#endif
    if (check) {
        // Deliberately wide: these catch a broken sim, not tuning drift.
        struct Range { const char* name; double v, lo, hi; };
        const Range ranges[] = {
            {"goals", goals / tg, 2.2, 4.3},
            {"shots on goal", sog / tg, 22, 42},
            {"shot attempts", att / tg, 42, 78},
            {"hits", hits / tg, 12, 36},
            {"power plays", ppOpp / tg, 1.5, 5.0},
            {"icings", icing / tg, 0.5, 10},
            {"save %", shotsAgainst ? double(saves) / shotsAgainst : 0.0, 0.870, 0.930},
            {"games to OT", double(ot) / n, 0.03, 0.40},
        };
        int bad = 0;
        for (const Range& r : ranges)
            if (!(r.v >= r.lo && r.v <= r.hi)) { std::printf("CHECK FAILED: %s = %.3f, expected %.3f-%.3f\n", r.name, r.v, r.lo, r.hi); ++bad; }
        if (bad) return 1;
        std::printf("check: all league averages within sanity ranges\n");
    }
    if (season) {
        std::vector<int> idx(rec.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = int(i);
        auto pts = [&](int i) { return 2 * rec[size_t(i)].w + rec[size_t(i)].otl; };
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return pts(a) > pts(b); });
        std::printf("\n%-4s %3s %3s %3s %3s %4s %4s %4s  %5s\n", "Team", "GP", "W", "L", "OTL", "PTS", "GF", "GA", "OVR");
        for (int i : idx) {
            const Rec& r = rec[size_t(i)];
            std::printf("%-4s %3d %3d %3d %3d %4d %4d %4d  %5.1f\n", L.teams[size_t(i)].info.abbr, r.gp, r.w, r.l, r.otl, pts(i), r.gf, r.ga,
                        L.teams[size_t(i)].overall);
        }
    }
    return 0;
}
