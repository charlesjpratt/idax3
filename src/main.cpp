#include <SDL.h>

#include "Config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double kFixedStep = 1.0 / 120.0; // physics tick
constexpr double kMaxFrame  = 0.25;        // don't spiral after a stall

// The window edge lights up where the square has run out of room against it.
// The clamp in move_square() lands the box exactly on the boundary, so the gap
// counted as a touch only has to survive the rounding into pixels.
constexpr float     kEdgeWidth = 4.0f; // thickness of the lit bar
constexpr float     kEdgeTouch = 0.5f; // gap still counted as against the edge
constexpr SDL_Color kEdgeColor{0xFF, 0xFF, 0xFF, 0xFF};

// Three small dots centered along the top edge: the hits the ball has left.
constexpr int   kDotCount   = 3;
constexpr float kDotRadius  = 5.25f;
constexpr float kDotSpacing = 26.0f;   // center to center
constexpr float kDotTop     = 22.0f;   // center's distance from the top edge
constexpr float kDotPop     = -70.0f;  // upward flick before a dot falls away
constexpr float kDotGravity = 1100.0f;

// A wall catching the ball while the square is moving: everything freezes and
// the ball rattles, then a dot drops away and play resumes.
constexpr float kShakeTime = 1.4f;
constexpr float kShakeAmp  = 6.0f;
constexpr float kShakeRate = 47.0f;
constexpr float kHitGrace  = 0.75f; // no second hit while the ball gets clear
// A driven wall flashes the ball between its own color and the square's, for
// as long as the shake it started runs. It ends where the shake does, at a
// recovery or at the burst, so it needs no length of its own.
constexpr float kFlashRate = 24.0f; // swaps per second

// Last dot gone: the ball bursts outward, then the screen cycles black and back.
constexpr int   kShardCount   = 14;
constexpr float kShardSpeed   = 320.0f;
constexpr float kShardTimeout = 3.0f; // in case a shard somehow lingers
constexpr float kFadeOutTime  = 0.9f;
constexpr float kBlackTime    = 2.0f;
constexpr float kFadeInTime   = 1.1f;

// The pickup: one green diamond at a time, anywhere on the field, sitting there
// until the ball reaches it. One outside the square is a reason to drive the
// square over there.
constexpr SDL_Color kDiamondColor{0x3C, 0xC8, 0x64, 0xFF};
constexpr float kDiamondHalfW  = 8.0f;
constexpr float kDiamondHalfH  = 12.0f;
// The opening few land near the middle of the window rather than anywhere in it.
constexpr int   kOpeningDiamonds = 3;    // how many spawn close in
constexpr float kOpeningSpread   = 0.3f; // of the usual band, measured from center
constexpr float kDiamondReach  = 9.0f; // collision radius, between the two halves
// Kept off the ball, as a multiple of the reach it is collected at.
constexpr float kDiamondClear  = 6.0f;
constexpr int   kDiamondTries  = 12;   // placements tried for one that far off
constexpr float kDiamondGapMin = 4.0f; // wait between one being eaten and the next
constexpr float kDiamondGapMax = 9.0f;

// The tally along the bottom: one small diamond per one eaten, the row growing
// out from the middle of the screen.
constexpr float kScoreScale   = 0.60f; // of a field diamond
constexpr float kScoreSpacing = 18.0f; // center to center
constexpr float kScoreBottom  = 22.0f; // center's distance from the bottom edge


// What eating one is worth: the speed wears off, the size stays. How much size
// is `circle.growth_per_diamond` in config.json, so it is tunable with R.
constexpr float kBoostTime  = 4.0f;  // seconds of extra speed
constexpr float kBoostSpeed = 1.5f;
constexpr float kGrowCeil   = 0.48f; // of the square's shorter side, per radius

// The star: rarer than a diamond, and not a pickup at all. It hauls the ball
// out of the square to itself, holds it over the square's center for a moment,
// then lets it go again on a new heading. Its size, color, rate, pause, chase
// speed and hold all live in the `star` section of config.json; what is left
// here is the shape of the points and two safety valves.
// While a star has the ball, it wears one in the star's own color.
constexpr float kStarRingGap   = 0.5f;  // clear of the ball's rim, per ball radius
constexpr float kStarRingWidth = 0.16f; // thickness, per ball radius
constexpr float kStarRingMin   = 2.0f;  // never thinner than this, in pixels
constexpr float kStarInnerRatio = 0.44f; // waist of the points, per outer radius
constexpr float kStarTimeout    = 6.0f;  // a crawling ball would never arrive

// The eye: a bare pupil on the pink, no white behind it, just a speck of one
// caught in its right-hand end. It points where the ball is headed, turning at
// a fixed rate rather than snapping, which is what makes it read as looking.
constexpr SDL_Color kEyePupil{0x00, 0x00, 0x00, 0xFF};
constexpr SDL_Color kEyeGlint{0xFF, 0xFF, 0xFF, 0xFF};
// Sizes and travel are all fractions of the ball's radius, so the eye scales
// with it. Rise and travel together keep the pupil in the ball's top two
// thirds: at full reach the capsule comes short of the rim going up and stops
// about level with the two-thirds line coming down.
constexpr float kPupilSize   = 0.21f; // pupil radius, per ball radius
constexpr float kEyeRise     = 0.25f; // eye's rest point above center
constexpr float kEyeTravel   = 0.38f; // how far the pupil roams from that point
constexpr float kPupilStretch = 0.42f; // capsule half-length, per pupil radius
constexpr float kPupilThin    = 0.72f; // capsule cap radius, per pupil radius
// Both per cap radius, and they add up to less than one, which is what keeps
// the highlight from breaking the edge of the ink it sits in.
constexpr float kGlintSize    = 0.34f; // highlight radius
constexpr float kGlintRise    = 0.30f; // how far off the middle of the cap it sits
constexpr float kEyeTurnRate = 9.0f;  // radians per second
constexpr float kEyeAimed    = 0.02f; // close enough to call the turn finished
constexpr float kGazeRate    = 4.0f;  // how fast the pupil slides out and back
constexpr float kSquareFadeRate = 6.0f; // how fast the frame firms up and dims

// Bounds on the eye's turn itself, measured from the end of `star.pause`: the
// floor keeps the hold from counting as "already aimed", the ceiling is there in
// case the turn somehow never lands.
constexpr float kStarTurnMin = 0.15f;
constexpr float kStarTurnMax = 1.9f;

// A hexagon wins back this much of the ground the square has lost: half the gap
// between where it has shrunk to and where it started.
constexpr float kHexRecovery = 0.5f;
constexpr float kSpawnPad = 10.0f; // breathing room between anything spawned

// Red hazards. Sizes, colors, speeds, rates and unlocks are the two hazard
// sections of config.json; what stays here is how hard they are to hit and how
// they come apart.
constexpr int   kHazardMax     = 8;     // moving and still ones together
constexpr float kTriangleHit   = 0.60f; // collision radius, per triangle size
constexpr float kPairOffset    = 0.50f; // half the gap in a pair, per size: the
                                        // two triangles overlap at this range
// A still triangle comes in one of three sizes, evenly drawn: the configured
// one, half again, or double.
constexpr int   kStillSizeCount = 3;
constexpr float kStillSizeStep  = 0.5f;
// Once armed it buzzes on the spot, which is the tell that it can hurt you.
constexpr float kStillBuzz     = 0.10f; // amplitude, per hazard size
constexpr float kStillBuzzRate = 38.0f; // radians per second
// The lane a crossing will take, shown before it sets off.
constexpr float kStripHalfWidth = 0.95f; // per hazard size
constexpr Uint8 kStripAlpha     = 60;
constexpr int   kTriShardCount = 10;
constexpr float kTriShardSpeed = 240.0f;

constexpr float kPi    = 3.1415927f;
constexpr float kTwoPi = 6.2831853f;

enum class Phase { Play, Shake, Burst, FadeOut, Black, FadeIn, StarLook, StarSeek, StarHold };

struct Dot {
    float x = 0.0f;
    float y = kDotTop;
    float vy = 0.0f;
    bool falling = false;
    bool gone = false;
};

struct Shard {
    float x = 0.0f, y = 0.0f;
    float vx = 0.0f, vy = 0.0f;
    float radius = 0.0f;
    bool alive = false;
};

struct Diamond {
    float x = 0.0f, y = 0.0f;
    bool active = false;
};

// One hazard, either kind. A moving one is a pair of triangles either side of
// `x, y` along its heading; a still one is the single triangle at that point,
// counting `life` down.
struct Triangle {
    float x = 0.0f, y = 0.0f;
    float vx = 0.0f, vy = 0.0f;
    float heading = 0.0f; // radians: the way it points, and for a pair, travels
    float scale = 1.0f;   // multiplier on `triangle.size`
    float warn = 0.0f;    // seconds left as a harmless outline, still ones only
    float life = 0.0f;    // seconds left once armed, still ones only
    bool moving = false;
    bool active = false;
};

struct Hexagon {
    float x = 0.0f, y = 0.0f;
    float spin = 0.0f; // radians, turning over while it waits to be collected
    bool active = false;
};

struct Star {
    float x = 0.0f, y = 0.0f;
    float spin = 0.0f; // radians, wound up during the hold
    bool active = false;
};

struct World {
    // Square is stored by its top-left corner and its own size — it starts at
    // the configured size and closes in from there — circle by its center.
    float square_x = 0.0f, square_y = 0.0f;
    float square_w = 0.0f, square_h = 0.0f;
    float square_vx = 0.0f, square_vy = 0.0f;
    float square_alpha = 1.0f; // frame goes see-through when it isn't driven
    float grow_to_w = 0.0f, grow_to_h = 0.0f; // size a hexagon promised, 0 when none
    float circle_x = 0.0f, circle_y = 0.0f;
    float circle_vx = 0.0f, circle_vy = 0.0f;

    // A world opens by fading up out of black, the same way one rebuilt behind
    // a death does — the reset in FadeOut moves it straight on to Black, so
    // that sequence still holds before it reveals anything.
    Phase phase = Phase::FadeIn;
    float timer = 0.0f; // time spent in the current phase
    float grace = 0.0f; // hit immunity left
    bool  flash = false; // is this shake a wall's, and so flashing
    bool  ball_alive = true;
    bool  started = false; // has the player taken hold of the square yet
    std::array<Dot, kDotCount> dots{};
    std::array<Shard, kShardCount> shards{};

    Diamond diamond{};
    float diamond_wait = 0.0f; // until the next one appears
    Hexagon hex{};
    float hex_wait = 0.0f;     // until the next one appears
    Star star{};
    float star_wait = 0.0f;    // until the next one appears
    std::array<Triangle, kHazardMax> triangles{};
    std::array<Shard, kTriShardCount> tri_shards{};
    SDL_Color shard_tint{}; // whichever hazard the loose shards came from
    float triangle_wait = 0.0f;
    float still_wait = 0.0f;
    float look = 0.0f;         // where the eye points, in radians
    float gaze = 1.0f;         // how far out the pupil sits, 0 = dead center
    int   eaten = 0;           // diamonds collected, the bottom row's tally
    float boost = 0.0f;        // seconds of extra speed left
    float grow = 1.0f;         // size multiplier, kept until the next reset
    Uint32 rng = 1u;
};

