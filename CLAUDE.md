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
the whole game — the simulation, the renderer, and the CRT pass that puts the
rendered frame on the glass.

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
square through `move_square()`,
integrates the circle, then calls `confine_circle_to_square()`. That last pass
is what makes both interactions work with one piece of code: it clamps the
circle to the square's inner bounds and flips the velocity component *away* from
whichever wall was crossed, so a circle hitting a wall bounces and a wall driven
into the circle knocks it away. It also *reports* the touch, which is what the
damage rule keys off.

**Driving the square.** The keys set a direction to accelerate along, not a
position: `square.acceleration` builds `World::square_vx`/`vy` up to
`square.speed` — capped on the *vector*, so a diagonal doesn't outrun a straight
line — and `square.friction` scrubs it off once the keys are let go, never
overshooting into reverse. Friction well above acceleration is what makes it
read as stopping dead while still having weight. The window edge zeroes the
velocity on that axis rather than letting the box scrape along at speed, and
lights up white down the whole of that screen edge while the square is against
it — `kEdgeWidth` thick, at `kEdgeTouch` of slack, which only has to cover the
rounding into pixels since the clamp lands the box exactly on the boundary. It
is drawn with the square and carries `World::square_alpha`, so an edge parked
against dims with the frame and firms up again as it is driven.

A frame that isn't going anywhere fades to `square.idle_alpha` and firms up
again as it moves, eased through `World::square_alpha` so it doesn't flicker on
every tap of a key. Only the phases that actually drive the square count as
moving — during a fade the velocity is merely stale, not real — with one
exception: `Shake` and `Burst` hold it solid, so the wall that landed a hit
keeps its weight until the ball has finished reacting to it.

**The flash.** `World::flash` records whether the shake now running was a
wall's, and `ball_color()` swaps the body between `circle_color` and
`square_color` `kFlashRate` times a second for as long as it is set. It has no
length of its own: it is set with the phase change, cleared at the recovery, and
at a burst it simply stops mattering, since `ball_alive` is false and there is
no body left to color — so it runs exactly until the ball recovers or explodes.
`World::timer` doubles as its clock, being the shake's own and starting at zero,
which puts the square's color on the first frame of the hit.

Only a wall sets it — `square_moving` in `Phase::Play`, the edge crossing in the
three star phases, both of them the same terms the damage rule uses — so a
hazard's shake, which is the red one's to own, goes unflashed. The square's
full-strength color is used, never the one dimmed by `square_alpha`, though a
wall being driven is at full strength regardless. Only the body takes it: the
eye, the star ring and the loose shards keep their own colors.

**The hit sequence.** A touch only costs something when the player was pushing
the square that tick (`move_square()` reports the keys, not its velocity, so a
wall still coasting after the key is let go is free) and `World::grace` has
expired — an idle bounce is free, and the
`kHitGrace` window after a hit keeps a held key from eating every dot at once.
A hit runs `Play → Shake → Play`: the whole world freezes for `kShakeTime` while
`render()` offsets the ball by a decaying sine rattle, then `drop_next_dot()`
knocks the rightmost dot in the top row loose. `update_dots()` runs in *every*
phase, so that dot keeps arcing off the bottom of the screen while play resumes
around it — over the top of the field, since the dots are HUD. It carries
`World::draining` too: a forfeit hands over the whole row a dot at a time rather
than all at once, and putting that here is what lets the row go on emptying
itself through a burst and a fade, when nothing else is still running.

A world *opens* on `Phase::FadeIn` — that is the `World::phase` default, so
startup and an R reload both fade up out of black instead of snapping in, and
the ball holds still until the fade lands. The death sequence is unchanged by
it: `FadeOut`'s rebuild sets `Black` on the fresh world immediately after
`make_world()`, so the hold still happens before anything is revealed.

The opening also waits on the player. `World::started` turns on the first tick
`move_square()` reports a key in `Phase::Play`, and until it does, the square
does not shrink and `update_diamond()` returns early — the ball just bounces in
a full-size frame. Both timers *hold* rather than drain while it waits, the same
rule the hazard and star gates follow, so the first diamond comes a full gap
after that first push. Everything downstream is gated on `World::eaten`, which
cannot move before a diamond does, so one flag holds the whole field back. A
reset clears it, so every life opens the same way.

