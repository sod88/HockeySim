#!/usr/bin/env python3
"""Refresh data/players.csv and data/goalies.csv from the public NHL stats APIs.

Pulls current-season regular-season stats for every skater and goalie, assigns each
player to the team whose current roster lists him, and writes the two CSV files the
engine reads. Standard library only.

    python3 tools/fetch_nhl_stats.py                 # current season, writes ./data
    python3 tools/fetch_nhl_stats.py --season 20262027 --out data

Endpoints used (unofficial but public):
    https://api-web.nhle.com/v1/standings/now            -> the 32 team abbreviations
    https://api-web.nhle.com/v1/roster/{TEAM}/current    -> who is on each team, age
    https://api.nhle.com/stats/rest/en/skater/{report}   -> summary, realtime, faceoffwins
    https://api.nhle.com/stats/rest/en/goalie/summary
"""
import argparse
import csv
import datetime as dt
import json
import os
import sys
import time
import urllib.parse
import urllib.request

WEB = "https://api-web.nhle.com/v1"
REST = "https://api.nhle.com/stats/rest/en"
UA = {"User-Agent": "nhl-sim-stats-fetcher/1.0"}


def get_json(url, tries=3):
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read().decode("utf-8"))
        except Exception as e:  # noqa: BLE001 - report and retry
            if i == tries - 1:
                raise RuntimeError(f"GET {url} failed: {e}") from e
            time.sleep(1.5 * (i + 1))


def current_season(today=None):
    today = today or dt.date.today()
    y = today.year if today.month >= 9 else today.year - 1
    return f"{y}{y + 1}"


def rest_report(kind, report, season):
    exp = f"gameTypeId=2 and seasonId<={season} and seasonId>={season}"
    q = urllib.parse.urlencode({"isAggregate": "false", "isGame": "false", "start": 0, "limit": -1, "cayenneExp": exp})
    data = get_json(f"{REST}/{kind}/{report}?{q}")
    return {row["playerId"]: row for row in data.get("data", [])}


def name_of(obj, key):
    v = obj.get(key, "")
    return v.get("default", "") if isinstance(v, dict) else (v or "")


def age_on(birth, today):
    try:
        b = dt.date.fromisoformat(birth[:10])
    except (TypeError, ValueError):
        return ""
    return today.year - b.year - ((today.month, today.day) < (b.month, b.day))


def mmss(seconds):
    s = int(round(float(seconds or 0)))
    return f"{s // 60}:{s % 60:02d}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--season", default=current_season(), help="e.g. 20262027 (default: current)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data"))
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    today = dt.date.today()
    print(f"Season {args.season} -> {out}")

    standings = get_json(f"{WEB}/standings/now")
    teams = sorted({name_of(t, "teamAbbrev") for t in standings.get("standings", [])})
    if len(teams) != 32:
        print(f"warning: standings returned {len(teams)} teams", file=sys.stderr)

    # Roster membership decides the team for traded players.
    roster_team, roster_age, roster_pos, roster_name = {}, {}, {}, {}
    for abbr in teams:
        r = get_json(f"{WEB}/roster/{abbr}/current")
        for group in ("forwards", "defensemen", "goalies"):
            for p in r.get(group, []):
                pid = p["id"]
                roster_team[pid] = abbr
                roster_age[pid] = age_on(p.get("birthDate"), today)
                roster_pos[pid] = p.get("positionCode", "")
                roster_name[pid] = f"{name_of(p, 'firstName')} {name_of(p, 'lastName')}".strip()
        time.sleep(0.2)
    print(f"Rosters: {len(roster_team)} players on {len(teams)} teams")

    summary = rest_report("skater", "summary", args.season)
    realtime = rest_report("skater", "realtime", args.season)
    faceoffs = rest_report("skater", "faceoffwins", args.season)
    goalies = rest_report("goalie", "summary", args.season)

    def team_for(pid, row):
        if pid in roster_team:
            return roster_team[pid]
        abbrs = (row.get("teamAbbrevs") or "").split(",")
        return abbrs[-1].strip() if abbrs and abbrs[-1].strip() else None

    n_sk = 0
    with open(os.path.join(out, "players.csv"), "w", newline="", encoding="utf-8") as f:
        f.write(f"# NHL {args.season} regular-season skater stats, fetched {today.isoformat()} by tools/fetch_nhl_stats.py\n")
        w = csv.writer(f)
        w.writerow(["team", "name", "pos", "age", "gp", "g", "a", "pts", "pm", "pim", "s", "atoi", "blk", "hit", "fow", "fol"])
        for pid, s in sorted(summary.items(), key=lambda kv: -(kv[1].get("points") or 0)):
            team = team_for(pid, s)
            if not team or not s.get("gamesPlayed"):
                continue
            rt, fo = realtime.get(pid, {}), faceoffs.get(pid, {})
            w.writerow([
                team, s.get("skaterFullName") or roster_name.get(pid, ""), s.get("positionCode") or roster_pos.get(pid, "F"),
                roster_age.get(pid, ""), s.get("gamesPlayed", 0), s.get("goals", 0), s.get("assists", 0), s.get("points", 0),
                s.get("plusMinus", 0), s.get("penaltyMinutes", 0), s.get("shots", 0), mmss(s.get("timeOnIcePerGame")),
                rt.get("blockedShots", 0), rt.get("hits", 0), fo.get("totalFaceoffWins", 0), fo.get("totalFaceoffLosses", 0),
            ])
            n_sk += 1

    n_g = 0
    with open(os.path.join(out, "goalies.csv"), "w", newline="", encoding="utf-8") as f:
        f.write(f"# NHL {args.season} regular-season goalie stats, fetched {today.isoformat()} by tools/fetch_nhl_stats.py\n")
        w = csv.writer(f)
        w.writerow(["team", "name", "age", "gp", "gs", "w", "l", "otl", "sa", "sv", "ga", "toi_min"])
        for pid, g in sorted(goalies.items(), key=lambda kv: -(kv[1].get("gamesPlayed") or 0)):
            team = team_for(pid, g)
            if not team or not g.get("gamesPlayed"):
                continue
            w.writerow([
                team, g.get("goalieFullName") or roster_name.get(pid, ""), roster_age.get(pid, ""), g.get("gamesPlayed", 0),
                g.get("gamesStarted", 0), g.get("wins", 0), g.get("losses", 0), g.get("otLosses", 0), g.get("shotsAgainst", 0),
                g.get("saves", 0), g.get("goalsAgainst", 0), round(float(g.get("timeOnIce") or 0) / 60.0, 1),
            ])
            n_g += 1

    print(f"Wrote {n_sk} skaters and {n_g} goalies")


if __name__ == "__main__":
    main()