// Small LCG: the pickup wants scattered spawns, and this beats pulling in
// <random> for two numbers a second.
Uint32 next_rand(World& w) {
    w.rng = w.rng * 1664525u + 1013904223u;
    return w.rng;
}

float rand_range(World& w, float low, float high) {
    const float unit = static_cast<float>(next_rand(w) >> 8) / 16777216.0f;
    return low + (high - low) * unit;
}

// Diamonds swell the ball by `circle_growth_per_diamond` for the rest of the
// run, so every pass that cares about its size — bouncing, drawing, bursting —
// has to ask for the radius
// rather than assume it. Growth stacks and never lapses, so it is capped at a
// ball the square can still hold: past that the confine pass would have nowhere
// left to put it.
float ball_radius(const World& w, const Config& cfg) {
    const float ceiling = kGrowCeil * std::min(w.square_w, w.square_h);
    return std::min(cfg.circle_diameter * 0.5f * w.grow, ceiling);
}

// What the game actually collides on: walls, pickups and the fit test all use
// this, while everything drawn uses ball_radius(). `circle.collider_scale` is
// what sets the two apart — under 1.0 the pink sinks into a wall on a bounce.
float ball_collider(const World& w, const Config& cfg) {
    return ball_radius(w, cfg) * cfg.circle_collider_scale;
}

World make_world(const Config& cfg) {
    World w;
    w.square_w = cfg.square_w;
    w.square_h = cfg.square_h;
    w.square_x = (static_cast<float>(cfg.window_w) - w.square_w) * 0.5f;
    w.square_y = (static_cast<float>(cfg.window_h) - w.square_h) * 0.5f;

    // Place the circle across the square's interior — the span its center can
    // occupy without clipping a wall. Config clamps the fractions to [0, 1] and
    // guarantees the square is at least as wide as the circle, so this span is
    // never negative.
    const float radius = cfg.circle_diameter * 0.5f;
    w.circle_x = w.square_x + radius + (w.square_w - cfg.circle_diameter) * cfg.circle_start_x;
    w.circle_y = w.square_y + radius + (w.square_h - cfg.circle_diameter) * cfg.circle_start_y;

    // Launch on a diagonal so both axes bounce right away.
    const float diagonal = 0.70710678f; // 1/sqrt(2)
    w.circle_vx = cfg.circle_speed * diagonal;
    w.circle_vy = cfg.circle_speed * diagonal;
    w.look = std::atan2(w.circle_vy, w.circle_vx); // open the eye already aiming

    // The dot row is centered on the window, so it survives a resize on reload.
    const float center_x = static_cast<float>(cfg.window_w) * 0.5f;
    const float first    = center_x - kDotSpacing * (kDotCount - 1) * 0.5f;
    for (int i = 0; i < kDotCount; ++i) {
        w.dots[i].x = first + kDotSpacing * static_cast<float>(i);
    }

    w.rng = SDL_GetTicks() * 2654435761u + 1u; // don't replay the same spawns
    w.diamond_wait = rand_range(w, kDiamondGapMin, kDiamondGapMax);
    w.hex_wait     = rand_range(w, cfg.hexagon_gap_min, cfg.hexagon_gap_max);
    w.star_wait    = rand_range(w, cfg.star_gap_min, cfg.star_gap_max);
    w.triangle_wait = rand_range(w, cfg.moving_hazard_gap_min, cfg.moving_hazard_gap_max);
    w.still_wait    = rand_range(w, cfg.still_hazard_gap_min, cfg.still_hazard_gap_max);
    w.shard_tint    = cfg.moving_hazard_color;
    return w;
}

// Keeps the circle inside the square, reflecting whichever wall it crossed, and
// reports whether it touched one. The same pass handles the square being driven
// into the circle, so shoving the box at the ball knocks the ball away instead
// of swallowing it.
bool confine_circle_to_square(World& w, const Config& cfg) {
    const float radius = ball_collider(w, cfg);
    const float min_x  = w.square_x + radius;
    const float max_x  = w.square_x + w.square_w - radius;
    const float min_y  = w.square_y + radius;
    const float max_y  = w.square_y + w.square_h - radius;

    bool hit = false;

    if (max_x <= min_x) {
        w.circle_x  = (min_x + max_x) * 0.5f; // square narrower than the circle
        w.circle_vx = -w.circle_vx;
        hit = true;
    } else if (w.circle_x < min_x) {
        w.circle_x  = min_x;
        w.circle_vx = std::fabs(w.circle_vx);
        hit = true;
    } else if (w.circle_x > max_x) {
        w.circle_x  = max_x;
        w.circle_vx = -std::fabs(w.circle_vx);
        hit = true;
    }

    if (max_y <= min_y) {
        w.circle_y  = (min_y + max_y) * 0.5f;
        w.circle_vy = -w.circle_vy;
        hit = true;
    } else if (w.circle_y < min_y) {
        w.circle_y  = min_y;
        w.circle_vy = std::fabs(w.circle_vy);
        hit = true;
    } else if (w.circle_y > max_y) {
        w.circle_y  = max_y;
        w.circle_vy = -std::fabs(w.circle_vy);
        hit = true;
    }

    return hit;
}

// Is the ball's center inside the span it can occupy without touching a wall —
// the same bounds confine_circle_to_square() clamps to? The star phases don't
// confine the ball, so this is how they notice it crossing an edge.
bool ball_inside_square(const World& w, const Config& cfg) {
    const float radius = ball_collider(w, cfg);
    const float min_x  = w.square_x + radius;
    const float max_x  = w.square_x + w.square_w - radius;
    const float min_y  = w.square_y + radius;
    const float max_y  = w.square_y + w.square_h - radius;
    if (max_x <= min_x || max_y <= min_y) return false; // no room to be inside

    return w.circle_x >= min_x && w.circle_x <= max_x &&
           w.circle_y >= min_y && w.circle_y <= max_y;
}

int dots_left(const World& w) {
    int count = 0;
    for (const Dot& d : w.dots) {
        if (!d.falling && !d.gone) ++count;
    }
    return count;
}

// Knocks the rightmost dot still in the row loose; it falls on its own from here.
void drop_next_dot(World& w) {
    for (int i = kDotCount - 1; i >= 0; --i) {
        Dot& d = w.dots[i];
        if (!d.falling && !d.gone) {
            d.falling = true;
            d.vy = kDotPop;
            return;
        }
    }
}

// Dots keep falling through every phase, so one drops away while play resumes.
void update_dots(World& w, const Config& cfg, float dt) {
    for (Dot& d : w.dots) {
        if (!d.falling || d.gone) continue;
        d.vy += kDotGravity * dt;
        d.y  += d.vy * dt;
        if (d.y - kDotRadius > static_cast<float>(cfg.window_h)) d.gone = true;
    }
}

// Drops a diamond anywhere on the field, square or not, and leaves it there.
// The ball never leaves the square, so one that lands outside is collected by
// steering the square onto it.
void spawn_diamond(World& w, const Config& cfg) {
    // Both margins are trimmed to what a small window can spare, so the band
    // they leave never inverts.
    const float room_x = static_cast<float>(cfg.window_w) * 0.5f - kDiamondHalfW - 4.0f;
    const float room_y = static_cast<float>(cfg.window_h) * 0.5f - kDiamondHalfH - 4.0f;
    const float inset_x = kDiamondHalfW + 4.0f +
                          std::clamp(cfg.diamond_edge_margin_x, 0.0f, std::max(room_x, 0.0f));
    const float inset_y = kDiamondHalfH + 4.0f +
                          std::clamp(cfg.diamond_edge_margin_y, 0.0f, std::max(room_y, 0.0f));
    const float max_x = static_cast<float>(cfg.window_w) - inset_x;
    const float max_y = static_cast<float>(cfg.window_h) - inset_y;
    if (max_x <= inset_x || max_y <= inset_y) return; // window too small to hold one

    float low_x = inset_x, high_x = max_x;
    float low_y = inset_y, high_y = max_y;

    // The first few land close to the middle. The band is drawn in toward the
    // center rather than replaced, so every margin worked out above still
    // holds — the opening band is a subset of the ordinary one, whatever the
    // window size — and the square starts centered too, which puts these
    // within the frame's reach instead of across the field from it.
    if (w.eaten < kOpeningDiamonds) {
        const float mid_x = static_cast<float>(cfg.window_w) * 0.5f;
        const float mid_y = static_cast<float>(cfg.window_h) * 0.5f;
        low_x  = mid_x + (low_x  - mid_x) * kOpeningSpread;
        high_x = mid_x + (high_x - mid_x) * kOpeningSpread;
        low_y  = mid_y + (low_y  - mid_y) * kOpeningSpread;
        high_y = mid_y + (high_y - mid_y) * kOpeningSpread;
    }

    // Never under the ball. One that lands inside the collection reach is eaten
    // on the frame it appears — a pickup the player never got to take — so the
    // spot has to be a good multiple of that reach away before it counts.
    const float clear = (ball_collider(w, cfg) + kDiamondReach) * kDiamondClear;
    float x = 0.0f;
    float y = 0.0f;
    for (int attempt = 0; attempt < kDiamondTries; ++attempt) {
        x = rand_range(w, low_x, high_x);
        y = rand_range(w, low_y, high_y);
        const float dx = x - w.circle_x;
        const float dy = y - w.circle_y;
        if (dx * dx + dy * dy >= clear * clear) break;
    }

    // The last try stands whatever it measured. Unlike a hazard, which can sit
    // the round out, the tally these feed is what unlocks the hazards and the
    // star, so the field is never left without one — a window too crowded to
    // place it properly gets a near miss rather than nothing at all.
    w.diamond.x      = x;
    w.diamond.y      = y;
    w.diamond.active = true;
}

// Runs the pickup: count down to the next spawn, then wait for the ball to
// reach the one on the field and hand out the boost.
void update_diamond(World& w, const Config& cfg, float dt) {
    // The opening is just the ball bouncing: nothing spawns until the player
    // first drives the square. The timer holds rather than drains while it
    // waits — like the hazard and star gates — so the first diamond arrives a
    // full gap after that push, not the instant it lands. Nothing can be on
    // the field to collect yet, so there is nothing else to run down here.
    if (!w.started) return;

    if (!w.diamond.active) {
        w.diamond_wait -= dt;
        if (w.diamond_wait <= 0.0f) {
            spawn_diamond(w, cfg);
            w.diamond_wait = rand_range(w, kDiamondGapMin, kDiamondGapMax);
        }
        return;
    }

    const float dx = w.circle_x - w.diamond.x;
    const float dy = w.circle_y - w.diamond.y;
    const float reach = ball_collider(w, cfg) + kDiamondReach;
    if (dx * dx + dy * dy > reach * reach) return;

    // Speed rides on the velocity itself, so only scale it up on a fresh boost;
    // a second diamond mid-boost just buys more time. Size has no such problem:
    // it is derived from `grow`, so it can stack freely and stay stacked.
    if (w.boost <= 0.0f) {
        w.circle_vx *= kBoostSpeed;
        w.circle_vy *= kBoostSpeed;
    }
    w.boost = kBoostTime;
    w.grow *= cfg.circle_growth_per_diamond;
    ++w.eaten;
    w.diamond.active = false;
}

