# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B Bouncer_CPP_build
& "C:\Program Files\CMake\bin\cmake.exe" --build Bouncer_CPP_build --config Release
.\Bouncer_CPP_build\Release\Bouncer_CPP.exe
```

There are no tests. The binary is the verification step. SDL2 and nlohmann_json
are pulled by FetchContent; a post-build step copies `SDL2.dll` next to the exe.

## Architecture

Two files. `src/Config.hpp` is the header-only config layer, `src/main.cpp` is
the whole game.

**Config (`src/Config.hpp`):** `load_config()` walks `config_search_paths()` —
cwd first, then exe-relative with one and two `..` hops — and uses the first
`config.json` it finds, so the copy at the project root serves both `cmake`-dir
runs and a double-clicked exe. Every field is read with `value(key, default)`
against the `Config` struct's defaults, so a partial or absent file still runs.
Colors go through `parse_hex_color`, which returns the fallback on a bad string
instead of throwing — one typo should not cost the other three colors. After
loading, sizes are clamped so the simulation stays solvable (square is never
smaller than the circle, never bigger than the window).

**Game (`src/main.cpp`):** `World` holds the square by top-left corner *and its
own width and height* — the square closes in as the game runs, so its size is
state, not config — the circle by center, plus the phase state that drives the
hit sequence.
`make_world()` centers the square in the window, then places the circle by
lerping `circle_start_x/y` across the square's *interior* span
(`square_w - circle_diameter`) rather than its full width — the config clamps
those fractions to `[0, 1]` and forces the square to be at least circle-sized,
so the placement can never start the ball clipped into a wall. The loop is a fixed 120 Hz accumulator with a 0.25 s frame cap
and vsync'd presentation; input is read from `SDL_GetKeyboardState` per frame,
not from events, so held keys work.

The ball draws and collides on two different circles. `ball_radius()` is what
you see; `ball_collider()` is that times `circle.collider_scale` (0.95 by
default, clamped away from zero so it can't tunnel), and it is what
`confine_circle_to_square()`, `ball_inside_square()`, the diamond's reach test
and the shrink floor all measure against, so the pink overlaps a wall slightly
instead of stopping short of it. Anything drawn — the ball, its shards, the eye
— uses the full radius.

`step()` is a switch on `World::phase`. `Play` is the ordinary one: it moves the
square (diagonals normalized, clamped to the window),
integrates the circle, then calls `confine_circle_to_square()`. That last pass
is what makes both interactions work with one piece of code: it clamps the
circle to the square's inner bounds and flips the velocity component *away* from
whichever wall was crossed, so a circle hitting a wall bounces and a wall driven
into the circle knocks it away. It also *reports* the touch, which is what the
damage rule keys off.

**The hit sequence.** A touch only costs something when the square was moving
that tick (`square_moving`, taken from the raw input before the diagonal
normalization) and `World::grace` has expired — an idle bounce is free, and the
`kHitGrace` window after a hit keeps a held key from eating every dot at once.
A hit runs `Play → Shake → Play`: the whole world freezes for `kShakeTime` while
`render()` offsets the ball by a decaying sine rattle, then `drop_next_dot()`
knocks the rightmost dot in the top row loose. `update_dots()` runs in *every*
phase, so that dot keeps arcing off the bottom of the screen while play resumes
around it.

When `drop_next_dot()` takes the last one, the sequence continues
`Shake → Burst → FadeOut → Black → FadeIn → Play` instead: `burst_ball()` clears
`ball_alive` and calls `fan_shards()`, which throws a `std::array` of shards out
on an even fan at staggered speeds and sizes (index-derived, so no RNG) —
the ball and the triangles each own one such array — unconfined by the square;
`Burst` ends when the last shard is off screen. The reset happens at the end of
`FadeOut` (`w = make_world(cfg)`), behind full black, so `FadeIn` reveals a game
already back in its starting state. Phase lengths are the `k*Time` constants at
the top of the file.

**The shrinking square.** `shrink_square()` takes `square.shrink_rate` pixels
off `World::square_w`/`square_h` per second and moves the corner by half of
that, so the walls close in evenly around the square's own center instead of the
box crawling one way. It is called from `Phase::Play` only, and only while
`boost` is spent — which is the whole pause rule: a diamond's speed boost holds
the walls, and so does every star phase, since none of them run through `Play`.
The floor is `square.min_size` or the ball's current diameter, whichever is
larger, so the circle always fits however much diamonds have grown it; the
config clamps `min_size` between `circle.diameter` and the starting square. Note
that a closing wall touching the ball costs nothing — the damage rule keys off
`square_moving`, which is read from the keyboard, not from the shrink.

**The pickup.** `update_diamond()` runs only in `Play` and keeps at most one
diamond on the field: it counts `diamond_wait` down, calls `spawn_diamond()` to
place one at a random point anywhere in the window — inside the square or out —
and grants the boost when the ball comes within `ball_radius() + kDiamondReach`.
Nothing else clears one: a diamond has no lifetime, so the next `diamond_wait`
only starts running once the ball has eaten the current one. Since the ball
never leaves the square, a diamond that lands outside it is collected by driving
the square onto it, which is the point of spawning them out there. Positions and
gaps come from a small LCG on `World::rng`, seeded from `SDL_GetTicks()` in
`make_world()` so a reset doesn't replay the same spawns.

Each pickup also bumps `World::eaten`, which `render()` lays out along the
bottom as a row of small diamonds, centered on the window so it opens outward
from the middle as it fills. The row is drawn as backdrop like the top dots, so
the square passes over it, and it is capped at what the window's width can hold
— the count itself keeps going. A reset clears it along with the rest of the
world.

A diamond pays out two different ways. Size is *derived*: each pickup multiplies
`World::grow` by `circle_growth_per_diamond` — the one pickup value that lives
in config.json (`circle.growth_per_diamond`, clamped to at least 1.0 so a
diamond can never shrink the ball) rather than in a `k` constant — and it stays
there for the rest of the run, so growth stacks and only a reset
(`make_world()`) takes it back. Every pass that
cares about the ball's size (bouncing, drawing, bursting) goes through
`ball_radius()` rather than reading `circle_diameter` directly, which is also
where the stack is capped — `kGrowCeil` of the square's shorter side, since a
ball wider than the square would leave the confine pass nowhere to put it.

Speed is *stateful* instead: `kBoostSpeed` is scaled into `circle_vx/vy` on
pickup and divided back out when `World::boost` lapses in `Play`, because the
bounce logic only ever flips the sign of those components. That asymmetry is why
a diamond grabbed mid-boost only refreshes the clock — scaling the velocity
twice would compound — while the size it adds stacks regardless.

**The eye.** `World::look` is an angle, not a vector, and `update_look()` walks
it toward its target at `kEyeTurnRate` every tick instead of snapping — the turn
*is* the animation. The target is the star during `StarLook` and the ball's own
heading everywhere else, so the eye leads the ball without any per-phase drawing
code. `update_look()` returns how far the turn still has to go, which is the
signal `StarLook` waits on. It also carries `World::gaze`, which eases to 0 during
`StarHold` and back to 1 everywhere else. `fill_eye()` multiplies three things
by it — the pupil's reach, the socket's rise up the ball, and the capsule's
stretch — so at rest on a star the eye slides to the middle of the body and
collapses into a plain circle, all from the one factor. `fill_eye()` draws a bare pupil on the pink — no white behind it —
a horizontal capsule the look direction moves but never rotates.
`kPupilSize`, `kEyeRise` and `kEyeTravel` are all fractions of `ball_radius()`,
so the eye scales with every diamond; rise and travel are picked together so
that at full reach the capsule grazes the rim going up and stops level with the
two-thirds line coming down, which is the band the eye roams. The bottom third
reads as body — except at rest on a star, where `gaze` takes the rise out along
with everything else.
Everything is a fraction of `ball_radius()`, so the eye grows with every
diamond, and the pupil's reach stops short of the socket's rim, which keeps it
inside the ball at any size. `fill_capsule()` walks a line of `fill_circle()`
discs rather than solving a rotated stadium — same primitive as everything else
round in here.

**The triangles.** `update_triangles()` runs wherever the ball is live — `Play`
and all three star phases — so hazards keep coming during a chase but freeze
with everything else during a shake or a burst. `spawn_triangle()` picks a
random angle, drops one off screen on that bearing and aims it at the middle of
the window, so each crosses the center on its own line and points where it goes
(`heading` feeds `fill_triangle()`). They retire once past the far side, and at
most `kTriangleMax` are out at a time — the timer skips a beat rather than
queueing when the screen is full. A touch (ball collider plus `kTriangleHit` of
the triangle's size) bursts the triangle through the same `fan_shards()` the
ball uses and sends the ball into `Phase::Shake`, the identical sequence a
moving wall triggers: rattle, drop a dot, resume — or the death sequence if that
was the last dot. Size, color, speed and rate are the `triangle` section of
config.json.

**The star.** Rarer than a diamond and not a pickup: it takes the ball off the
player entirely. Everything about it is config — `star.size`, `star.color`,
`star.gap_min`/`gap_max`, `star.edge_margin`, `star.pause`, `star.seek_speed`,
`star.hold` and `star.spin_speed` — so
the whole effect is tunable with R; only the point geometry
(`kStarInnerRatio`), the turn bounds and `kStarTimeout` stay in the code. `Play` counts `star_wait` down, and when `spawn_star()` places
one on screen — anywhere `star.edge_margin` clear of the window edge, and that
margin is trimmed at spawn to what the window can spare rather than starving the
band it picks from — the phase switches to `StarLook`: the ball stops dead,
holds `star.pause` still facing the way it was going, and only then does the eye
come around to the star. The phase ends once that turn lands. `kStarTurnMin` and
`kStarTurnMax` are both measured *from the end of the pause*, which matters:
during the hold the eye is aimed along its old heading, so a floor that did not
clear the pause would end the phase before the eye ever moved. Then the chase
proper, `StarSeek`. Neither
star phase calls `confine_circle_to_square()`, which is the whole trick: the
ball passes straight through the wall instead of bouncing off it. It does not
pass through for free, though — `StarSeek` compares `ball_inside_square()`
before and after each tick, and any change of side (the ball crossing out, or
the player driving an edge into it while it is out there) runs the same
`Phase::Shake` a moving wall does in `Play`. That is why the shake sends you
back to `StarSeek` rather than `Play` when a star is still out: the chase is
interrupted, not cancelled. `StarLook` and `StarHold` run the same check, since the square is
still the player's to drive while the ball sits there.

`home_ball()` snaps on the final step, so the centers land exactly on top of
each other rather than orbiting the target. The trip out runs at
`star.seek_speed` of `circle.speed` — an absolute speed, not a scale on what the
ball is carrying, so a diamond boost cannot make a chase faster. Position and velocity part ways in there: the step
uses the chase speed while the stored vector keeps the ball's carried magnitude,
which is what `ball_speed()` reads back next tick and what `launch_ball()`
releases it with. Arriving switches to `StarHold`: the ball parks
exactly where the star is and the star spins in place around it at
`star.spin_speed` for `star.hold` seconds — `fill_star()` takes that wound-up
angle, and `spawn_star()` zeroes it so every star arrives upright. The square is
still the player's to drive throughout, but the ball no longer has anything to
do with it. Then the star clears, `launch_ball()`
picks one of the four diagonals at the same speed — a random *angle* would let
the ball crawl along a wall, so it releases on the same heading the game opens
on, with two bits of the LCG choosing the quadrant — and `Play` resumes with
`kHitGrace` set, since the ball may still be crossing back into the square. `kStarTimeout`
is the escape hatch: at `circle.speed` of 0 the ball would never arrive.

`Play`, `StarSeek` and `StarReturn` share `move_square()` and `age_boost()`,
which is why the square still steers and a boost still expires mid-star.

Rendering is immediate-mode SDL — clear to background, the top dots, the square
rect, the diamond, the ball (or its shards), then the fade — which is what puts
the pickup over the square but under the ball. `fill_circle()` draws one
`SDL_RenderDrawLine` span per pixel row and is the only circle primitive: ball,
dots and shards all go through it, all in `circle_color`; `fill_diamond()` is
the same row-by-row fill with a linear taper. `fill_polygon()` covers what a closed-form span cannot, with an
even-odd scanline fill over sorted edge crossings — same one-line-per-row idea;
`fill_star()` (concave, ten points) and `fill_triangle()` (three, one corner
leading) both build their outline and hand it over. The diamond's green is the
only hardcoded color left. The star draws last of the field, over the ball, so only
the fade covers it: a full-window black rect at `fade_alpha()`, which is why the
renderer is put in `SDL_BLENDMODE_BLEND` at startup.

**R reloads config.json** at runtime (resizes the window, rebuilds the world,
which also restores the dots and clears any phase in progress), which is the
intended way to tune values without a rebuild.
