# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An NHL game simulation (viewable in top-down 2D or 3D) in C++17 (CMake, raylib 5.5). Players are rated from current box-score stats; each match is a real-time physics + AI simulation, not a score dice roll. See README.md for the rating formulas, gameplay description and calibration table.

## Commands

```bash
cmake -S . -B build                       # first configure fetches raylib 5.5 if not installed
cmake --build build --config Release
./build/nhl_sim                           # viewer (Windows: build\Release\nhl_sim.exe)
cmake -S . -B build -DNHL_SIM_VIEWER=OFF  # core + headless_sim only, no raylib
```

There is no unit-test suite or linter. Verification is done with the headless runner (`tools/headless_sim.cpp`):

```bash
./build/headless_sim data 400             # 400 games, prints league-wide averages vs NHL targets
./build/headless_sim data --box EDM COL   # one full box score (random seed)
./build/headless_sim data --ratings       # team/player ratings
./build/headless_sim data --season        # 41-home-game schedule + standings (~10 min)
```

CI (`.github/workflows/ci.yml`) runs `headless_sim data 100 --check` (fails if league averages leave broad sanity ranges), builds the viewer on Linux/macOS/Windows, and on Linux renders 2D/3D screenshots plus a `--script` live game under xvfb (uploaded as an artifact). If a deliberate tuning change moves averages outside the `--check` ranges, widen them in `tools/headless_sim.cpp`.

- Batch games use fixed seeds (`i * 7919 + 17`), so calibration output is reproducible for a given code + data state. Re-run calibration after any gameplay change and compare with the README table.
- Define `HK_DIAG` (e.g. `-DCMAKE_CXX_FLAGS=-DHK_DIAG`) to collect shot-quality/goalie-positioning diagnostics (`g_diag` in `Game.cpp`), printed by headless_sim.
- Viewer screenshot mode for checking rendering without interaction: `./build/nhl_sim --data data --home EDM --away COL --screenshot out.png --after 600` (600 sim seconds); add `--view 3d|wide|end` to capture a 3D camera. `--script "2:v,4:shot,5:c,...,12:quit"` plays a live game and fires keyboard actions at wall-clock seconds (v, c, n, z±N, s1–s5, shot, quit), saving `<--script-out>_<k>.png` and logging camera state/FPS; use it to test interactive viewer changes.
- `python3 tools/fetch_nhl_stats.py` refreshes `data/players.csv` and writes `data/goalies.csv` from the unofficial public NHL APIs (stdlib only). The `data/` folder is copied next to `nhl_sim` on every build.

## Architecture

Three CMake targets: `hkcore` (static lib, `src/core/`, **no graphics dependencies**: keep raylib out of it), `headless_sim`, and `nhl_sim` (`src/viewer/main.cpp`, raylib). The viewer has two renderers over the same `Game` state: 2D (`drawRink`/`drawBodies`) and 3D (`draw3D`; sim `(x, y)` → world `(x, height, -y)` in feet, with an off-centre frustum so the camera aims at the strip between the top bar and bottom panel). The sim has no puck height, so the 3D view is purely visual. Everything is in `namespace hk`; includes are rooted at `src/` (`#include "core/Game.h"`).

**League loading (`League.cpp`)** — `loadLeague(dataDir, seed)` reads the CSVs, converts per-60 stats into 40–99 ratings (70 = average regular) with Bayesian-style shrinkage toward positional priors, pads each of the 32 teams with generated `"Depth ..."` players (`Player::generated`) to 12F/6D/2G, and builds `Lineup` (4 forward lines, 3 D pairs, starter/backup). `goalies.csv` is optional; without it all goalies are generated. Team metadata (abbr, colors) comes from `teamTable()`.

**Game (`Game.h/.cpp`)** — a `Game` holds a `const League&` and runs a 120 Hz fixed-step sim; callers drive it with `advance(seconds)` (viewer, scaled by speed) or `playToEnd()` (headless). Key data model:
- `bodies_`: exactly 12 `Body` slots, indexed `team*6 + slot` with slot 0 C, 1 LW, 2 RW, 3 LD, 4 RD, 5 G. Line changes and penalties reassign `roster` indices into these fixed slots (`assignLineup`) and toggle `active` for shorthanded play; players are swapped in place, there's no bench.
- `tg_[2]` (`TeamGame`): side 0 = home, 1 = away. Per-game stats, energy and penalty box are vectors indexed by **roster index** into `Team::players`. `dir` flips each period; use `attackNet()/ownNet()` instead of hardcoding x signs.
- `Phase` state machine in `tick()`: Intermission → FaceoffSetup → Play ⇄ Stoppage → … → Final. During Play the per-tick order is penalty clocks → line changes → `teamAI` (both sides) → `moveBodies` → `updatePuck` → `puckControl` → `pressure`, bailing out whenever a whistle or goal changes the phase.
- Rink coordinates are in feet, origin at center ice (`namespace rink` in `Game.h`).
- `events_` is the play-by-play log; both front ends render box scores from `events()` and `side(i).stats`.

**Tuning** — balance constants live in `namespace tune` at the top of `Game.cpp` (shot urge, goal scale, hit/poke/block rates, penalty chances), plus shift lengths `kFShift`/`kDShift`. Rating-derivation constants (priors, shrinkage) are in `League.cpp`.

## Known gaps (from README)

No offside; power-play %, blocked shots and OT frequency run below NHL targets; skating is a proxy rating. Icings run low (about 0.3 per team) now that panic clears are rare and passes rarely miss, and the shot gap between strong and weak teams is wider than the NHL's.