// A hazard measures itself against its own kind's configured size.
float hazard_size(const Triangle& t, const Config& cfg) {
    const float base = t.moving ? cfg.moving_hazard_size : cfg.still_hazard_size;
    return base * t.scale;
}

SDL_Color hazard_color(const Triangle& t, const Config& cfg) {
    return t.moving ? cfg.moving_hazard_color : cfg.still_hazard_color;
}

// Two things of these sizes, at these points: is there room between them? Every
// spawn that has to stay off something else asks this.
bool spots_are_clear(float x, float y, float size,
                     float other_x, float other_y, float other_size) {
    const float reach = size + other_size + kSpawnPad;
    const float dx = x - other_x;
    const float dy = y - other_y;
    return dx * dx + dy * dy > reach * reach;
}

// Is this spot clear of every still hazard on the field? Stars and planted
// triangles both ask, so the two never land on top of each other.
bool clear_of_still_hazards(const World& w, const Config& cfg, float x, float y,
                            float size) {
    for (const Triangle& t : w.triangles) {
        if (!t.active || t.moving) continue;
        if (!spots_are_clear(x, y, size, t.x, t.y, hazard_size(t, cfg))) return false;
    }
    return true;
}

bool hex_clear_of(const Config& cfg, float x, float y,
                  float other_x, float other_y, float other_size) {
    return spots_are_clear(x, y, cfg.hexagon_size, other_x, other_y, other_size);
}

// Drops a star anywhere on screen. Unlike a diamond it is never collected: the
// ball is pulled to it, so it only has to be reachable, not inside the square.
bool spawn_star(World& w, const Config& cfg) {
    // Keep clear of the window edge by the configured margin, trimmed to what
    // the window can actually give up so a wide margin can't starve the band.
    const float room_x = static_cast<float>(cfg.window_w) * 0.5f - cfg.star_size - 1.0f;
    const float room_y = static_cast<float>(cfg.window_h) * 0.5f - cfg.star_size - 1.0f;
    const float margin_x = std::clamp(cfg.star_edge_margin, 0.0f, std::max(room_x, 0.0f));
    const float margin_y = std::clamp(cfg.star_edge_margin, 0.0f, std::max(room_y, 0.0f));

    const float min_x = cfg.star_size + margin_x;
    const float min_y = cfg.star_size + margin_y;
    const float max_x = static_cast<float>(cfg.window_w) - min_x;
    const float max_y = static_cast<float>(cfg.window_h) - min_y;
    if (max_x <= min_x || max_y <= min_y) return false;

    for (int attempt = 0; attempt < 16; ++attempt) {
        const float x = rand_range(w, min_x, max_x);
        const float y = rand_range(w, min_y, max_y);
        // The ball is sent straight to a star, so one planted on a hazard would
        // be a trap with no way around it.
        if (!clear_of_still_hazards(w, cfg, x, y, cfg.star_size)) continue;

        w.star.x      = x;
        w.star.y      = y;
        w.star.spin   = 0.0f; // every star arrives upright
        w.star.active = true;
        return true;
    }
    return false;
}

// Speed lives in the velocity vector, so read it back rather than recomputing it
// from config: that way a diamond boost still counts while the star steers.
float ball_speed(const World& w, const Config& cfg) {
    const float speed = std::sqrt(w.circle_vx * w.circle_vx + w.circle_vy * w.circle_vy);
    if (speed > 0.0f) return speed;
    return cfg.circle_speed * (w.boost > 0.0f ? kBoostSpeed : 1.0f);
}

// Points the ball straight at a target and steps it, unconfined by the square,
// at `chase_speed` — an absolute speed, not a scale on the ball's own. Reports
// arrival; the last step snaps, so the two centers land exactly on top of each
// other instead of jittering around the target.
//
// Position and velocity part ways here. The step uses `chase_speed`, so a star
// runs at the same pace whether or not a diamond boost is up; the stored vector
// keeps the ball's carried magnitude, which is what it is released with and
// what ball_speed() reads back on the next tick.
bool home_ball(World& w, const Config& cfg, float target_x, float target_y,
               float chase_speed, float dt) {
    const float dx = target_x - w.circle_x;
    const float dy = target_y - w.circle_y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float carried = ball_speed(w, cfg);
    const float travel = chase_speed * dt;

    if (distance <= travel || distance <= 0.0001f) {
        w.circle_x = target_x;
        w.circle_y = target_y;
        return true;
    }

    const float inv = 1.0f / distance;
    w.circle_vx = dx * inv * carried;
    w.circle_vy = dy * inv * carried;
    w.circle_x += dx * inv * travel;
    w.circle_y += dy * inv * travel;
    return false;
}

// Lets the ball go again at whatever speed it is carrying, on one of the four
// diagonals rather than a free angle — the same heading the game opens on, and
// the only one that keeps both axes bouncing.
void launch_ball(World& w, const Config& cfg) {
    const Uint32 quadrant = next_rand(w) >> 30; // two bits, one per axis
    const float diagonal = 0.70710678f;         // 1/sqrt(2)
    const float speed = ball_speed(w, cfg) * diagonal;
    w.circle_vx = (quadrant & 1u) ? -speed : speed;
    w.circle_vy = (quadrant & 2u) ? -speed : speed;
}

// A hexagon has to be driven to, and it has to be readable when you get there,
// so it keeps away from the square, the ball, every other pickup and every
// hazard on the field, and off the HUD rows top and bottom.
bool hex_spot_is_clear(const World& w, const Config& cfg, float x, float y) {
    const float margin = cfg.hexagon_size + 6.0f;
    if (x > w.square_x - margin && x < w.square_x + w.square_w + margin &&
        y > w.square_y - margin && y < w.square_y + w.square_h + margin) {
        return false;
    }
    if (y < cfg.diamond_edge_margin_y ||
        y > static_cast<float>(cfg.window_h) - cfg.diamond_edge_margin_y) {
        return false;
    }
    if (!hex_clear_of(cfg, x, y, w.circle_x, w.circle_y, ball_radius(w, cfg))) {
        return false;
    }
    if (w.diamond.active &&
        !hex_clear_of(cfg, x, y, w.diamond.x, w.diamond.y, kDiamondHalfH)) {
        return false;
    }
    if (w.star.active && !hex_clear_of(cfg, x, y, w.star.x, w.star.y, cfg.star_size)) {
        return false;
    }
    for (const Triangle& t : w.triangles) {
        if (!t.active) continue;
        // A crossing is still off screen while it waits, so judge it by the
        // lane it is about to run rather than where it happens to be parked.
        const float size = hazard_size(t, cfg);
        if (t.moving) {
            const float center_x = static_cast<float>(cfg.window_w) * 0.5f;
            const float center_y = static_cast<float>(cfg.window_h) * 0.5f;
            const float along_x = std::cos(t.heading);
            const float along_y = std::sin(t.heading);
            const float off_x = x - center_x;
            const float off_y = y - center_y;
            const float across = std::fabs(off_x * along_y - off_y * along_x);
            if (across < cfg.hexagon_size + size + kSpawnPad) return false;
        } else if (!hex_clear_of(cfg, x, y, t.x, t.y, size)) {
            return false;
        }
    }
    return true;
}

// Drops a hexagon somewhere clear of everything, so it has to be driven to
// rather than collected the instant it appears.
void spawn_hexagon(World& w, const Config& cfg) {
    const float margin = cfg.hexagon_size + 6.0f;
    const float max_x = static_cast<float>(cfg.window_w) - margin;
    const float max_y = static_cast<float>(cfg.window_h) - margin;
    if (max_x <= margin || max_y <= margin) return;

    for (int attempt = 0; attempt < 24; ++attempt) {
        const float x = rand_range(w, margin, max_x);
        const float y = rand_range(w, margin, max_y);
        if (!hex_spot_is_clear(w, cfg, x, y)) continue;
        w.hex.x      = x;
        w.hex.y      = y;
        w.hex.spin   = 0.0f; // every one arrives the same way up
        w.hex.active = true;
        return;
    }
}

// The square's pickup: touching one puts back half the difference between the
// size it has shrunk to and the size it started at, around its own center, and
// nudges it back inside the window if the growth pushed it out.
void update_hexagon(World& w, const Config& cfg, float dt) {
    if (!w.hex.active) {
        if (w.eaten < cfg.hexagon_unlock) return; // earned, like the hazards
        w.hex_wait -= dt;
        if (w.hex_wait <= 0.0f) {
            w.hex_wait = rand_range(w, cfg.hexagon_gap_min, cfg.hexagon_gap_max);
            spawn_hexagon(w, cfg);
        }
        return;
    }

    w.hex.spin += cfg.hexagon_spin_speed * kTwoPi * dt;

    // Nearest point on the square to the hexagon's center: the frame has caught
    // it once that point is within the hexagon.
    const float near_x = std::clamp(w.hex.x, w.square_x, w.square_x + w.square_w);
    const float near_y = std::clamp(w.hex.y, w.square_y, w.square_y + w.square_h);
    const float dx = w.hex.x - near_x;
    const float dy = w.hex.y - near_y;
    if (dx * dx + dy * dy > cfg.hexagon_size * cfg.hexagon_size) return;

    // Set the target, don't jump to it: grow_square() walks the walls back out.
    w.grow_to_w = w.square_w + (cfg.square_w - w.square_w) * kHexRecovery;
    w.grow_to_h = w.square_h + (cfg.square_h - w.square_h) * kHexRecovery;

    w.hex.active = false;
    w.hex_wait   = rand_range(w, cfg.hexagon_gap_min, cfg.hexagon_gap_max);
}

// Throws a set of shards out on an even fan, each a little different so the
// burst doesn't read as one expanding ring. Both the ball and a triangle come
// apart through here.
template <std::size_t N>
void fan_shards(std::array<Shard, N>& shards, float cx, float cy, float radius, float speed) {
    for (std::size_t i = 0; i < N; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(N);
        const float rate  = speed * (0.7f + 0.2f * static_cast<float>(i % 4));
        Shard& s = shards[i];
        s.x      = cx;
        s.y      = cy;
        s.vx     = std::cos(angle) * rate;
        s.vy     = std::sin(angle) * rate;
        s.radius = std::max(radius * (0.28f + 0.07f * static_cast<float>(i % 3)), 1.5f);
        s.alive  = true;
    }
}

void burst_ball(World& w, const Config& cfg) {
    fan_shards(w.shards, w.circle_x, w.circle_y, ball_radius(w, cfg), kShardSpeed);
    w.ball_alive = false;
}

// Flies the shards straight out, unconfined by the square, and reports whether
// any are still on screen.
template <std::size_t N>
bool update_shards(std::array<Shard, N>& shards, const Config& cfg, float dt) {
    const float right  = static_cast<float>(cfg.window_w);
    const float bottom = static_cast<float>(cfg.window_h);
    bool any = false;
    for (Shard& s : shards) {
        if (!s.alive) continue;
        s.x += s.vx * dt;
        s.y += s.vy * dt;
        if (s.x + s.radius < 0.0f || s.x - s.radius > right ||
            s.y + s.radius < 0.0f || s.y - s.radius > bottom) {
            s.alive = false;
        } else {
            any = true;
        }
    }
    return any;
}

