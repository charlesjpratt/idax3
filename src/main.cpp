#include <SDL.h>

#include "Config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

constexpr double kFixedStep = 1.0 / 120.0; // physics tick
constexpr double kMaxFrame  = 0.25;        // don't spiral after a stall

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
constexpr float kDiamondReach  = 9.0f; // collision radius, between the two halves
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
constexpr float kStarInnerRatio = 0.44f; // waist of the points, per outer radius
constexpr float kStarTimeout    = 6.0f;  // a crawling ball would never arrive

// The eye: a bare pupil on the pink, no white behind it. It points where the
// ball is headed, turning at a fixed rate rather than snapping, which is what
// makes it read as looking.
constexpr SDL_Color kEyePupil{0x24, 0x1E, 0x2B, 0xFF};
// Sizes and travel are all fractions of the ball's radius, so the eye scales
// with it. Rise and travel together keep the pupil in the ball's top two
// thirds: at full reach the capsule grazes the rim going up and stops level
// with the two-thirds line coming down.
constexpr float kPupilSize   = 0.25f; // pupil radius, per ball radius
constexpr float kEyeRise     = 0.333f; // eye's rest point above center
constexpr float kEyeTravel   = 0.45f; // how far the pupil roams from that point
constexpr float kPupilStretch = 0.42f; // capsule half-length, per pupil radius
constexpr float kPupilThin    = 0.72f; // capsule cap radius, per pupil radius
constexpr float kEyeTurnRate = 9.0f;  // radians per second
constexpr float kEyeAimed    = 0.02f; // close enough to call the turn finished
constexpr float kGazeRate    = 4.0f;  // how fast the pupil slides out and back

// Bounds on the eye's turn itself, measured from the end of `star.pause`: the
// floor keeps the hold from counting as "already aimed", the ceiling is there in
// case the turn somehow never lands.
constexpr float kStarTurnMin = 0.15f;
constexpr float kStarTurnMax = 1.9f;

// Red triangles. Their size, color, speed and rate are config; what stays here
// is how hard they are to hit and how they come apart.
constexpr int   kTriangleMax   = 4;     // on screen at once
constexpr float kTriangleHit   = 0.60f; // collision radius, per triangle size
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

struct Triangle {
    float x = 0.0f, y = 0.0f;
    float vx = 0.0f, vy = 0.0f;
    float heading = 0.0f; // radians, the way it points and travels
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
    float circle_x = 0.0f, circle_y = 0.0f;
    float circle_vx = 0.0f, circle_vy = 0.0f;

    Phase phase = Phase::Play;
    float timer = 0.0f; // time spent in the current phase
    float grace = 0.0f; // hit immunity left
    bool  ball_alive = true;
    std::array<Dot, kDotCount> dots{};
    std::array<Shard, kShardCount> shards{};

    Diamond diamond{};
    float diamond_wait = 0.0f; // until the next one appears
    Star star{};
    float star_wait = 0.0f;    // until the next one appears
    std::array<Triangle, kTriangleMax> triangles{};
    std::array<Shard, kTriShardCount> tri_shards{};
    float triangle_wait = 0.0f;
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
    w.star_wait    = rand_range(w, cfg.star_gap_min, cfg.star_gap_max);
    w.triangle_wait = rand_range(w, cfg.triangle_gap_min, cfg.triangle_gap_max);
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
    const float inset_x = kDiamondHalfW + 4.0f;
    const float inset_y = kDiamondHalfH + 4.0f;
    const float max_x = static_cast<float>(cfg.window_w) - inset_x;
    const float max_y = static_cast<float>(cfg.window_h) - inset_y;
    if (max_x <= inset_x || max_y <= inset_y) return; // window too small to hold one

    w.diamond.x      = rand_range(w, inset_x, max_x);
    w.diamond.y      = rand_range(w, inset_y, max_y);
    w.diamond.active = true;
}