When `drop_next_dot()` takes the last one, the sequence continues
`Shake → Burst → FadeOut → Black → FadeIn → Play` instead: `burst_ball()` clears
`ball_alive` and calls `fan_shards()`, which throws a `std::array` of shards out
on an even fan at staggered speeds and sizes (index-derived, so no RNG) —
the ball and the triangles each own one such array — unconfined by the square;
`Burst` ends when the last shard is off screen — and, if a forfeit is handing
over the row, not before the last dot has been knocked loose: it can fall on
into the fade like any other, but the taking of it has to be seen. The reset happens at the end of
`FadeOut` (`w = make_world(cfg)`), behind full black, so `FadeIn` reveals a game
already back in its starting state. Phase lengths are the `k*Time` constants at
the top of the file.

**The shrinking square.** `shrink_square()` takes `square.shrink_rate` pixels
off `World::square_w`/`square_h` per second, through `resize_square()` — where
the shrink and the recovery both change the box's size, moving the corner by
half the difference so the walls close in evenly around its own center instead
of the box crawling one way. A hexagon's recovery goes through the same helper;
the squeeze is the one resize that does not, since it anchors on the ball. It is called from `Phase::Play` only, and only once
`World::started` is set and while `boost` is spent — which is the whole pause rule: a diamond's speed boost holds
the walls, and so does every star phase, since none of them run through `Play`.
The floor is `square.min_size` or the ball's current diameter, whichever is
larger, so the circle always fits however much diamonds have grown it; the
config clamps `min_size` between `circle.diameter` and the starting square. Note
that a closing wall touching the ball costs nothing — the damage rule keys off
`square_moving`, which is read from the keyboard, not from the shrink.

**The squeeze.** Holding space takes hold of the ball. `Phase::Squeeze` walks
the frame in until it is wrapped around the pink, the ball stops where it is,
and from there it goes wherever the square goes — `move_square()` drives the
frame as always and the ball is carried by the same delta. It is one number,
`World::squeeze`, 0 open and 1 shut, running up while the key is held and back
down when it is let go; `apply_squeeze()` is the only thing that reads it, and
it runs two lerps off it — the frame's size from `hold_w`/`hold_h` down to a
grip on the ball, and the ball's place *inside* the frame to dead center. Both
ends are recorded once, at the grab, so winding the one number back to zero
restores exactly the frame it interrupted, down to where the ball sat in it,
which is the only thing "extends to its normal dimensions" can mean when the
walls have been closing in all game. It is the one resize that does not go
through `resize_square()`, because it anchors on the ball rather than on the
box's own center: the walls come to the ball, which is what makes it read as a
grab and not as the ball being drawn to the middle. The window clamp moves the
ball with the frame, so driving a grip into a screen edge carries the ball
rather than squirting it out of the corner.

The ball does not have to be *in* the frame to be taken. `hold_fx`/`hold_fy` are
only where it sits in the frame's span, as happily outside `[0, 1]` as in, which
is what lets a grab be made during `StarSeek`: the frame leaves its ground and
closes on the ball out at the star, then hands that ground back on the way out,
since the same two numbers run it both ways. A ball already out of the square is
already in trouble, so that grab starts with the ball's patience spent —
`begin_squeeze()` takes the fuse to start at, `squeeze.warn` from a chase and
nothing from open play — and it shakes from the moment the walls reach it, with
only `squeeze.crush` to go. Letting go hands the ball back to whatever it was
taken from: a star still on the field means a chase, the same rule a shake
recovers by. Because the ball comes back exactly where the frame found it,
outside the walls if that is where it was, the chase counts no crossing for
having been interrupted. `StarLook` and `StarHold` are not grabbable — the first
has the ball sitting in the square anyway, and the second is the star's to
settle.

Once the walls are wrapped, nothing gives another pixel — not the ball, not the
frame. What runs from there is the clock: `squeeze_strain()` is 0 until the grip
has been on for `squeeze.warn`, then goes to 1 over `squeeze.crush`, and the
only thing that reads it is `render()`, which rattles the ball by it. The shake
is the whole of the warning, and it runs the other way up from a hit's rattle,
which starts hard and settles. The strain is scaled by `squeeze` itself, since
the walls are what the ball is shaking against, so letting go quiets it as they
open.