// Turns the eye toward whatever it should be watching — the star it is about to
// chase, otherwise wherever the ball is heading — at a fixed rate, and reports
// how far it still has to go. `StarLook` waits on that number.
//
// It also slides the pupil back to the middle of the ball while the ball sits on
// a star, since there is nothing to look at once it has arrived.
float update_look(World& w, const Config& cfg, float dt) {
    const float gaze_target = (w.phase == Phase::StarHold) ? 0.0f : 1.0f;
    const float gaze_step = kGazeRate * dt;
    w.gaze += std::clamp(gaze_target - w.gaze, -gaze_step, gaze_step);

    float target = w.look;
    if (w.phase == Phase::StarLook && w.star.active && w.timer >= cfg.star_pause) {
        target = std::atan2(w.star.y - w.circle_y, w.star.x - w.circle_x);
    } else if (w.circle_vx != 0.0f || w.circle_vy != 0.0f) {
        target = std::atan2(w.circle_vy, w.circle_vx);
    }

    float delta = target - w.look;
    while (delta >  kPi) delta -= kTwoPi;
    while (delta < -kPi) delta += kTwoPi;

    const float step = kEyeTurnRate * dt;
    w.look += std::clamp(delta, -step, step);
    if (w.look >  kPi) w.look -= kTwoPi;
    if (w.look < -kPi) w.look += kTwoPi;

    return std::fabs(delta) - std::min(std::fabs(delta), step);
}

Triangle* free_hazard(World& w) {
    for (Triangle& t : w.triangles) {
        if (!t.active) return &t;
    }
    return nullptr; // field is full; the timer just skips a turn
}

// Where a hazard's triangles actually sit: a moving one is a pair straddling
// its center along the heading, a still one is the single triangle on it. Both
// drawing and collision go through this, so they can never disagree.
int hazard_lobes(const Triangle& t, const Config& cfg, float* xs, float* ys) {
    if (!t.moving) {
        xs[0] = t.x;
        ys[0] = t.y;
        return 1;
    }
    const float dx = std::cos(t.heading) * hazard_size(t, cfg) * kPairOffset;
    const float dy = std::sin(t.heading) * hazard_size(t, cfg) * kPairOffset;
    xs[0] = t.x - dx;
    ys[0] = t.y - dy;
    xs[1] = t.x + dx;
    ys[1] = t.y + dy;
    return 2;
}

// Sends a pair in from off screen, aimed dead at the middle of the window, so
// every one of them crosses the center on a different line.
void spawn_triangle(World& w, const Config& cfg) {
    Triangle* slot = free_hazard(w);
    if (!slot) return;

    const float center_x = static_cast<float>(cfg.window_w) * 0.5f;
    const float center_y = static_cast<float>(cfg.window_h) * 0.5f;
    const float reach = std::sqrt(center_x * center_x + center_y * center_y) +
                        cfg.moving_hazard_size * 3.0f;
    const float angle = rand_range(w, 0.0f, kTwoPi);

    slot->x  = center_x + std::cos(angle) * reach;
    slot->y  = center_y + std::sin(angle) * reach;
    slot->vx = -std::cos(angle) * cfg.moving_hazard_speed;
    slot->vy = -std::sin(angle) * cfg.moving_hazard_speed;
    slot->heading = angle + kPi; // it points the way it travels
    slot->scale  = 1.0f;
    slot->warn   = cfg.moving_hazard_warn; // waits behind its own telegraph
    slot->life   = 0.0f;
    slot->moving = true;
    slot->active = true;
}

// Plants a lone triangle somewhere outside the square, upright and at one of
// three sizes. It won't land on the ball — that would be a hit with nothing to
// react to — and it won't land inside the square, where the ball has nowhere to
// dodge to. It arrives as a harmless outline and arms itself later.
void spawn_still_triangle(World& w, const Config& cfg) {
    Triangle* slot = free_hazard(w);
    if (!slot) return;

    const Uint32 pick = (next_rand(w) >> 16) % kStillSizeCount;
    const float scale = 1.0f + kStillSizeStep * static_cast<float>(pick);
    const float size  = cfg.still_hazard_size * scale;

    const float margin = size * 2.0f;
    const float max_x = static_cast<float>(cfg.window_w) - margin;
    const float max_y = static_cast<float>(cfg.window_h) - margin;
    if (max_x <= margin || max_y <= margin) return;

    const float clear = ball_collider(w, cfg) + size * 3.0f;
    for (int attempt = 0; attempt < 12; ++attempt) {
        const float x = rand_range(w, margin, max_x);
        const float y = rand_range(w, margin, max_y);
        const float dx = x - w.circle_x;
        const float dy = y - w.circle_y;
        if (dx * dx + dy * dy < clear * clear) continue;

        if (w.star.active &&
            !spots_are_clear(x, y, size, w.star.x, w.star.y, cfg.star_size)) {
            continue;
        }

        // Not in the square, nor close enough to overlap its walls.
        if (x > w.square_x - size && x < w.square_x + w.square_w + size &&
            y > w.square_y - size && y < w.square_y + w.square_h + size) {
            continue;
        }

        slot->x  = x;
        slot->y  = y;
        slot->vx = 0.0f;
        slot->vy = 0.0f;
        slot->heading = -kPi * 0.5f; // upright, however big it is
        slot->scale  = scale;
        slot->warn   = cfg.still_hazard_arm;
        slot->life   = cfg.still_hazard_life;
        slot->moving = false;
        slot->active = true;
        return;
    }
}

// Flies the triangles across, retires the ones that have left, and reports a
// touch on the ball — which costs a dot exactly as a moving wall does. The
// triangle that lands it comes apart on the spot.
bool update_triangles(World& w, const Config& cfg, float dt) {
    update_shards(w.tri_shards, cfg, dt);

    // Both hazards are earned: neither appears until the tally along the bottom
    // has reached its own threshold, and each timer holds rather than running
    // down behind the scenes, so the first one comes a full interval after the
    // qualifying diamond rather than the instant it is eaten.
    if (w.eaten >= cfg.moving_hazard_unlock) {
        w.triangle_wait -= dt;
        if (w.triangle_wait <= 0.0f) {
            w.triangle_wait =
                rand_range(w, cfg.moving_hazard_gap_min, cfg.moving_hazard_gap_max);
            spawn_triangle(w, cfg);
        }
    }

    if (w.eaten >= cfg.still_hazard_unlock) {
        w.still_wait -= dt;
        if (w.still_wait <= 0.0f) {
            w.still_wait =
                rand_range(w, cfg.still_hazard_gap_min, cfg.still_hazard_gap_max);
            spawn_still_triangle(w, cfg);
        }
    }

    const float center_x = static_cast<float>(cfg.window_w) * 0.5f;
    const float center_y = static_cast<float>(cfg.window_h) * 0.5f;
    const float gone = std::sqrt(center_x * center_x + center_y * center_y) +
                       cfg.moving_hazard_size * 4.0f;
    bool struck = false;
    for (Triangle& t : w.triangles) {
        if (!t.active) continue;

        if (t.moving) {
            if (t.warn > 0.0f) {
                t.warn -= dt; // still just a strip on the field
                continue;
            }
            t.x += t.vx * dt;
            t.y += t.vy * dt;
            const float ox = t.x - center_x;
            const float oy = t.y - center_y;
            if (ox * ox + oy * oy > gone * gone) { // crossed and left the far side
                t.active = false;
                continue;
            }
        } else if (t.warn > 0.0f) {
            t.warn -= dt; // still an outline, still harmless
            continue;
        } else {
            t.life -= dt;
            if (t.life <= 0.0f) {
                t.active = false;
                continue;
            }
        }

        if (!w.ball_alive) continue;

        // Bigger triangles reach further, so the test is per hazard.
        const float reach = ball_collider(w, cfg) + hazard_size(t, cfg) * kTriangleHit;
        float xs[2], ys[2];
        const int lobes = hazard_lobes(t, cfg, xs, ys);
        for (int i = 0; i < lobes; ++i) {
            const float dx = xs[i] - w.circle_x;
            const float dy = ys[i] - w.circle_y;
            if (dx * dx + dy * dy > reach * reach) continue;

            fan_shards(w.tri_shards, t.x, t.y, hazard_size(t, cfg) * 0.5f, kTriShardSpeed);
            w.shard_tint = hazard_color(t, cfg);
            t.active = false;
            struck = true;
            break;
        }
    }
    return struck;
}

// Drives the square from the keyboard and reports whether the player was
// pushing it this tick — not whether it happens to be in motion. The damage
// rule keys off that, so a wall still coasting after the key is let go is free,
// the way an untouched wall always has been.
//
// The keys set a direction to accelerate along rather than a position; letting
// go hands the square to friction, which at the configured rate scrubs off a
// full turn of speed in a fraction of a second.
bool move_square(World& w, const Config& cfg, const Uint8* keys, float dt) {
    float push_x = 0.0f, push_y = 0.0f;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) push_x -= 1.0f;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) push_x += 1.0f;
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) push_y -= 1.0f;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) push_y += 1.0f;

    const bool pushing = (push_x != 0.0f || push_y != 0.0f);

    if (pushing) {
        if (push_x != 0.0f && push_y != 0.0f) { // no free speed on the diagonal
            const float inv = 0.70710678f;
            push_x *= inv;
            push_y *= inv;
        }
        w.square_vx += push_x * cfg.square_acceleration * dt;
        w.square_vy += push_y * cfg.square_acceleration * dt;

        // Cap the vector, not each axis, or a diagonal would outrun a straight
        // line the way the old normalization already avoided.
        const float speed =
            std::sqrt(w.square_vx * w.square_vx + w.square_vy * w.square_vy);
        if (speed > cfg.square_speed && speed > 0.0f) {
            const float trim = cfg.square_speed / speed;
            w.square_vx *= trim;
            w.square_vy *= trim;
        }
    } else {
        // Friction takes a fixed bite per second and never overshoots into
        // reverse, so the square coasts to a stop rather than rebounding.
        const float speed =
            std::sqrt(w.square_vx * w.square_vx + w.square_vy * w.square_vy);
        const float shed = cfg.square_friction * dt;
        if (speed <= shed || speed <= 0.0f) {
            w.square_vx = 0.0f;
            w.square_vy = 0.0f;
        } else {
            const float trim = (speed - shed) / speed;
            w.square_vx *= trim;
            w.square_vy *= trim;
        }
    }

    w.square_x += w.square_vx * dt;
    w.square_y += w.square_vy * dt;

    // The window edge is a dead stop, not a wall to slide along at speed.
    const float max_x = static_cast<float>(cfg.window_w) - w.square_w;
    const float max_y = static_cast<float>(cfg.window_h) - w.square_h;
    if (w.square_x < 0.0f || w.square_x > max_x) {
        w.square_x = std::clamp(w.square_x, 0.0f, max_x);
        w.square_vx = 0.0f;
    }
    if (w.square_y < 0.0f || w.square_y > max_y) {
        w.square_y = std::clamp(w.square_y, 0.0f, max_y);
        w.square_vy = 0.0f;
    }

    return pushing;
}

