# NHL Sim — C++ hockey simulation engine

A watchable NHL game, in top-down 2D or a 3D arena view. Every skater and goalie is rated from current
(2026-27) box-score stats; the match itself is a real-time physics and AI simulation,
not a dice roll on the final score. C++17, CMake, raylib. Builds on macOS and Windows.

```
src/core/      simulation engine (no graphics): ratings, rink physics, AI, rules, stats
src/viewer/    raylib front end: team picker, live rink view (2D or 3D), box score
tools/         headless_sim (batch games + calibration), fetch_nhl_stats.py (data refresh)
data/          players.csv (+ goalies.csv after you run the fetcher)
```

## Build and run

**macOS:** Xcode command line tools and CMake (`brew install cmake`).
**Windows:** Visual Studio 2022 with "Desktop development with C++" (it includes CMake).

```bash
cmake -S . -B build
cmake --build build --config Release
./build/nhl_sim                       # Windows: build\Release\nhl_sim.exe
```

CMake downloads raylib 5.5 on the first configure if it isn't installed. The `data`
folder is copied next to the executable after each build.

### Controls

| Screen | Keys |
| --- | --- |
| Menu | Up/Down pick a team, Left/Right switch Away/Home, `R` random matchup, Enter start (mouse works too) |
| Game | Space pause, `1`–`5` speed (1x 2x 4x 8x 30x), `N` names, `B` box score, `F` finish instantly, Esc menu |
| 3D view | `V` switch 2D/3D, `C` cycle camera (broadcast, wide, end zone), mouse wheel zoom |
| Final | Enter for a new game |

## Getting full, current rosters

The bundled `data/players.csv` is a snapshot of the top ~220 scorers as of Nov 4, 2026.
Teams are filled out with generated "Depth" players, and both goalies on every team
are generated. For complete rosters and real goalies:

```bash
python3 tools/fetch_nhl_stats.py      # writes data/players.csv and data/goalies.csv
```

It uses the public NHL APIs (`api-web.nhle.com`, `api.nhle.com/stats/rest`), Python
standard library only. Rebuild or copy `data/` next to the executable afterwards.
The script was tested against mocked API responses; the live endpoints are
unofficial and could change field names.

## How stats become ratings (`src/core/League.cpp`)

Per-60 rates for each stat, regressed toward a positional average (400 minutes of
"prior" ice time, so 15 games in, a hot streak counts about half), then turned into
a 40–99 scale where 70 is an average regular.

| Rating | From |
| --- | --- |
| Shooting | shooting % (shrunk with 80 shots of prior) + goals/60 |
| Shot rate | shots/60 — how often he lets it go |
| Passing / Handling | assists/60, points/60 |
| Defense | blocks/60, +/- per game, ice time (weighted more for D) |
| Checking | hits/60 |
| Faceoff | faceoff % (shrunk); 50 if he rarely takes draws |
| Discipline | penalty minutes/60 (drives penalty rate) |
| Skating | proxy: scoring pace, ice time and age curve (box scores have no speed data) |
| Goalie | save % shrunk toward .900 with 600 shots of prior |

Lines are built from the ratings: top 12 forwards (centers by position or draw volume),
top 6 D in pairs, starter = the goalie with the most games.

## How a game plays (`src/core/Game.cpp`)

- 120 Hz fixed-step simulation on a 200×85 ft rink with rounded boards, solid nets,
  puck friction and board bounces.
- Team AI: puck carrier decides every ~0.2 s to shoot, pass, dump, clear or carry
  (perimeter play when the lane to the net is closed). Teammates fill offensive spots
  (net-front, high slot, points, weak side) or breakout lanes; the defending team
  pressures the carrier and marks goal-side.
- Shots: release quality from distance, angle and shooter rating, plus one-timers,
  rebounds, screens and rushes. Shots can be blocked, miss the net, or reach the goalie,
  whose save chance depends on his save % and on whether he's set in position (cross-ice
  passes beat goalies who can't slide in time).
- Hits, stick checks, minor penalties (rate from discipline), power plays (top unit
  goes out), line changes on shift timers with fatigue, faceoffs, 3-on-3 overtime and
  a shootout.
- Hybrid icing: a puck sent from behind the red line that crosses the far goal line
  untouched is whistled unless an attacker wins the race to it; faceoff in the offending
  team's zone and they can't change lines. Shorthanded teams may ice it. The AI sizes
  clears and passes from its own half so they die before the goal line.

### Calibration (200 headless games, `./build/headless_sim data 200`)

| Per team per game | Sim | NHL |
| --- | --- | --- |
| Goals | 2.79 | 3.0–3.1 |
| Shots on goal | 28.4 | 28–29 |
| Shot attempts | 52.1 | 55–60 |
| Blocked shots | 11.2 | 13–15 |
| Hits | 20.9 | 20–24 |
| Power plays | 2.79 | 2.8–3.2 |
| Power-play % | 15.7% | 20–22% |
| Icings | 1.44 | |
| Save % | .901 | .900–.905 |
| Games to OT | 19.0% | 22–25% |
| Zone entries | 49.0 | |
| Carried in / passed in / dumped in | 19% / 44% / 37% | |
| Pass completion | 73% | |

Measured with `shootUrge = 0.024`; it was then nudged to 0.026 to lift goals and attempts
slightly, so re-run before trusting the last digit.

### Play selection and strategy

- The puck carrier weighs carrying, passing, dumping it in and clearing it against each other,
  from the pressure on him, the room ahead, how many defenders are standing up at the far blue
  line, his own skill, shift fatigue, the team's style and the score and clock.
- Each team has a style from dump-and-chase to puck possession (`Team::style`, from roster skill
  vs physicality; listed by `headless_sim --ratings`). Possession teams carry and pass the puck
  in more; dump-and-chase teams get it deep and forecheck harder.
- Late in the third period a team protecting a lead chips pucks out, avoids risky passes and
  sits back, while a team chasing the game shoots more, crashes the net and pinches.
- Defenders keep a gap through the neutral zone and stand the rush up at their blue line; a
  second forward supports the forecheck when the plan is aggressive enough.
- The viewer shows each team's current plan under the score bar.

Tuning knobs live in `namespace tune` at the top of `Game.cpp`.
`headless_sim --box EDM COL` prints one full box score; `--ratings` lists team ratings;
`--season` plays a 41-home-game schedule (about 10 minutes).

## Known gaps / next steps

- No offside yet; line changes swap players in place instead of using the bench door.
- Power plays convert low, blocks run a little low and OT is a little rare; all are tuning targets.
- Skating speed is a proxy until a tracking-data source is added.
- Ideas: a season mode with standings, trades and injuries; a player-controlled skater;
  real jersey numbers and depth-chart overrides from a config file.