Held to the end, the grip does not cost a dot — it costs the run, and it goes
straight to `Phase::Burst` rather than through `Phase::Shake`: there is no
rattle left to play, the ball having been shaking for the whole of
`squeeze.crush` already. The frame keeps its grip through it, which is the one
place `release_squeeze()` is *not* called — the frame is what did this, and
letting go at the last moment is exactly what the player did not do, so the ball
goes off inside a frame still shut on it and only the rebuild behind the fade
opens it again. The row goes after the ball rather than with it: `World::draining`
knocks the dots loose one after another, `kDrainFirst` after the burst and
`kDrainGap` apart, so the lives read as being taken in turn.

A hazard reaching the ball mid-grip is the ordinary hit and goes out by its own
line, costing the one dot it always costs and springing the frame open so the
ball has room to react — a ball held still is in no position to dodge one, but
it is a hit like any other. The clock unwinds as well as winds, so letting go
part way buys back exactly the time it costs to take hold again and tapping the
key is not a free hold.

The rest of the field runs on around it. A diamond is still collected — by
driving the frame onto it, the ball having nothing to do with where it goes any
more — and it pays out as it always does, but `update_diamond()` reports the
pickup and the grip puts the clock straight to `squeeze.warn`, so the ball
starts to give on the spot. Hazards keep coming and keep hurting. Nothing
confines the ball, nothing shrinks the square, and no star appears, since only
`Play` spawns those. `squeeze.close`, `squeeze.warn` and `squeeze.crush` are
config; the grip's own clearance and its rattle are constants.

**The resting frame.** `resting_w()`/`resting_h()` report the size the square
counts as being: its own, or the one a squeeze will spring back to.
`resting_frame()` puts that box where the walls will land, around the ball where
it sat. Three passes ask rather than reading the walls directly —
`ball_radius()`'s growth ceiling, which *cannot* read the current frame, since a
squeeze closing the walls in would otherwise take the ball down with them and
the grip that follows the ball would chase it to nothing; the hexagon's
recovery; and the two spawns that have to stay off the square. Outside a squeeze every one of them is the square itself, so the
whole idea costs nothing the rest of the time.

**The pickup.** `update_diamond()` runs wherever the ball is live and keeps at most one
diamond on the field: it counts `diamond_wait` down, calls `spawn_diamond()` to
place one at a random point anywhere in the window — inside the square or out,
but `diamond.edge_margin_x`/`_y` clear of the sides and of the HUD rows top and
bottom, each margin trimmed at spawn to what a small window can spare —
and grants the boost when the ball comes within `ball_radius() + kDiamondReach`.
The first `kOpeningDiamonds` are the exception: their band is drawn in toward
the middle of the window by `kOpeningSpread`, so the opening pickups are near
the centered square rather than a drive away. The band is pulled in rather than
replaced, which keeps it a subset of the ordinary one — every margin above still
holds at any window size.

Wherever the band is, the spot has to be clear of the ball: `kDiamondTries`
placements are drawn looking for one `kDiamondClear` times the collection reach
away, since a diamond landing inside that reach is eaten on the frame it appears
and is a pickup the player never got to take. Measuring the clearance in
collection reaches rather than pixels keeps it honest as the ball grows. The
last try stands whatever it measured, which is where this parts company with
`spawn_still_triangle()`: a hazard that cannot find a spot sits the round out,
but `World::eaten` is what unlocks the hazards and the star, so a skipped
diamond would stall the run — a crowded window gets a near miss instead.
Nothing else clears one: a diamond has no lifetime, so the next `diamond_wait`
only starts running once the ball has eaten the current one. Since the ball
never leaves the square, a diamond that lands outside it is collected by driving
the square onto it, which is the point of spawning them out there. Positions and
gaps come from a small LCG on `World::rng`, seeded from `SDL_GetTicks()` in
`make_world()` so a reset doesn't replay the same spawns.