// Resizes the square around its own center and keeps it in the window. Both the
// shrink and a hexagon's recovery go through here, so the box never lurches off
// in one direction as it changes size.
void resize_square(World& w, const Config& cfg, float next_w, float next_h) {
    w.square_x += (w.square_w - next_w) * 0.5f;
    w.square_y += (w.square_h - next_h) * 0.5f;
    w.square_w = next_w;
    w.square_h = next_h;
    w.square_x = std::clamp(w.square_x, 0.0f, static_cast<float>(cfg.window_w) - w.square_w);
    w.square_y = std::clamp(w.square_y, 0.0f, static_cast<float>(cfg.window_h) - w.square_h);
}

// Walks the walls back out toward whatever a hexagon promised, and reports that
// it is still doing so — the shrink holds off until it lands.
bool grow_square(World& w, const Config& cfg, float dt) {
    if (w.grow_to_w <= w.square_w && w.grow_to_h <= w.square_h) {
        w.grow_to_w = 0.0f; // nothing pending
        w.grow_to_h = 0.0f;
        return false;
    }

    const float step = cfg.hexagon_grow_rate * dt;
    resize_square(w, cfg, std::min(w.square_w + step, w.grow_to_w),
                  std::min(w.square_h + step, w.grow_to_h));
    return true;
}

// Closes the square in around its own center. The floor keeps the circle able
// to fit, whatever diamonds have done to it.
void shrink_square(World& w, const Config& cfg, float dt) {
    const float floor_size = std::max(cfg.square_min_size, ball_collider(w, cfg) * 2.0f);
    const float step = cfg.square_shrink_rate * dt;
    resize_square(w, cfg, std::max(w.square_w - step, floor_size),
                  std::max(w.square_h - step, floor_size));
}

// Spends the speed boost. The size half of a diamond never lapses, so this only
// has the velocity to undo.
void age_boost(World& w, float dt) {
    if (w.boost <= 0.0f) return;
    w.boost -= dt;
    if (w.boost <= 0.0f) {
        w.boost = 0.0f;
        w.circle_vx /= kBoostSpeed;
        w.circle_vy /= kBoostSpeed;
    }
}

// One phase runs at a time: only Play bounces the ball off the walls, so
// everything else holds still while a hit or a star plays out.
void step(World& w, const Config& cfg, const Uint8* keys, float dt) {
    update_dots(w, cfg, dt);
    const float aim_error = update_look(w, cfg, dt);

    // A frame that isn't going anywhere fades back, so the eye is drawn to the
    // one that is. Only the phases that actually drive it count as moving —
    // during a shake the velocity is merely stale. It stays solid through a
    // collision, though: the wall that landed the hit holds its weight until
    // the ball has finished reacting to it.
    const bool drivable = w.phase == Phase::Play || w.phase == Phase::StarLook ||
                          w.phase == Phase::StarSeek || w.phase == Phase::StarHold;
    const bool colliding = w.phase == Phase::Shake || w.phase == Phase::Burst;
    const bool driven = drivable && (w.square_vx != 0.0f || w.square_vy != 0.0f);
    const float alpha_target = (colliding || driven) ? 1.0f : cfg.square_idle_alpha;
    const float alpha_step = kSquareFadeRate * dt;
    w.square_alpha += std::clamp(alpha_target - w.square_alpha, -alpha_step, alpha_step);
    if (w.grace > 0.0f) w.grace -= dt;
    w.timer += dt;

    switch (w.phase) {
    case Phase::Play: {
        const bool square_moving = move_square(w, cfg, keys, dt);
        if (square_moving) w.started = true; // the run proper begins here

        // Only open play closes the walls in: a diamond boost holds them, and
        // so does every star phase, none of which come through here. A hexagon's
        // recovery holds them too, until it has finished opening them out. So
        // does the opening, until the player has taken hold of the square.
        if (!grow_square(w, cfg, dt) && w.boost <= 0.0f && w.started) {
            shrink_square(w, cfg, dt);
        }

        // Let the speed boost lapse before the bounce, so a tick is never
        // integrated at one speed and reflected at another.
        age_boost(w, dt);

        w.circle_x += w.circle_vx * dt;
        w.circle_y += w.circle_vy * dt;

        const bool hit = confine_circle_to_square(w, cfg);
        update_diamond(w, cfg, dt);
        update_hexagon(w, cfg, dt);
        const bool struck = update_triangles(w, cfg, dt);

        // Only a wall that was on the move costs a dot; an idle bounce is free,
        // and `grace` keeps a held key from eating every dot at once. A triangle
        // costs one however it is met, window or no window: it bursts on
        // contact, so unlike a wall it cannot land the same hit twice, and
        // forgiving it would spend the hazard for nothing.
        const bool walled = hit && square_moving;
        if (struck || (walled && w.grace <= 0.0f)) {
            w.flash = walled; // a wall's shake flashes; a hazard's does not
            w.phase = Phase::Shake;
            w.timer = 0.0f;
            break;
        }

        // A star takes the ball over the moment it appears — but like the
        // hazards, it is earned first, and its timer holds until then.
        if (w.eaten >= cfg.star_unlock_diamonds) {
            w.star_wait -= dt;
            if (w.star_wait <= 0.0f) {
                w.star_wait = rand_range(w, cfg.star_gap_min, cfg.star_gap_max);
                if (spawn_star(w, cfg)) {
                    w.phase = Phase::StarLook;
                    w.timer = 0.0f;
                }
            }
        }
        break;
    }

    case Phase::StarLook: {
        // Dead stop while the eye comes around. The square is still the
        // player's, so the same edge-crossing rule as the chase applies.
        const bool was_inside = ball_inside_square(w, cfg);

        move_square(w, cfg, keys, dt);
        age_boost(w, dt);
        update_diamond(w, cfg, dt);
        update_hexagon(w, cfg, dt);
        grow_square(w, cfg, dt); // a recovery in progress keeps opening out
        const bool struck = update_triangles(w, cfg, dt);

        const bool crossed = ball_inside_square(w, cfg) != was_inside;
        if (struck || (crossed && w.grace <= 0.0f)) {
            w.flash = crossed; // the wall passing through it counts as a wall
            w.phase = Phase::Shake;
            w.timer = 0.0f;
            break;
        }

        // Both bounds sit past the pause, so the eye can't be called "aimed"
        // during the hold, when it is still pointing the old way.
        if ((aim_error <= kEyeAimed && w.timer >= cfg.star_pause + kStarTurnMin) ||
            w.timer >= cfg.star_pause + kStarTurnMax) {
            w.phase = Phase::StarSeek;
            w.timer = 0.0f;
        }
        break;
    }

    case Phase::StarSeek: {
        // Out of the square and straight to the star. Nothing confines the ball
        // here, so the wall doesn't stop it — but crossing that wall still costs
        // a dot, the same as a moving wall catching it in Play.
        const bool was_inside = ball_inside_square(w, cfg);

        move_square(w, cfg, keys, dt);
        age_boost(w, dt);
        update_diamond(w, cfg, dt);
        update_hexagon(w, cfg, dt);
        grow_square(w, cfg, dt);
        const bool struck = update_triangles(w, cfg, dt);
        const bool arrived = home_ball(w, cfg, w.star.x, w.star.y,
                                      cfg.circle_speed * cfg.star_seek_speed, dt);

        // Either direction counts: the ball crossing an edge on its way out, or
        // the player driving an edge into it while it is out there. A triangle
        // caught mid-chase costs the same dot.
        const bool crossed = ball_inside_square(w, cfg) != was_inside;
        if (struck || (crossed && w.grace <= 0.0f)) {
            w.flash = crossed; // the wall passing through it counts as a wall
            w.phase = Phase::Shake;
            w.timer = 0.0f;
            break;
        }

        if (arrived || w.timer >= kStarTimeout) {
            w.phase = Phase::StarHold;
            w.timer = 0.0f;
        }
        break;
    }

    case Phase::StarHold: {
        // Caught: the ball parks where the star is and the star spins on that
        // spot for `star.hold`, then lets it go on a fresh diagonal.
        const bool was_inside = ball_inside_square(w, cfg);

        move_square(w, cfg, keys, dt);
        age_boost(w, dt);
        update_diamond(w, cfg, dt);
        update_hexagon(w, cfg, dt);
        grow_square(w, cfg, dt);
        const bool struck = update_triangles(w, cfg, dt);
        w.star.spin += cfg.star_spin_speed * kTwoPi * dt;

        const bool crossed = ball_inside_square(w, cfg) != was_inside;
        if (struck || (crossed && w.grace <= 0.0f)) {
            w.flash = crossed; // the wall passing through it counts as a wall
            w.phase = Phase::Shake;
            w.timer = 0.0f;
            break;
        }

        if (w.timer >= cfg.star_hold) {
            w.star.active = false;

            // The chase is a loan, and bringing the square out to meet the ball
            // is the price of it. Caught out here when the star lets go, every
            // dot still up drops at once and the ball goes with them. Dropping
            // them before the shake rather than during it is what turns the
            // ordinary sequence into this one: `Phase::Shake` bursts the ball
            // whenever it finds nothing left to take, so the rattle plays, the
            // whole row arcs off the bottom, and the burst follows.
            if (!ball_inside_square(w, cfg)) {
                while (dots_left(w) > 0) drop_next_dot(w);
                w.phase = Phase::Shake;
                w.timer = 0.0f;
                break;
            }

            launch_ball(w, cfg);
            w.phase = Phase::Play;
            w.timer = 0.0f;
            // The ball is wholly inside by the test above, so this is only
            // covering the wall the player may be driving at the moment of it.
            w.grace = kHitGrace;
        }
        break;
    }

    case Phase::Shake:
        if (w.timer >= kShakeTime) {
            drop_next_dot(w);
            if (dots_left(w) > 0) {
                // A star still out means the shake interrupted the chase, so go
                // back to it rather than to open play.
                w.phase = w.star.active ? Phase::StarSeek : Phase::Play;
                w.timer = 0.0f;
                w.grace = kHitGrace;
                w.flash = false; // recovered
            } else {
                burst_ball(w, cfg);
                w.phase = Phase::Burst;
                w.timer = 0.0f;
            }
        }
        break;

    case Phase::Burst:
        update_shards(w.tri_shards, cfg, dt);
        if (!update_shards(w.shards, cfg, dt) || w.timer >= kShardTimeout) {
            w.phase = Phase::FadeOut;
            w.timer = 0.0f;
        }
        break;

    case Phase::FadeOut:
        // Rebuild behind the black, so fading back up reveals a fresh game.
        if (w.timer >= kFadeOutTime) {
            w = make_world(cfg);
            w.phase = Phase::Black;
        }
        break;

    case Phase::Black:
        if (w.timer >= kBlackTime) {
            w.phase = Phase::FadeIn;
            w.timer = 0.0f;
        }
        break;

    case Phase::FadeIn:
        if (w.timer >= kFadeInTime) {
            w.phase = Phase::Play;
            w.timer = 0.0f;
        }
        break;
    }
}