// Runs the pickup: count down to the next spawn, then wait for the ball to
// reach the one on the field and hand out the boost.
void update_diamond(World& w, const Config& cfg, float dt) {
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

    w.star.x      = rand_range(w, min_x, max_x);
    w.star.y      = rand_range(w, min_y, max_y);
    w.star.spin   = 0.0f; // every star arrives upright
    w.star.active = true;
    return true;
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

// Sends a triangle in from off screen, aimed dead at the middle of the window,
// so every one of them crosses the center on a different line.
void spawn_triangle(World& w, const Config& cfg) {
    Triangle* slot = nullptr;
    for (Triangle& t : w.triangles) {
        if (!t.active) { slot = &t; break; }
    }
    if (!slot) return; // screen is full; try again on the next timer

    const float center_x = static_cast<float>(cfg.window_w) * 0.5f;
    const float center_y = static_cast<float>(cfg.window_h) * 0.5f;
    const float reach =
        std::sqrt(center_x * center_x + center_y * center_y) + cfg.triangle_size * 2.0f;
    const float angle = rand_range(w, 0.0f, kTwoPi);

    slot->x  = center_x + std::cos(angle) * reach;
    slot->y  = center_y + std::sin(angle) * reach;
    slot->vx = -std::cos(angle) * cfg.triangle_speed;
    slot->vy = -std::sin(angle) * cfg.triangle_speed;
    slot->heading = angle + kPi; // it points the way it travels
    slot->active  = true;
}

// Flies the triangles across, retires the ones that have left, and reports a
// touch on the ball — which costs a dot exactly as a moving wall does. The
// triangle that lands it comes apart on the spot.
bool update_triangles(World& w, const Config& cfg, float dt) {
    update_shards(w.tri_shards, cfg, dt);

    w.triangle_wait -= dt;
    if (w.triangle_wait <= 0.0f) {
        w.triangle_wait = rand_range(w, cfg.triangle_gap_min, cfg.triangle_gap_max);
        spawn_triangle(w, cfg);
    }

    const float center_x = static_cast<float>(cfg.window_w) * 0.5f;
    const float center_y = static_cast<float>(cfg.window_h) * 0.5f;
    const float gone =
        std::sqrt(center_x * center_x + center_y * center_y) + cfg.triangle_size * 3.0f;
    const float reach = ball_collider(w, cfg) + cfg.triangle_size * kTriangleHit;

    bool struck = false;
    for (Triangle& t : w.triangles) {
        if (!t.active) continue;
        t.x += t.vx * dt;
        t.y += t.vy * dt;

        const float ox = t.x - center_x;
        const float oy = t.y - center_y;
        if (ox * ox + oy * oy > gone * gone) { // crossed and left the far side
            t.active = false;
            continue;
        }

        if (!w.ball_alive) continue;
        const float dx = t.x - w.circle_x;
        const float dy = t.y - w.circle_y;
        if (dx * dx + dy * dy > reach * reach) continue;

        fan_shards(w.tri_shards, t.x, t.y, cfg.triangle_size * 0.5f, kTriShardSpeed);
        t.active = false;
        struck = true;
    }
    return struck;
}

// Drives the square from the keyboard and reports whether it actually moved.
// The damage rule keys off that, and the star chases steer the square too.
bool move_square(World& w, const Config& cfg, const Uint8* keys, float dt) {
    float move_x = 0.0f, move_y = 0.0f;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) move_x -= 1.0f;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) move_x += 1.0f;
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) move_y -= 1.0f;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) move_y += 1.0f;

    const bool moving = (move_x != 0.0f || move_y != 0.0f);

    if (move_x != 0.0f && move_y != 0.0f) { // no free speed on the diagonal
        const float inv = 0.70710678f;
        move_x *= inv;
        move_y *= inv;
    }

    w.square_x += move_x * cfg.square_speed * dt;
    w.square_y += move_y * cfg.square_speed * dt;
    w.square_x = std::clamp(w.square_x, 0.0f, static_cast<float>(cfg.window_w) - w.square_w);
    w.square_y = std::clamp(w.square_y, 0.0f, static_cast<float>(cfg.window_h) - w.square_h);
    return moving;
}