Each pickup also bumps `World::eaten`, which `render()` lays out along the
bottom as a row of small diamonds, centered on the window so it opens outward
from the middle as it fills. The row is HUD, drawn over everything on the field, and it is capped at what the window's width can hold
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
where the stack is capped — `kGrowCeil` of the *resting* square's shorter side,
since a ball wider than the square would leave the confine pass nowhere to put
it.

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
a horizontal capsule the look direction moves but never rotates, with a speck of
white caught in its right-hand end. `kGlintSize` and `kGlintRise` are fractions
of the capsule's cap radius that sum to less than one, so the highlight never
breaks the edge of the ink, and because the capsule holds its angle the glint
stays in the corner instead of swimming around as the eye turns.
`kPupilSize`, `kEyeRise` and `kEyeTravel` are all fractions of `ball_radius()`,
so the eye scales with every diamond; rise and travel are picked together so
that at full reach the capsule comes short of the rim going up and stops about
level with the two-thirds line coming down, which is the band the eye roams. The bottom third
reads as body — except at rest on a star, where `gaze` takes the rise out along
with everything else.
Everything is a fraction of `ball_radius()`, so the eye grows with every
diamond, and the pupil's reach stops short of the socket's rim, which keeps it
inside the ball at any size. `fill_capsule()` walks a line of `fill_circle()`
discs rather than solving a rotated stadium — same primitive as everything else
round in here.

**The triangles.** `update_triangles()` runs wherever the ball is live — `Play`
and all three star phases — so hazards keep coming during a chase but freeze
with everything else during a shake or a burst. Two kinds share one `Triangle`
struct and one `kHazardMax` pool, told apart by `moving`, and each has its own
spawn timer:

- **Moving** (`spawn_triangle()`): a pair drawn like a fast-forward button. A
  random bearing puts it off screen aimed at the middle of the window, so each
  crosses the center on its own line, pointing where it goes. It waits off
  screen for `moving_hazard.warn` first, with `fill_path_strip()` drawing the
  translucent lane it is about to run down — the same `warn` field the still
  ones use to stay harmless — then sets off. Retires past the far side. Tuned by
  the `moving_hazard` section.
- **Still** (`spawn_still_triangle()`): one upright triangle planted on the
  field, which arrives *unarmed* — drawn hollow by `draw_triangle()` and skipped
  by the collision pass entirely — for `still_hazard.arm` seconds, then fills
  in, buzzes on the spot (`kStillBuzz`, a render-only offset) and is dangerous
  for `still_hazard.life` before it goes. Solid-and-buzzing versus hollow-and-
  still is the whole tell, so the two states must never look alike. It spawns
  outside the square as well as clear of the ball: inside the frame the ball
  would have nowhere to dodge to. The square it keeps out of is the *resting*
  one, so a squeeze does not open back out onto a triangle planted in the room
  the grip gave up. It comes in one of three sizes, drawn evenly — `triangle.size`, half again, or double — carried on the
  hazard's own `scale`, which `hazard_size()` turns into pixels for drawing, for
  the spawn clearance and for the hit radius, so a bigger one really is harder
  to dodge. It retries a few placements to avoid landing on the ball, which
  would be an unreactable hit, and clear of a star that is already out, the
  same rule from the other side. Tuned by the `still_hazard` section.

Both kinds also get *faster*, and go on getting faster. `hazard_gap()` is what
picks either wait, and it scales the draw by `World::pressure` — a clock that
starts at the `kHazardRampAt` diamond and from then on never stops, closing both
gaps steadily to `kHazardRampFloor` of what the config asks over
`kHazardRampTime`. Only the *arming* is a tally; what tightens the gaps is time,
which is the point: a player who stops eating diamonds used to stop the pressure
along with them, and waiting the run out is exactly what this is for. The clock
is advanced inside `update_triangles()` rather than in `step()`, so it runs in
precisely the phases the hazards do — a shake, a burst or a fade is not time the
run gets to hold against you. Scaling the draw rather than the config is what
keeps the file honest — it still means what it says, and an R reload still
reports its own numbers — and the clock is world state like everything else, so
a reset takes the pressure off and the run earns it back.

Both gates read `World::eaten`, the same tally the bottom row draws, and both
timers *hold* rather than draining while locked — so the first hazard of a kind
comes a full interval after the qualifying diamond, not the instant it is eaten.
A reset zeroes the tally, so each life earns its hazards again.

`World::grace` does *not* hold the hazards off, only the walls. A hazard bursts
on contact, which means it cannot land the same hit twice the way a held key
against a wall can — that repeat is the whole reason the window exists — and it
is spent either way, so forgiving one would destroy it for nothing and take
nothing back for it. The grace check therefore sits on the wall term alone in
all four phases that damage: `struck || (wall && grace)`, never
`(struck || wall) && grace`. Coming out of a star is where the older grouping
showed, the ball released into open field with a fresh window running.