void set_draw_color(SDL_Renderer* renderer, SDL_Color c) {
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
}

// Horizontal-span fill: one line per pixel row of the circle.
void fill_circle(SDL_Renderer* renderer, float cx, float cy, float radius) {
    const int span = static_cast<int>(radius);
    for (int dy = -span; dy <= span; ++dy) {
        const float row = static_cast<float>(dy);
        const float half = std::sqrt(std::max(radius * radius - row * row, 0.0f));
        const int y = static_cast<int>(std::lround(cy + row));
        SDL_RenderDrawLine(renderer,
                           static_cast<int>(std::lround(cx - half)), y,
                           static_cast<int>(std::lround(cx + half)), y);
    }
}

// Same row-by-row fill as the circle, with the span tapering linearly instead.
void fill_diamond(SDL_Renderer* renderer, float cx, float cy, float half_w, float half_h) {
    const int span = static_cast<int>(half_h);
    for (int dy = -span; dy <= span; ++dy) {
        const float row = static_cast<float>(dy);
        const float half = half_w * (1.0f - std::fabs(row) / half_h);
        const int y = static_cast<int>(std::lround(cy + row));
        SDL_RenderDrawLine(renderer,
                           static_cast<int>(std::lround(cx - half)), y,
                           static_cast<int>(std::lround(cx + half)), y);
    }
}

// A capsule drawn as a line of discs: cheap, and it reuses the one fill
// primitive everything round in this game already goes through.
void fill_capsule(SDL_Renderer* renderer, float x0, float y0, float x1, float y1,
                  float radius) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::sqrt(dx * dx + dy * dy);
    const int steps = std::max(static_cast<int>(std::lround(length)), 1);
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        fill_circle(renderer, x0 + dx * t, y0 + dy * t, radius);
    }
}

// The eye: a bare pupil roaming the ball's top two thirds, drawn straight onto
// the pink. Everything is a fraction of the radius it is handed, so passing the
// ball's current radius scales the whole eye with every diamond.
// A ring, filled a row at a time between two radii — the same span-per-row idea
// as fill_circle(), and for the same reason: a polyline around the rim facets
// at anything but a small radius, where this follows the true circle at every
// row the way the ball itself does. Rows clear of the hole are one span; the
// rest are two, either side of it.
void fill_ring(SDL_Renderer* renderer, float cx, float cy, float outer, float inner) {
    const int span = static_cast<int>(outer);
    for (int dy = -span; dy <= span; ++dy) {
        const float row = static_cast<float>(dy);
        const float out_half = std::sqrt(std::max(outer * outer - row * row, 0.0f));
        const int y = static_cast<int>(std::lround(cy + row));
        const int left  = static_cast<int>(std::lround(cx - out_half));
        const int right = static_cast<int>(std::lround(cx + out_half));

        if (std::fabs(row) >= inner) { // past the hole: the row is solid
            SDL_RenderDrawLine(renderer, left, y, right, y);
            continue;
        }
        const float in_half = std::sqrt(std::max(inner * inner - row * row, 0.0f));
        SDL_RenderDrawLine(renderer, left, y,
                           static_cast<int>(std::lround(cx - in_half)), y);
        SDL_RenderDrawLine(renderer, static_cast<int>(std::lround(cx + in_half)), y,
                           right, y);
    }
}

// The ball's color this frame. A shake a driven wall started swaps it for the
// square's own color and back, `kFlashRate` times a second, and runs for the
// whole of that shake — so it ends when the ball recovers, and at a burst it
// ends because there is no longer a ball to draw. The square's full-strength
// color is used, not the faded one an idle frame is drawn in, though a wall
// being driven is at full strength regardless. `World::timer` is the shake's
// own clock and starts at zero, so the first frame of the hit is the square's.
SDL_Color ball_color(const World& w, const Config& cfg) {
    if (!w.flash) return cfg.circle_color;
    const int swap = static_cast<int>(w.timer * kFlashRate);
    return (swap & 1) ? cfg.circle_color : cfg.square_color;
}

void fill_eye(SDL_Renderer* renderer, const World& w, float cx, float cy, float radius) {
    // The eye rests above center so the lower third or so reads as body. `gaze`
    // carries the rise as well as the reach: at rest on a star the eye slides
    // all the way to the middle of the body.
    cy -= radius * kEyeRise * w.gaze;

    const float pupil_r = std::max(radius * kPupilSize, 1.0f);
    const float reach   = radius * kEyeTravel * w.gaze;

    // The look only moves the pupil; the capsule itself never rotates, so the
    // eye reads the same way up whichever direction the ball is travelling.
    const float px = cx + std::cos(w.look) * reach;
    const float py = cy + std::sin(w.look) * reach;
    // The stretch goes with `gaze` too, so a centered eye is a plain circle.
    const float half_len = pupil_r * kPupilStretch * w.gaze;

    const float cap_r = pupil_r * kPupilThin;
    set_draw_color(renderer, kEyePupil);
    fill_capsule(renderer, px - half_len, py, px + half_len, py, cap_r);

    // A speck of white in the right-hand end, lifted just off the middle of the
    // cap. Both are fractions of the cap, so the highlight rides every scale the
    // eye does — the ball's growth, and `gaze` pulling the capsule closed — and
    // it stays put in the corner rather than swimming as the pupil turns, since
    // the capsule never rotates. fill_circle() bottoms out at a single pixel, so
    // on a small ball this is a dot rather than nothing at all.
    set_draw_color(renderer, kEyeGlint);
    fill_circle(renderer, px + half_len, py - cap_r * kGlintRise, cap_r * kGlintSize);
}

// Even-odd scanline fill: one line per pixel row, spans between sorted edge
// crossings. Same one-line-per-row idea as the circle, but it covers concave
// outlines the closed-form span cannot — which is what the star needs.
constexpr int kMaxPolyPoints = 12;

void fill_polygon(SDL_Renderer* renderer, const float* px, const float* py, int count) {
    if (count < 3 || count > kMaxPolyPoints) return;

    float top = py[0], bottom = py[0];
    for (int i = 1; i < count; ++i) {
        top    = std::min(top, py[i]);
        bottom = std::max(bottom, py[i]);
    }

    for (int y = static_cast<int>(std::lround(top));
         y <= static_cast<int>(std::lround(bottom)); ++y) {
        const float row = static_cast<float>(y) + 0.5f;
        float crossings[kMaxPolyPoints];
        int found = 0;
        for (int i = 0; i < count; ++i) {
            const int j = (i + 1) % count;
            if ((row >= py[i]) == (row >= py[j])) continue; // edge misses this row
            const float t = (row - py[i]) / (py[j] - py[i]);
            crossings[found++] = px[i] + (px[j] - px[i]) * t;
        }
        std::sort(crossings, crossings + found);
        for (int i = 0; i + 1 < found; i += 2) {
            SDL_RenderDrawLine(renderer,
                               static_cast<int>(std::lround(crossings[i])), y,
                               static_cast<int>(std::lround(crossings[i + 1])), y);
        }
    }
}

void fill_star(SDL_Renderer* renderer, float cx, float cy, float outer, float inner,
               float spin) {
    constexpr int kPoints = 10;
    float px[kPoints], py[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        // Points up at spin 0, then wherever the hold has wound it to.
        const float angle = -kPi * 0.5f + kPi * static_cast<float>(i) / 5.0f + spin;
        const float r = (i % 2 == 0) ? outer : inner;
        px[i] = cx + std::cos(angle) * r;
        py[i] = cy + std::sin(angle) * r;
    }
    fill_polygon(renderer, px, py, kPoints);
}

// The lane a crossing will run down: a long translucent band through the middle
// of the window, laid along the hazard's heading.
void fill_path_strip(SDL_Renderer* renderer, const Config& cfg, const Triangle& t,
                     float size) {
    const float center_x = static_cast<float>(cfg.window_w) * 0.5f;
    const float center_y = static_cast<float>(cfg.window_h) * 0.5f;
    const float half_len =
        std::sqrt(center_x * center_x + center_y * center_y) + size * 4.0f;
    const float half_w = size * kStripHalfWidth;

    const float dir_x = std::cos(t.heading);
    const float dir_y = std::sin(t.heading);
    const float side_x = -dir_y * half_w; // perpendicular to the run
    const float side_y = dir_x * half_w;

    const float px[4] = {
        center_x - dir_x * half_len + side_x, center_x + dir_x * half_len + side_x,
        center_x + dir_x * half_len - side_x, center_x - dir_x * half_len - side_x,
    };
    const float py[4] = {
        center_y - dir_y * half_len + side_y, center_y + dir_y * half_len + side_y,
        center_y + dir_y * half_len - side_y, center_y - dir_y * half_len - side_y,
    };
    fill_polygon(renderer, px, py, 4);
}

void fill_hexagon(SDL_Renderer* renderer, float cx, float cy, float size, float spin) {
    constexpr int kPoints = 6;
    float px[kPoints], py[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const float angle = -kPi * 0.5f + kPi * static_cast<float>(i) / 3.0f + spin;
        px[i] = cx + std::cos(angle) * size;
        py[i] = cy + std::sin(angle) * size;
    }
    fill_polygon(renderer, px, py, kPoints);
}

void triangle_points(float cx, float cy, float size, float heading,
                     float* px, float* py) {
    for (int i = 0; i < 3; ++i) {
        const float angle = heading + kTwoPi * static_cast<float>(i) / 3.0f;
        px[i] = cx + std::cos(angle) * size;
        py[i] = cy + std::sin(angle) * size;
    }
}

// One corner leads, so the triangle points the way it is travelling.
void fill_triangle(SDL_Renderer* renderer, float cx, float cy, float size, float heading) {
    float px[3], py[3];
    triangle_points(cx, cy, size, heading, px, py);
    fill_polygon(renderer, px, py, 3);
}

// The unarmed state: the same outline, hollow, so a hazard that cannot hurt you
// yet never looks like one that can.
void draw_triangle(SDL_Renderer* renderer, float cx, float cy, float size, float heading) {
    float px[3], py[3];
    triangle_points(cx, cy, size, heading, px, py);
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        SDL_RenderDrawLine(renderer,
                           static_cast<int>(std::lround(px[i])), static_cast<int>(std::lround(py[i])),
                           static_cast<int>(std::lround(px[j])), static_cast<int>(std::lround(py[j])));
    }
}

// How much black covers the frame: opaque through the hold, ramped either side.
Uint8 fade_alpha(const World& w) {
    float amount = 0.0f;
    if (w.phase == Phase::FadeOut) {
        amount = w.timer / kFadeOutTime;
    } else if (w.phase == Phase::Black) {
        amount = 1.0f;
    } else if (w.phase == Phase::FadeIn) {
        amount = 1.0f - w.timer / kFadeInTime;
    }
    return static_cast<Uint8>(std::lround(std::clamp(amount, 0.0f, 1.0f) * 255.0f));
}


// -- The tube ----------------------------------------------------------------
// Everything above draws in config-space pixels and never learns what the
// window is really showing; this is the only code that knows. The field goes to
// an off-screen frame at config size, and that frame is what gets bent onto the
// glass. SDL's 2D renderer has no shader stage, so the curve lives in vertex
// positions and the vignette in vertex colors, over a mesh fine enough that the
// shading between its corners reads as a gradient.