// Closes the square in around its own center, so the walls come to the ball
// evenly rather than the box crawling off in one direction. The floor keeps the
// circle able to fit, whatever diamonds have done to it.
void shrink_square(World& w, const Config& cfg, float dt) {
    const float floor_size = std::max(cfg.square_min_size, ball_collider(w, cfg) * 2.0f);
    const float step = cfg.square_shrink_rate * dt;

    const float next_w = std::max(w.square_w - step, floor_size);
    const float next_h = std::max(w.square_h - step, floor_size);
    w.square_x += (w.square_w - next_w) * 0.5f;
    w.square_y += (w.square_h - next_h) * 0.5f;
    w.square_w = next_w;
    w.square_h = next_h;
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
    if (w.grace > 0.0f) w.grace -= dt;
    w.timer += dt;

    switch (w.phase) {
    case Phase::Play: {
        const bool square_moving = move_square(w, cfg, keys, dt);

        // Only open play closes the walls in: a diamond boost holds them, and
        // so does every star phase, none of which come through here.
        if (w.boost <= 0.0f) shrink_square(w, cfg, dt);

        // Let the speed boost lapse before the bounce, so a tick is never
        // integrated at one speed and reflected at another.
        age_boost(w, dt);

        w.circle_x += w.circle_vx * dt;
        w.circle_y += w.circle_vy * dt;

        const bool hit = confine_circle_to_square(w, cfg);
        update_diamond(w, cfg, dt);
        const bool struck = update_triangles(w, cfg, dt);

        // Only a wall that was on the move costs a dot; an idle bounce is free.
        // A triangle costs one however it is met.
        if ((struck || (hit && square_moving)) && w.grace <= 0.0f) {
            w.phase = Phase::Shake;
            w.timer = 0.0f;
            break;
        }

        // A star takes the ball over the moment it appears.
        w.star_wait -= dt;
        if (w.star_wait <= 0.0f) {
            w.star_wait = rand_range(w, cfg.star_gap_min, cfg.star_gap_max);
            if (spawn_star(w, cfg)) {
                w.phase = Phase::StarLook;
                w.timer = 0.0f;
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
        const bool struck = update_triangles(w, cfg, dt);

        if ((struck || ball_inside_square(w, cfg) != was_inside) && w.grace <= 0.0f) {
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
        const bool struck = update_triangles(w, cfg, dt);
        const bool arrived = home_ball(w, cfg, w.star.x, w.star.y,
                                      cfg.circle_speed * cfg.star_seek_speed, dt);

        // Either direction counts: the ball crossing an edge on its way out, or
        // the player driving an edge into it while it is out there. A triangle
        // caught mid-chase costs the same dot.
        if ((struck || ball_inside_square(w, cfg) != was_inside) && w.grace <= 0.0f) {
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
        const bool struck = update_triangles(w, cfg, dt);
        w.star.spin += cfg.star_spin_speed * kTwoPi * dt;

        if ((struck || ball_inside_square(w, cfg) != was_inside) && w.grace <= 0.0f) {
            w.phase = Phase::Shake;
            w.timer = 0.0f;
            break;
        }

        if (w.timer >= cfg.star_hold) {
            w.star.active = false;
            launch_ball(w, cfg);
            w.phase = Phase::Play;
            w.timer = 0.0f;
            w.grace = kHitGrace; // the ball may be well outside the square
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

    set_draw_color(renderer, kEyePupil);
    fill_capsule(renderer, px - half_len, py, px + half_len, py,
                 pupil_r * kPupilThin);
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

// One corner leads, so the triangle points the way it is travelling.
void fill_triangle(SDL_Renderer* renderer, float cx, float cy, float size, float heading) {
    constexpr int kPoints = 3;
    float px[kPoints], py[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const float angle = heading + kTwoPi * static_cast<float>(i) / 3.0f;
        px[i] = cx + std::cos(angle) * size;
        py[i] = cy + std::sin(angle) * size;
    }
    fill_polygon(renderer, px, py, kPoints);
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

void render(SDL_Renderer* renderer, const World& w, const Config& cfg) {
    set_draw_color(renderer, cfg.background_color);
    SDL_RenderClear(renderer);

    // Same pink as the ball, so one config key still drives every circle. Drawn
    // as backdrop, so the square slides over the dots instead of under.
    set_draw_color(renderer, cfg.circle_color);
    for (const Dot& d : w.dots) {
        if (!d.gone) fill_circle(renderer, d.x, d.y, kDotRadius);
    }

    // The tally, centered on the window so it opens outward as it fills. Drawn
    // as backdrop like the dots, so the square passes over it.
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

    const SDL_Rect square{
        static_cast<int>(std::lround(w.square_x)),
        static_cast<int>(std::lround(w.square_y)),
        static_cast<int>(std::lround(w.square_w)),
        static_cast<int>(std::lround(w.square_h)),
    };
    set_draw_color(renderer, cfg.square_color);
    SDL_RenderFillRect(renderer, &square);

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
        fill_circle(renderer, w.circle_x + offset_x, w.circle_y + offset_y,
                    ball_radius(w, cfg));
        fill_eye(renderer, w, w.circle_x + offset_x, w.circle_y + offset_y,
                 ball_radius(w, cfg));
        set_draw_color(renderer, cfg.circle_color); // shards follow, still pink
    }
    for (const Shard& s : w.shards) {
        if (s.alive) fill_circle(renderer, s.x, s.y, s.radius);
    }

    // Hazards ride over the ball, so one crossing it is never hidden behind it.
    set_draw_color(renderer, cfg.triangle_color);
    for (const Triangle& t : w.triangles) {
        if (t.active) fill_triangle(renderer, t.x, t.y, cfg.triangle_size, t.heading);
    }
    for (const Shard& s : w.tri_shards) {
        if (s.alive) fill_circle(renderer, s.x, s.y, s.radius);
    }

    // Over everything else on the field; only the fade covers it.
    if (w.star.active) {
        set_draw_color(renderer, cfg.star_color);
        fill_star(renderer, w.star.x, w.star.y, cfg.star_size,
                  cfg.star_size * kStarInnerRatio, w.star.spin);
    }

    if (const Uint8 fade = fade_alpha(w); fade > 0) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, fade);
        SDL_RenderFillRect(renderer, nullptr);
    }

    SDL_RenderPresent(renderer);
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

    World world = make_world(cfg);

    bool running = true;
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
                } else if (event.key.keysym.scancode == SDL_SCANCODE_R) {
                    // Re-read config.json so tweaking values needs no rebuild.
                    cfg = load_and_report();
                    SDL_SetWindowSize(window, cfg.window_w, cfg.window_h);
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

        render(renderer, world, cfg);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