`hazard_lobes()` is what keeps the two honest: it returns the one or two points a
hazard's triangles actually occupy, and *both* drawing and collision go through
it, so the shape you see is the shape that hits you. A touch (ball collider plus
`kTriangleHit` of the size, tested per lobe) bursts the hazard through the same
`fan_shards()` the ball uses and sends the ball into `Phase::Shake` — the
identical sequence a moving wall triggers: rattle, drop a dot, resume, or the
death sequence if that was the last dot. The two kinds have separate config sections —
`moving_hazard` and `still_hazard` — each with its own size, color, rate and
unlock, so they can be tuned against each other; `hazard_size()` and
`hazard_color()` pick the right one off the hazard's `moving` flag. The loose
shards carry `World::shard_tint`, set at the burst, since a burst replaces the
whole array anyway.

**The hexagon.** The square's own pickup, and the only thing that undoes the
shrink. `update_hexagon()` keeps at most one out, gated on `hexagon.unlock`
diamonds and spawned by `spawn_hexagon()`, which retries placements until
`hex_spot_is_clear()` accepts one: clear of the resting square (so it has to be
driven to rather than collected where it stands, and a squeeze cannot open out
onto one), of the ball, of any diamond or star
on the field, of every hazard — a waiting crossing judged by the *lane* it is
about to run, since it is still parked off screen — and off the HUD rows. Collection is a rect-to-circle
test — nearest point on the square to the hexagon's center, against
`hexagon.size` — because it is the *frame* that collects this one, not the ball.
Taking it doesn't resize the square, it sets a *target*: `kHexRecovery` of the
way from the current size back to `square.width`/`height`. `grow_square()` then
walks the walls out to it at `hexagon.grow_rate`, and while it is doing so the
shrink holds off, so the two never fight over the same pixels. The target can
approach the starting size but never pass it. The gap it closes is measured off
the *resting* frame, and `grow_square()` does not run during a squeeze at all,
so one collected while the walls are shut on the ball counts the ground the
shrink took rather than the grip, and is paid out once the frame is open again.

**The star.** Rarer than a diamond and not a pickup: it takes the ball off the
player entirely. Everything about it is config — `star.size`, `star.color`,
`star.gap_min`/`gap_max`, `star.edge_margin`, `star.pause`, `star.seek_speed`,
`star.hold`, `star.spin_speed` and `star.unlock_diamonds` — so
the whole effect is tunable with R; only the point geometry
(`kStarInnerRatio`), the turn bounds and the trip allowance stay in the code. `Play` counts `star_wait` down once `World::eaten` has reached
`star.unlock_diamonds` — held, not drained, until then, the same as the hazard
gates — and when `spawn_star()` places
one on screen — anywhere `star.edge_margin` clear of the window edge, that
margin trimmed at spawn to what the window can spare rather than starving the
band it picks from, and clear of any planted hazard, since the ball is sent
*straight* to a star and one sitting on a triangle would be a trap with no way
around it — the phase switches to `StarLook`: the ball stops dead,
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
exactly where the star is, wearing a ring in `star.color` — `fill_ring()` fills it a
row at a time between two radii, the same span-per-row idea as `fill_circle()`,
since a polyline around the rim facets at anything but a small radius; its
radius is `kStarRingGap` per ball radius and its thickness `kStarRingWidth` of
the same, floored at `kStarRingMin`, so the whole ring grows with every
diamond. It is drawn
with the ball rather than with the star, which is what puts it under the star
and gives it the same rattle. Then the star spins in place around it at
`star.spin_speed` for `star.hold` seconds — `fill_star()` takes that wound-up
angle, and `spawn_star()` zeroes it so every star arrives upright. The square is
still the player's to drive throughout, but the ball no longer has anything to
do with it. Where the square is when the hold ends decides whether there is a release at
all. `ball_inside_square()` — the same whole-collider test the crossing rule
uses, so a ball straddling a wall counts as out — is checked as the star lets
go, and a ball still outside costs the run: every dot left drops at once and
`Phase::Shake` follows. Dropping them *before* the shake is what turns the
ordinary sequence into a death, since the shake bursts the ball whenever it
finds nothing left to take, so the rattle plays, the whole row arcs off the
bottom and the burst comes after it. Bringing the square out to meet the ball is
the price of the chase.