constexpr int kTubeCols  = 32; // mesh resolution, across and down
constexpr int kTubeRows  = 24;
constexpr int kTubeVerts = (kTubeCols + 1) * (kTubeRows + 1);
constexpr int kTubeIndex = kTubeCols * kTubeRows * 6;
constexpr int kGlowStep  = 2;  // each blur pass halves the picture

struct Screen {
    SDL_Texture* frame = nullptr; // the field, drawn at config size
    SDL_Texture* half  = nullptr; // halfway down to the blur
    SDL_Texture* glow  = nullptr; // quarter size, added back for phosphor bloom
    SDL_Texture* lines = nullptr; // 1 x height scanline mask
};

void free_screen(Screen& s) {
    if (s.frame) SDL_DestroyTexture(s.frame);
    if (s.half)  SDL_DestroyTexture(s.half);
    if (s.glow)  SDL_DestroyTexture(s.glow);
    if (s.lines) SDL_DestroyTexture(s.lines);
    s = Screen{};
}

// Sized off the config, so a reload rebuilds it. A failure here leaves `frame`
// null, which every pass below reads as "the game drew straight to the window"
// - a renderer that cannot hold a target still gets a game, just a flat one.
void build_screen(Screen& s, SDL_Renderer* renderer, const Config& cfg) {
    free_screen(s);

    s.frame = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_TARGET, cfg.window_w, cfg.window_h);
    if (!s.frame) {
        SDL_Log("no render target (%s); CRT off", SDL_GetError());
        return;
    }
    SDL_SetTextureScaleMode(s.frame, SDL_ScaleModeLinear);
    // Everything is laid onto black and added, so the three color passes sum
    // instead of painting over one another.
    SDL_SetTextureBlendMode(s.frame, SDL_BLENDMODE_ADD);

    const int half_w = std::max(cfg.window_w / kGlowStep, 1);
    const int half_h = std::max(cfg.window_h / kGlowStep, 1);
    s.half = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_TARGET, half_w, half_h);
    s.glow = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_TARGET,
                               std::max(half_w / kGlowStep, 1),
                               std::max(half_h / kGlowStep, 1));
    for (SDL_Texture* tex : {s.half, s.glow}) {
        if (!tex) continue;
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD);
    }

    // The scanline mask: one pixel wide, one row per config-space row, black at
    // whatever alpha the gaps want. It is stretched with the picture rather than
    // built per output pixel, so the lines keep their relationship to the art at
    // any window size - `line_gap` is tuned against the game, not the monitor.
    // The band is a cosine rather than a hard row, which keeps it from beating
    // against the pixel grid once the curve has stretched it.
    if (cfg.crt_scanlines > 0.0f) {
        std::vector<Uint32> mask(static_cast<size_t>(cfg.window_h));
        for (int y = 0; y < cfg.window_h; ++y) {
            const float phase = 2.0f * 3.14159265f * static_cast<float>(y) / cfg.crt_line_gap;
            const float dark  = 0.5f - 0.5f * std::cos(phase);
            const Uint32 alpha = static_cast<Uint32>(
                std::lround(std::clamp(cfg.crt_scanlines * dark, 0.0f, 1.0f) * 255.0f));
            mask[static_cast<size_t>(y)] = alpha << 24; // black, so only alpha carries
        }
        s.lines = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STATIC, 1, cfg.window_h);
        if (s.lines) {
            SDL_UpdateTexture(s.lines, nullptr, mask.data(), sizeof(Uint32));
            SDL_SetTextureScaleMode(s.lines, SDL_ScaleModeLinear);
            SDL_SetTextureBlendMode(s.lines, SDL_BLENDMODE_BLEND); // this one darkens
        }
    }
}

// Where the picture sits in the window: as large as it goes with its shape kept,
// centered, black either side. This is all that fullscreen actually changes.
SDL_FRect picture_rect(SDL_Renderer* renderer, const Config& cfg) {
    int out_w = cfg.window_w;
    int out_h = cfg.window_h;
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
    const float scale = std::min(static_cast<float>(out_w) / static_cast<float>(cfg.window_w),
                                 static_cast<float>(out_h) / static_cast<float>(cfg.window_h));
    const float w = static_cast<float>(cfg.window_w) * scale;
    const float h = static_cast<float>(cfg.window_h) * scale;
    return SDL_FRect{(static_cast<float>(out_w) - w) * 0.5f,
                     (static_cast<float>(out_h) - h) * 0.5f, w, h};
}

// One pass of a texture onto the glass. `spread` scales the whole picture about
// its center, which is the whole of the color fringing: the same image laid down
// three times at three sizes, one channel each, so the split is nothing in the
// middle and widest at the rim.
void draw_tube(SDL_Renderer* renderer, SDL_Texture* tex, const SDL_FRect& dest,
               const Config& cfg, float spread, SDL_Color tint) {
    if (!tex) return;

    std::array<SDL_Vertex, kTubeVerts> verts{};
    std::array<int, kTubeIndex> index{};

    const float mid_x  = dest.x + dest.w * 0.5f;
    const float mid_y  = dest.y + dest.h * 0.5f;
    // The pull below draws every edge in, so open the mesh back out by what it
    // takes off an edge midpoint: those meet the window exactly, and the corners
    // - drawn in harder, on a longer radius - stay inside it as rounded black.
    const float fill   = 1.0f + cfg.crt_curvature;
    const float half_w = dest.w * 0.5f * spread * fill;
    const float half_h = dest.h * 0.5f * spread * fill;

    for (int row = 0; row <= kTubeRows; ++row) {
        for (int col = 0; col <= kTubeCols; ++col) {
            const float u = static_cast<float>(col) / static_cast<float>(kTubeCols);
            const float v = static_cast<float>(row) / static_cast<float>(kTubeRows);
            const float nx = u * 2.0f - 1.0f;
            const float ny = v * 2.0f - 1.0f;
            const float r2 = nx * nx + ny * ny; // 1 at the edge midpoints, 2 at the corners

            // Draw the grid in toward the middle by the square of how far out it
            // is. The picture packs together as it nears the rim the way it does
            // on curved glass, and its own edges bow outward into the rounded
            // shape of a tube face - corners pulled in furthest, which is what
            // keeps the whole picture inside the window with no overscan to trim.
            const float pull = 1.0f / (1.0f + cfg.crt_curvature * r2);

            // The corners fall off, on the same r^2 the curve rides. Only the
            // color is shaded, never the alpha, so the mask laid over the top
            // darkens by as much at the rim as it does in the middle.
            const float shade = std::clamp(1.0f - cfg.crt_vignette * r2 * 0.5f, 0.0f, 1.0f);

            SDL_Vertex& vert = verts[static_cast<size_t>(row * (kTubeCols + 1) + col)];
            vert.position.x  = mid_x + nx * pull * half_w;
            vert.position.y  = mid_y + ny * pull * half_h;
            vert.tex_coord.x = u;
            vert.tex_coord.y = v;
            vert.color.r = static_cast<Uint8>(std::lround(static_cast<float>(tint.r) * shade));
            vert.color.g = static_cast<Uint8>(std::lround(static_cast<float>(tint.g) * shade));
            vert.color.b = static_cast<Uint8>(std::lround(static_cast<float>(tint.b) * shade));
            vert.color.a = tint.a;
        }
    }

    int at = 0;
    for (int row = 0; row < kTubeRows; ++row) {
        for (int col = 0; col < kTubeCols; ++col) {
            const int top_left  = row * (kTubeCols + 1) + col;
            const int top_right = top_left + 1;
            const int low_left  = top_left + (kTubeCols + 1);
            const int low_right = low_left + 1;
            index[static_cast<size_t>(at++)] = top_left;
            index[static_cast<size_t>(at++)] = top_right;
            index[static_cast<size_t>(at++)] = low_right;
            index[static_cast<size_t>(at++)] = top_left;
            index[static_cast<size_t>(at++)] = low_right;
            index[static_cast<size_t>(at++)] = low_left;
        }
    }

    SDL_RenderGeometry(renderer, tex, verts.data(), kTubeVerts,
                       index.data(), kTubeIndex);
}

// Clear a target and lay the whole of `src` over it - one step of the blur.
void copy_into(SDL_Renderer* renderer, SDL_Texture* target, SDL_Texture* src) {
    if (!target || !src) return;
    SDL_SetRenderTarget(renderer, target);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(renderer); // the copy adds, so it can't be laid on stale light
    SDL_RenderCopy(renderer, src, nullptr, nullptr);
}

// The finished frame onto the window: curve, fringe, bloom, scanlines, present.
void present_screen(SDL_Renderer* renderer, const Screen& screen, const Config& cfg) {
    if (!screen.frame) { // no render target - the game drew straight to the window
        SDL_RenderPresent(renderer);
        return;
    }

    const bool glowing = cfg.crt_enabled && cfg.crt_glow > 0.0f;
    if (glowing) {
        // Two halvings: each is a bilinear filter averaging a 2x2 block, which
        // buys a box blur for the price of two blits. Going straight down to a
        // quarter would skip three pixels in four instead of averaging them.
        copy_into(renderer, screen.half, screen.frame);
        copy_into(renderer, screen.glow, screen.half);
    }

    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(renderer); // the letterbox, and the corners the curve leaves bare

    const SDL_FRect dest = picture_rect(renderer, cfg);
    if (!cfg.crt_enabled) {
        SDL_RenderCopyF(renderer, screen.frame, nullptr, &dest);
        SDL_RenderPresent(renderer);
        return;
    }

    // The picture. Laid onto black and added, so three channel passes that line
    // up sum back to exactly the image - which is what happens in the middle, by
    // construction, with the split opening up toward the rim.
    if (cfg.crt_aberration > 0.0f) {
        const float split = cfg.crt_aberration;
        draw_tube(renderer, screen.frame, dest, cfg, 1.0f + split, SDL_Color{0xFF, 0, 0, 0xFF});
        draw_tube(renderer, screen.frame, dest, cfg, 1.0f,         SDL_Color{0, 0xFF, 0, 0xFF});
        draw_tube(renderer, screen.frame, dest, cfg, 1.0f - split, SDL_Color{0, 0, 0xFF, 0xFF});
    } else {
        draw_tube(renderer, screen.frame, dest, cfg, 1.0f, SDL_Color{0xFF, 0xFF, 0xFF, 0xFF});
    }

    if (glowing) {
        const Uint8 lift = static_cast<Uint8>(std::lround(cfg.crt_glow * 255.0f));
        draw_tube(renderer, screen.glow, dest, cfg, 1.0f, SDL_Color{0xFF, 0xFF, 0xFF, lift});
    }

    // Last of all, over the lit picture and curved along with it.
    draw_tube(renderer, screen.lines, dest, cfg, 1.0f, SDL_Color{0xFF, 0xFF, 0xFF, 0xFF});

    SDL_RenderPresent(renderer);
}