Otherwise the star clears, `launch_ball()`
picks one of the four diagonals at the same speed — a random *angle* would let
the ball crawl along a wall, so it releases on the same heading the game opens
on, with two bits of the LCG choosing the quadrant — and `Play` resumes with
`kHitGrace` set, since the ball may still be crossing back into the square. The trip is allowed the
window's own diagonal — the longest a straight line to a star can be — at the
chase speed, times `kStarTripSlack`, with `kStarTripMin` as a floor. Measuring
it rather than fixing it is the point: window size and both speeds are config,
so any flat number is generous in one window and short of the far corner in
another, and a chase cut off part way puts the ball into the hold holding a star
it never reached. The floor is the real escape hatch, for `circle.speed` at 0
where there is no arriving at all — and it snaps the ball onto the star on its
way out, since a hold anywhere else reads as catching something it never got
to.

`Play`, `StarSeek` and `StarReturn` share `move_square()` and `age_boost()`,
which is why the square still steers and a boost still expires mid-star.

**The backdrop.** `build_backdrop()` fills the window with overlapping blue
circles, and `render()` lays them down before anything else. They are on a grid,
which is what lets the coverage be guaranteed instead of hoped for: a tile's
diameter is never under `kBackSizeMin` of its cell, which is `sqrt(2)` — a
circle has to reach its cell's corners, where a square only had to reach its
sides — plus twice `kBackJitter` *and* twice `kBackOrbit` for everywhere
wandering and turning can take it. From anywhere it can get to it still covers
the cell it came from, so no black shows between them. Each is one stretched
copy of `Screen::disc`, a single white circle tinted per tile with
`SDL_SetTextureColorMod`: circles this size drawn a row at a time would be tens
of thousands of line calls a frame, where this is one textured quad each, and
the upscale is also what softens the rim — a row of spans could not. The disc is
built with the other screen resources, and a backdrop falls back to squares if
it could not be made.

The tiles are shuffled once they are built. Drawn in the order the grid makes
them, each would overlap the one up and left of it and the grid would come back
as a shingle leaning one way; shuffling touches only which of two overlapping
circles ends up on top, never where any of them are, so the coverage argument
above is untouched. Each tile walks a small circle of `kBackOrbit`, all at `kBackSpin` and all
the same way round, differing only in where on it they start — one draw per tile
at build time — so the field turns without moving as a sheet. `World::drift` is
its clock and is advanced in `step()` before the phase switch, so the backdrop
keeps turning through shakes, bursts and fades, which are exactly the moments
the rest of the world is holding still. Size and jitter then vary enough that
the grid does not read as one. Hues are a mix of `kBackDeep` and one of two lifts:
`kBackLift` for blue, or `kBackViolet` for the `kBackVioletShare` of tiles that
come out purple instead — enough to be noticed, not enough to become the color
of the backdrop. Both ends of both mixes are dark, which is what lets the mix
run its whole range without lifting the field off the bottom: the pink, the
aquamarine and the red all have to stay the lit things on it. A
window too big for the pool coarsens the grid rather than leaving holes in it.
It is built in `make_world()` off `World::rng`, so it holds still while the game
runs over it and comes back different after a reset or an R reload — and behind
the fade, so a death reveals a new one already in place. `background.color` now
only shows through the gaps, of which there are none.

Rendering is immediate-mode SDL — clear to background, the backdrop, the square
as a hollow frame (`square.outline` nested `SDL_RenderDrawRect`s, so a diamond under it
shows through), the diamond, the ball (or its shards), then the fade — which is what puts
the pickup over the square but under the ball. None of it lands on the window:
`render()` points the renderer at `Screen::frame`, an off-screen texture of
exactly `window_w`/`window_h`, so every pass above works in config-space pixels
and the tube below is the only code that knows what the window is showing. `fill_circle()` draws one
`SDL_RenderDrawLine` span per pixel row and is the only circle primitive: ball,
dots and shards all go through it, all in `circle_color`; `fill_diamond()` is
the same row-by-row fill with a linear taper. `fill_polygon()` covers what a closed-form span cannot, with an
even-odd scanline fill over sorted edge crossings — same one-line-per-row idea;
`fill_star()` (concave, ten points) and `fill_triangle()` (three, one corner
leading) both build their outline and hand it over. The diamond's green is the
only hardcoded color left. The star draws last of the play field, over the ball. Last of all come the two
HUD rows — the hits left along the top, the diamonds eaten along the bottom —
which nothing on the field can cover; only the fade goes over them: a full-window black rect at `fade_alpha()`, which is why the
renderer is put in `SDL_BLENDMODE_BLEND` at startup.

config.json is parsed with comments allowed (`json::parse(..., ignore_comments)`),
so a value can carry a note on the line beside it — the file is meant to be
annotated and retuned by hand, and strict JSON would throw the whole file out
over one `//`.

**R reloads config.json** at runtime (resizes the window, rebuilds the world,
which also restores the dots and clears any phase in progress), which is the
intended way to tune values without a rebuild.

`main()` sets `SDL_HINT_WINDOWS_DPI_AWARENESS` before `SDL_Init`, which is where
the video driver reads it. SDL declares no awareness on its own, and an unaware
process on a scaled display is given a window measured in logical pixels that
Windows then stretches to physical ones — a fractional scale under the game's
feet, which smears every thin thing it draws. Declaring awareness makes the
window's pixels the screen's pixels, so the frame lands 1:1. Startup logs a line
if the renderer's output size does not match the config after all, since a
scaled picture is the first suspect when it looks soft.

**F toggles fullscreen.** Every position in the game is derived from
`cfg.window_w`/`window_h`, so fullscreen *scales* the picture instead of
widening the field: `picture_rect()` fits the frame to the window with its
shape kept and black either side, which is the whole of what the mode changes.
A reload rebuilds the frame either way — the `crt` section is baked into it —
and only sets the window size when windowed, since a fullscreen window has none
to set.

**The tube.** SDL's 2D renderer has no shader stage, so the CRT pass is built
out of what it does have: `present_screen()` puts the finished frame up through
`draw_tube()`, a 32x24 mesh of `SDL_RenderGeometry` triangles with the curve in
its vertex positions and the vignette in its vertex colors, both driven off the
same `r^2`. Each node is drawn toward the middle by `1/(1 + curvature*r^2)` —
the picture packs together toward the rim the way it does on curved glass, and
because the corners sit on the longer radius they come in hardest, which bows
the edges outward into the shape of a tube face and keeps the whole picture
inside the window with no overscan to trim. The mesh is then opened back out by
`1 + curvature`, exactly what the pull takes off an edge midpoint, so the edges
meet the window and only the corners stay black.

Everything is added onto a black window, which is what makes the color fringing
one function: `aberration` lays the same frame down three times at three sizes,
a channel at a time, and passes that line up sum back to exactly the image — so
the split is nothing at the center and widest at the rim. Then the bloom, off
`Screen::glow`: `kGlowFloor` is subtracted from the frame first, then two
halvings through `copy_into()`, each a bilinear filter averaging a 2x2 block,
which is a box blur for the price of two blits (straight to a quarter would skip
three pixels in four instead of averaging them), added back over the same mesh
at `glow`. The floor is what makes it bloom off the lit things rather than off
everything: a subtract blend over the whole target, clamping at zero on its own,
which is what stands in for the comparison a shader would do. Without it a dark
field blooms itself into a gray one — the backdrop's colors are deliberately
kept under `kGlowFloor` so they contribute nothing. `Screen::take_floor` holds
the composed blend, or `SDL_BLENDMODE_NONE` and a logged line if the backend has
no subtract, in which case the glow is the old indiscriminate one. Last is the scanline mask — one pixel wide,
one row per config-space row, a cosine band rather than a hard row so it does
not beat against the pixel grid once the curve stretches it, which is also why
`line_gap` is floored at 2 rather than 1: sampled once a row, a period of one
puts every sample on the same point of the wave and the lines come out flat — laid over the lit
picture and curved along with it. It is stretched with the picture rather than
built per output pixel, so `line_gap` is tuned against the art, not the monitor.

`draw_tube()` shades only the vertex color and never the alpha, which is why the
vignette dims the picture but leaves the mask darkening the rim by as much as
the middle. `crt.enabled` false skips all of it for a plain `SDL_RenderCopyF`
into the same rect, and a renderer that cannot hold a render target leaves
`Screen::frame` null, which every pass reads as "the game already drew straight
to the window" — a flat game, but a game. Since the whole section is config,
R retunes the tube live.