void render(SDL_Renderer* renderer, const Screen& screen, const World& w,
            const Config& cfg) {
    // All of this lands on the off-screen frame; the tube pass puts it up.
    SDL_SetRenderTarget(renderer, screen.frame);
    set_draw_color(renderer, cfg.background_color);
    SDL_RenderClear(renderer);

    const SDL_Rect square{
        static_cast<int>(std::lround(w.square_x)),
        static_cast<int>(std::lround(w.square_y)),
        static_cast<int>(std::lround(w.square_w)),
        static_cast<int>(std::lround(w.square_h)),
    };
    // Outline only, drawn as nested rects one pixel apart — the square is a
    // frame now, so a diamond under it shows through.
    SDL_Color frame = cfg.square_color;
    frame.a = static_cast<Uint8>(std::lround(static_cast<float>(frame.a) * w.square_alpha));
    set_draw_color(renderer, frame);
    const int thickness = static_cast<int>(std::lround(cfg.square_outline));
    for (int i = 0; i < thickness; ++i) {
        const SDL_Rect edge{square.x + i, square.y + i,
                            square.w - 2 * i, square.h - 2 * i};
        if (edge.w <= 0 || edge.h <= 0) break;
        SDL_RenderDrawRect(renderer, &edge);
    }

    // A wall the square has run out of room against lights up down the whole of
    // that screen edge, so the dead stop reads as the window's and not the
    // box's. It carries the frame's own alpha: park against an edge and the
    // highlight dims with the square, and firms up again as it is driven.
    SDL_Color edge = kEdgeColor;
    edge.a = static_cast<Uint8>(std::lround(static_cast<float>(edge.a) * w.square_alpha));
    set_draw_color(renderer, edge);
    const int bar = static_cast<int>(std::lround(kEdgeWidth));
    if (w.square_x <= kEdgeTouch) {
        const SDL_Rect lit{0, 0, bar, cfg.window_h};
        SDL_RenderFillRect(renderer, &lit);
    }
    if (w.square_x + w.square_w >= static_cast<float>(cfg.window_w) - kEdgeTouch) {
        const SDL_Rect lit{cfg.window_w - bar, 0, bar, cfg.window_h};
        SDL_RenderFillRect(renderer, &lit);
    }
    if (w.square_y <= kEdgeTouch) {
        const SDL_Rect lit{0, 0, cfg.window_w, bar};
        SDL_RenderFillRect(renderer, &lit);
    }
    if (w.square_y + w.square_h >= static_cast<float>(cfg.window_h) - kEdgeTouch) {
        const SDL_Rect lit{0, cfg.window_h - bar, cfg.window_w, bar};
        SDL_RenderFillRect(renderer, &lit);
    }

    // The square's pickup sits with the ball's: over the frame, under the ball.
    if (w.hex.active) {
        set_draw_color(renderer, cfg.hexagon_color);
        fill_hexagon(renderer, w.hex.x, w.hex.y, cfg.hexagon_size, w.hex.spin);
    }

    // Between the two: on top of the square, under the ball that eats it.
    if (w.diamond.active) {
        set_draw_color(renderer, kDiamondColor);
        fill_diamond(renderer, w.diamond.x, w.diamond.y, kDiamondHalfW, kDiamondHalfH);
    }

    set_draw_color(renderer, cfg.circle_color);
    if (w.ball_alive) {
        // Rattle in place while the hit registers, settling as the shake ends.
        float offset_x = 0.0f, offset_y = 0.0f;
        if (w.phase == Phase::Shake) {
            const float decay = 1.0f - w.timer / kShakeTime;
            offset_x = kShakeAmp * decay * std::sin(w.timer * kShakeRate);
            offset_y = kShakeAmp * decay * std::sin(w.timer * kShakeRate * 1.7f) * 0.6f;
        }
        set_draw_color(renderer, ball_color(w, cfg));
        fill_circle(renderer, w.circle_x + offset_x, w.circle_y + offset_y,
                    ball_radius(w, cfg));
        fill_eye(renderer, w, w.circle_x + offset_x, w.circle_y + offset_y,
                 ball_radius(w, cfg));

        // Caught: while the star has the ball, the ball wears a ring in the
        // star's color. Drawn with the ball rather than with the star, so it
        // takes the same rattle and the star itself still lands on top of it.
        if (w.phase == Phase::StarHold) {
            const float radius = ball_radius(w, cfg);
            const float ring = radius * (1.0f + kStarRingGap);
            // Thickness rides the ball like everything else about the ring, with
            // a floor so it is still a ring and not a hairline on a small one.
            const float half = std::max(radius * kStarRingWidth, kStarRingMin) * 0.5f;
            set_draw_color(renderer, cfg.star_color);
            fill_ring(renderer, w.circle_x + offset_x, w.circle_y + offset_y,
                      ring + half, ring - half);
        }
        set_draw_color(renderer, cfg.circle_color); // shards follow, still pink
    }
    for (const Shard& s : w.shards) {
        if (s.alive) fill_circle(renderer, s.x, s.y, s.radius);
    }

    // Hazards ride over the ball, so one crossing it is never hidden behind it.
    for (const Triangle& t : w.triangles) {
        if (!t.active) continue;
        set_draw_color(renderer, hazard_color(t, cfg));

        const float size = hazard_size(t, cfg);
        float xs[2], ys[2];
        const int lobes = hazard_lobes(t, cfg, xs, ys);

        if (t.warn > 0.0f) {
            if (t.moving) {
                // Off screen still: all there is to see is the lane it will take.
                const SDL_Color lane = hazard_color(t, cfg);
                SDL_SetRenderDrawColor(renderer, lane.r, lane.g, lane.b, kStripAlpha);
                fill_path_strip(renderer, cfg, t, size);
            } else {
                // Armed hazards are solid; this one is not yet.
                for (int i = 0; i < lobes; ++i) {
                    draw_triangle(renderer, xs[i], ys[i], size, t.heading);
                }
            }
            continue;
        }

        // Armed and buzzing on the spot, which is the tell.
        float shake_x = 0.0f, shake_y = 0.0f;
        if (!t.moving) {
            shake_x = size * kStillBuzz * std::sin(t.life * kStillBuzzRate);
            shake_y = size * kStillBuzz * std::sin(t.life * kStillBuzzRate * 1.7f) * 0.6f;
        }
        for (int i = 0; i < lobes; ++i) {
            fill_triangle(renderer, xs[i] + shake_x, ys[i] + shake_y, size, t.heading);
        }
    }
    // A burst replaces the whole array, so one tint covers whatever is loose.
    set_draw_color(renderer, w.shard_tint);
    for (const Shard& s : w.tri_shards) {
        if (s.alive) fill_circle(renderer, s.x, s.y, s.radius);
    }

    // Over everything else on the field; only the fade covers it.
    if (w.star.active) {
        set_draw_color(renderer, cfg.star_color);
        fill_star(renderer, w.star.x, w.star.y, cfg.star_size,
                  cfg.star_size * kStarInnerRatio, w.star.spin);
    }

    // HUD: the hits left along the top and the diamonds eaten along the bottom.
    // Drawn last of the field so nothing — square, ball, hazard or star — can
    // cover them; only the fade goes over.
    set_draw_color(renderer, cfg.circle_color); // the dots match the ball
    for (const Dot& d : w.dots) {
        if (!d.gone) fill_circle(renderer, d.x, d.y, kDotRadius);
    }

    // The tally is centered on the window, so it opens outward as it fills.
    const int fits = std::max(cfg.window_w / static_cast<int>(kScoreSpacing) - 1, 1);
    const int shown = std::min(w.eaten, fits);
    const float row_x = static_cast<float>(cfg.window_w) * 0.5f -
                        kScoreSpacing * static_cast<float>(shown - 1) * 0.5f;
    const float row_y = static_cast<float>(cfg.window_h) - kScoreBottom;
    set_draw_color(renderer, kDiamondColor);
    for (int i = 0; i < shown; ++i) {
        fill_diamond(renderer, row_x + kScoreSpacing * static_cast<float>(i), row_y,
                     kDiamondHalfW * kScoreScale, kDiamondHalfH * kScoreScale);
    }

    if (const Uint8 fade = fade_alpha(w); fade > 0) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, fade);
        SDL_RenderFillRect(renderer, nullptr);
    }

    present_screen(renderer, screen, cfg);
}

Config load_and_report() {
    std::string source, error;
    Config cfg = load_config(&source, &error);
    if (!error.empty()) {
        SDL_Log("config error, using defaults: %s", error.c_str());
    } else if (source.empty()) {
        SDL_Log("no config.json found, using defaults");
    } else {
        SDL_Log("loaded config from %s", source.c_str());
    }
    return cfg;
}

} // namespace

int main(int, char**) {
    // Ask Windows for real pixels. SDL declares no DPI awareness unless it is
    // told to, and an unaware process on a scaled display is handed a window
    // measured in logical pixels which Windows then stretches to the physical
    // ones — so every game pixel lands across a fraction over one screen pixel
    // and the thin work smears: the square's outline, the HUD dots, the scanline
    // mask. Read by the video driver, so it has to be set before SDL_Init.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    Config cfg = load_and_report();

    SDL_Window* window = SDL_CreateWindow(
        "Bouncer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cfg.window_w, cfg.window_h, SDL_WINDOW_SHOWN);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND); // for the fade

    // Anything but a match here means the frame is being scaled on its way to
    // the window, which is the first thing worth knowing when it looks soft.
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
    if (out_w != cfg.window_w || out_h != cfg.window_h) {
        SDL_Log("asked for %dx%d, drawing to %dx%d: the picture is being scaled",
                cfg.window_w, cfg.window_h, out_w, out_h);
    }

    // The whole game is laid out against cfg.window_w/h, and it draws into a
    // frame of exactly that size; the tube pass is what fits the frame to the
    // window. So fullscreen scales the picture rather than widening the field,
    // and nothing above present_screen() knows which one it is looking at.
    Screen screen;
    build_screen(screen, renderer, cfg);

    World world = make_world(cfg);

    bool running = true;
    bool fullscreen = false;
    Uint64 previous = SDL_GetPerformanceCounter();
    double accumulator = 0.0;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (event.key.keysym.scancode == SDL_SCANCODE_F) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(
                        window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                } else if (event.key.keysym.scancode == SDL_SCANCODE_R) {
                    // Re-read config.json so tweaking values needs no rebuild.
                    cfg = load_and_report();
                    // A fullscreen window has no size to set; the frame is
                    // what a reload resizes there. Rebuilt either way, since
                    // the whole crt section is baked into it.
                    if (!fullscreen) {
                        SDL_SetWindowSize(window, cfg.window_w, cfg.window_h);
                    }
                    build_screen(screen, renderer, cfg);
                    world = make_world(cfg);
                }
            }
        }

        const Uint64 now = SDL_GetPerformanceCounter();
        double frame = static_cast<double>(now - previous) /
                       static_cast<double>(SDL_GetPerformanceFrequency());
        previous = now;
        accumulator += std::min(frame, kMaxFrame);

        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        while (accumulator >= kFixedStep) {
            step(world, cfg, keys, static_cast<float>(kFixedStep));
            accumulator -= kFixedStep;
        }

        render(renderer, screen, world, cfg);
    }

    free_screen(screen);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
