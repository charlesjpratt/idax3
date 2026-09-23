#include <SDL.h>

#include "Config.hpp"
#include "font_data.hpp" // the title's face, built in at build time

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// The title's font goes through stb_truetype, which is written as C and lights
// up every warning a strict C++ build has. It is not this file's to fix, so its
// warnings are turned off for the one include and back on after it.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

constexpr double kFixedStep = 1.0 / 120.0; // physics tick
constexpr double kMaxFrame  = 0.25;        // don't spiral after a stall

// The window edge lights up where the square has run out of room against it.
// The clamp in move_square() lands the box exactly on the boundary, so the gap
// counted as a touch only has to survive the rounding into pixels.
constexpr float     kEdgeWidth = 4.0f; // thickness of the lit bar
constexpr float     kEdgeTouch = 0.5f; // gap still counted as against the edge
constexpr SDL_Color kEdgeColor{0xFF, 0xFF, 0xFF, 0xFF};

// The backdrop: overlapping blue circles over the whole window. Laid on a grid
// so coverage can be guaranteed rather than hoped for, then jittered and
// oversized until the grid stops reading as one. `kBackSizeMin` is what makes it
// airtight, and a circle asks more of it than a square did: it has to reach the
// corners of its cell, which is the sqrt(2), from wherever wandering and turning
// have taken it, which is the rest.
constexpr int   kBackMax     = 640;   // tiles the backdrop can hold
constexpr float kBackCell    = 84.0f; // nominal spacing, in pixels
constexpr float kBackJitter  = 0.25f; // of a cell, how far one wanders off center
constexpr float kBackOrbit   = 0.10f; // of a cell, the circle each one walks
constexpr float kBackSpin    = 0.05f; // revolutions a second around that circle
// sqrt(2) + 2 * (jitter + orbit), as a diameter in cells.
constexpr float kBackSizeMin = 2.15f;
constexpr float kBackSizeMax = 3.4f;
constexpr int   kDiscSize    = 256;   // the disc texture, in pixels across
// Every tile is a mix of the deep and one of the two lifts, so the whole field
// stays near the bottom of its range — dark enough that the pink, the
// aquamarine and the red all still read as the lit things on it. A few come out
// purple instead of blue, which is enough to be noticed and not enough to
// become the color of the backdrop.
constexpr SDL_Color kBackDeep{0x04, 0x08, 0x16, 0xFF};
constexpr SDL_Color kBackLift{0x11, 0x20, 0x3E, 0xFF};
constexpr SDL_Color kBackViolet{0x1C, 0x11, 0x34, 0xFF};
constexpr float     kBackVioletShare = 0.16f; // of the tiles, on average

// Three dots centered along the top of the window: the hits the ball has left.
// They sit low enough to leave the charge bar the room above them.
constexpr int   kDotCount   = 3;
constexpr float kDotRadius  = 7.5f;
constexpr float kDotSpacing = 36.0f;   // center to center
constexpr float kDotTop     = 58.0f;   // center's distance from the top edge

// The grab's charge, drawn as a bar over the lives and in the square's own
// color, since it is the square's to spend. Taking hold empties it and it fills
// again only once the frame has let go, so a bar short of full is a frame that
// cannot take hold at all. How wide it is and how long the filling takes are
// both config — `squeeze.bar_width` and `squeeze.recharge`.
constexpr float kBarHeight = 14.0f;
constexpr float kBarTop    = 20.0f; // top edge's distance from the top of the window
constexpr float kBarMaxWidth = 0.92f; // of the window, the widest it may ever grow
constexpr float kBarGrowRate = 9.0f;  // how fast the track runs out to a new length
// The ground the charge is drawn on, so an empty bar is still a bar. Dark
// enough to sit under `kGlowFloor` and so bloom no more than the backdrop does
// — the charge over it is the lit thing, and an empty bar should not glow.
constexpr SDL_Color kBarTrack{0x3C, 0x3C, 0x3C, 0xFF};
// Every fill the bar has done makes the next one longer, so the longest one is
// worth a ceiling: past this the bar has stopped being a wait and become a
// wall, and a steep `squeeze.recharge_growth` would run away from the run.
constexpr float kRechargeCeil = 45.0f; // seconds, the most a fill can ever take
constexpr float kDotPop     = -70.0f;  // upward flick before a dot falls away
constexpr float kDotGravity = 1100.0f;
// A forfeit doesn't take the row all at once: the dots are knocked loose one
// after another, so the lives read as being taken in turn rather than vanishing
// together.
constexpr float kDrainFirst = 0.30f; // beat after the burst before the first goes
constexpr float kDrainGap   = 0.34f; // between one and the next

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
constexpr float kScoreScale   = 0.85f; // of a field diamond
constexpr float kScoreSpacing = 25.0f; // center to center
constexpr float kScoreBottom  = 28.0f; // center's distance from the bottom edge


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
// It spends its last seconds blinking, so its going is something the player is
// told about rather than something they find out by being hit. The blink is the
// drawing only — it covers the ball for the whole of `star.linger` either way,
// the way a thing about to lapse always still works while it says so.
constexpr float kRingBlink  = 1.2f; // seconds of blinking before it goes
constexpr int   kRingBlinks = 4;    // on-and-off pairs fitted into those seconds
constexpr float kStarInnerRatio = 0.44f; // waist of the points, per outer radius
// The chase is given the ground there is to cover at the speed there is to
// cover it, and then some. A flat number cannot do this job: the window size and
// both speeds are config, so the same seconds are generous in one window and
// short of the far corner in another — and a chase cut off part way leaves the
// ball holding a star it never reached.
constexpr float kStarTripSlack  = 1.6f;  // of the straight-line time
constexpr float kStarTripMin    = 2.0f;  // seconds, however small the window is

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

// A hexagon wins back this much of whatever the run has taken: half the gap
// between where a thing has got to and where it started. It settles the square's
// size and the grab's recharge by the same line, which is what makes the hexagon
// the answer to both of the run's ratchets rather than only the one.
constexpr float kHexRecovery = 0.5f;
constexpr float kSpawnPad = 10.0f; // breathing room between anything spawned

// The grab. Holding space walks the frame in until it is wrapped around the
// ball and holds it there — neither gives another pixel after that; what runs
// from then on is the clock. How long it takes to close, how long the ball
// takes it and how long it lasts once it starts to shake are all config.
constexpr float kSqueezeGap   = 3.0f;  // pixels of frame kept clear around it
constexpr float kSqueezeShake = 5.0f;  // rattle amplitude as it goes
constexpr float kSqueezeRate  = 44.0f; // radians a second of that rattle

// Red hazards. Sizes, colors, speeds, rates and unlocks are the two hazard
// sections of config.json; what stays here is how hard they are to hit and how
// they come apart.
constexpr int   kHazardMax     = 8;     // moving and still ones together
// A run that has got going gets leaned on, and goes on being leaned on: the
// third diamond starts a clock, and from there both hazards come at a gap that
// tightens with every second that passes. Only the arming is a tally — what
// closes the gaps is time, so a player who stops eating diamonds no longer
// stops the pressure along with them.
constexpr int   kHazardRampAt    = 3;      // diamonds eaten before the gaps start closing
constexpr float kHazardRampTime  = 180.0f; // seconds to bring them all the way in
constexpr float kHazardRampFloor = 0.35f;  // tightest they ever get, per configured gap
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

// The pad. A stick rests off center, so anything inside the deadzone is nothing
// at all; a trigger rests at zero, so its threshold only has to be past a
// resting twitch.
constexpr float kStickDead   = 0.22f; // of the stick's full reach
constexpr float kTriggerPull = 0.30f; // of a trigger's full travel

// The title screen. It is the face of the opening's wait: the field runs on
// behind it, the ball bouncing in a full-size frame, until the first push
// takes the square — then it fades and the HUD comes up in its place. The name
// sits in the band above the centered square and the prompt in the band below
// it, so neither covers the game it is introducing. What it says and what face
// it is set in are config; where it sits and how big it is are here.
constexpr float kTitleSize   = 170.0f; // pixel height of the name, before fitting
constexpr float kTitleWidth  = 0.82f;  // of the window, the most the name may span
constexpr float kTitleTrack  = 0.05f;  // letter spacing, per pixel height
constexpr float kTitleY      = 0.17f;  // of the window height, the name's center
// The byline, tucked under the name: small, set wide, and in a quiet color so
// the name stays the thing that is lit.
constexpr float kSubtitleSize  = 26.0f;
constexpr float kSubtitleTrack = 0.10f;
constexpr float kSubtitleY     = 0.255f;
constexpr SDL_Color kSubtitleColor{0xDC, 0xDC, 0xDC, 0xFF};
constexpr float kPromptSize  = 32.0f;
constexpr float kPromptTrack = 0.08f;
constexpr float kPromptY     = 0.80f;
constexpr float kPromptPulse = 0.8f;   // breaths a second, so it reads as waiting
constexpr float kPromptFloor = 0.35f;  // how dim a breath lets it go
// The keys, in the margins either side of the centered field — the bands above
// and below are the title's and the middle is the square's, so the sides are
// what a screen already showing the game behind it has left. One block a hand:
// what moves the frame on the left, what closes it on the right. F and R are
// not here, being things done to the game rather than played with.
constexpr float kKeysSize      = 20.0f;
constexpr float kKeysTrack     = 0.06f;
constexpr float kKeysHeadSize  = 27.0f;
constexpr float kKeysHeadTrack = 0.14f;
constexpr float kKeysX         = 0.155f; // of the width, each block's center
constexpr float kKeysY         = 0.46f;  // of the height, the heading's center
constexpr float kKeysGap       = 0.055f; // heading down to the keys under it
constexpr SDL_Color kHintColor{0x9A, 0x9A, 0x9A, 0xFF};
// The version, tucked into the lower right corner. It comes from the build —
// `GAME_VERSION` is CMake's project version — so the screen and the release
// tag can only disagree if someone forgets to bump the one before cutting
// the other.
constexpr float kVersionSize   = 18.0f;
constexpr float kVersionTrack  = 0.06f;
constexpr float kVersionMargin = 22.0f; // from the right and bottom edges to the ink
constexpr SDL_Color kVersionColor{0x6E, 0x6E, 0x6E, 0xFF};
#ifndef GAME_VERSION
#define GAME_VERSION "dev"
#endif
constexpr const char* kVersionText = "v" GAME_VERSION;
constexpr float kTitleFadeRate = 2.5f; // how fast it goes once the run begins
constexpr const char* kPromptText = "MOVE TO START";
constexpr const char* kKeysLeftHead  = "MOVE";
constexpr const char* kKeysLeftText  = "ARROWS OR WASD";
constexpr const char* kKeysRightHead = "GRAB";
constexpr const char* kKeysRightText = "SPACEBAR";

// The high score board, down the title screen's left. A score is the diamonds
// one run ate — the tally the bottom row already draws, so the board counts
// the thing the player was watching anyway rather than inventing a number.
// It is the only thing here that outlives a run, and so the only thing
// written anywhere: SDL's pref path, a per-user folder Windows already keeps
// for it, which leaves the release the one exe with nothing beside it.
// Rank and score are baked as two labels a gutter apart rather than one line
// centered, since a column of center-anchored rows wanders as the digits
// change and the numbers are the whole point of reading it.
constexpr int   kBoardRows      = 5;
constexpr float kBoardSize      = 34.0f;  // a row's digits
constexpr float kBoardTrack     = 0.08f;
constexpr float kBoardHeadSize  = 26.0f;
constexpr float kBoardHeadTrack = 0.18f;
constexpr float kBoardY         = 0.28f;  // of the height, the heading's center
// The heading's drop and the row step are separate numbers so the places can
// be closed up without first place moving off the spot it was laid out on.
constexpr float kBoardHeadGap   = 0.088f; // of the height, heading to first place
constexpr float kBoardStep      = 0.062f; // of the height, one place to the next
constexpr float kBoardGap       = 30.0f;  // between a rank's ink and its group
// A row is the rank, then the group the count-up turns into: rank right
// against the middle, group left against it, so the ranks line up down the
// column and a group always starts where the one that lands has to arrive.
constexpr float kDigitGap       = 0.14f;  // of a digit's height, added to the advance
constexpr float kGroupDiaW      = 0.34f;  // of a digit's height, the diamond's half width
constexpr float kGroupDiaH      = 0.50f;
constexpr float kGroupGap       = 0.30f;  // between diamond, X and number
constexpr float kGroupExSize    = 0.55f;  // of the digits, the X between them
// The ending. The field goes, the screen holds black, and what the run was
// worth comes up on the backdrop alone: a diamond, an X, and a number counting
// to the score. If it places, the three of them shrink onto the row they
// earned while the table comes up under them.
constexpr float kEndBlackTime   = 0.9f;   // black between the field going and the count
constexpr float kTallyFade      = 0.8f;   // the count fading up out of that black
constexpr float kTallyRate      = 16.0f;  // diamonds a second the number counts at
constexpr float kTallyMin       = 0.7f;   // even a short score gets a count worth watching
constexpr float kTallyHold      = 1.0f;   // beat on the finished number before it moves
constexpr float kToBoardTime    = 0.85f;  // the group's trip to its row
constexpr float kToBoardFade    = 0.35f;  // of that trip, the group handing over to the row
constexpr float kTallyY         = 0.47f;  // of the height, the group's center as it counts
constexpr float kTallyScale     = 2.6f;   // how much bigger the count is than a row
// The table waits on the player rather than on a clock: a run's last score is
// worth as long as it takes to look at. `kBoardArm` is the one beat it does
// not listen for, so a key still down from the run — or the one that lost it —
// cannot take the table away before it has been seen.
constexpr float kBoardArm       = 0.40f;
constexpr float kBoardPromptSize  = 24.0f;
constexpr float kBoardPromptTrack = 0.12f;
constexpr float kBoardPromptY     = 0.83f;
constexpr const char* kBoardPromptText = "PRESS ANY KEY";
constexpr SDL_Color kBoardHeadColor{0x9A, 0x9A, 0x9A, 0xFF};
constexpr SDL_Color kBoardColor{0xDC, 0xDC, 0xDC, 0xFF};
constexpr const char* kBoardHeadText = "BEST SCORES";
// Where the board is kept. The org and app names are what make the folder.
constexpr const char* kPrefOrg    = "Joe Dudley";
constexpr const char* kPrefApp    = "IDAIDAIDA";
constexpr const char* kScoresFile = "scores.json";


constexpr float kPi    = 3.1415927f;
constexpr float kTwoPi = 6.2831853f;

enum class Phase { Play, Squeeze, Shake, Burst, EndFade, EndBlack, Tally, ToBoard,
                   Board, FadeOut, Black, FadeIn,
                   StarLook, StarSeek, StarHold };

struct Tile {
    float x = 0.0f, y = 0.0f; // center, at rest
    float size = 0.0f;        // diameter, in pixels
    float orbit = 0.0f;       // radius of the circle it walks, in pixels
    float phase = 0.0f;       // where on that circle it starts
    SDL_Color color{};
};

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
    // The grab. `squeeze` is how far in it has got, 0 open and 1 shut tight on
    // the ball. The size to give back and where the ball sat in the frame are
    // taken once, when the key goes down, so running the one number back to
    // zero undoes the whole of it — the frame the grab interrupted, exactly.
    float squeeze = 0.0f;
    float squeeze_time = 0.0f;            // seconds the player has held it
    float charge = 1.0f;                  // 1 = the frame can take hold, 0 = just did
    float recharge = 1.0f;                // seconds the next fill takes; seeded from config
    float hold_w = 0.0f, hold_h = 0.0f;   // the size to spring back to
    float hold_fx = 0.5f, hold_fy = 0.5f; // where the ball sat in the frame
    float circle_x = 0.0f, circle_y = 0.0f;
    float circle_vx = 0.0f, circle_vy = 0.0f;

    // A world opens by fading up out of black, the same way one rebuilt behind
    // a death does — the reset in FadeOut moves it straight on to Black, so
    // that sequence still holds before it reveals anything.
    Phase phase = Phase::FadeIn;
    float timer = 0.0f; // time spent in the current phase
    float grace = 0.0f; // hit immunity left
    float ring  = 0.0f; // seconds of the star's ring still worn after a release
    bool  flash = false; // is this shake a wall's, and so flashing
    bool  ball_alive = true;
    bool  started = false; // has the player taken hold of the square yet
    // How much of the title screen is still up: 1 over a world nobody has
    // driven yet, eased to 0 once `started` lands. The HUD comes up through
    // the same number, in the title's place.
    float title = 1.0f;
    std::array<Tile, kBackMax> back{};
    int back_count = 0;
    float drift = 0.0f; // seconds the backdrop has been turning
    std::array<Dot, kDotCount> dots{};
    bool  draining = false;   // is the whole row being given up, one at a time
    float drain_wait = 0.0f;  // until the next one is knocked loose
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
    float pressure = 0.0f;     // seconds the run has been leaning on the player
    float boost = 0.0f;        // seconds of extra speed left
    float grow = 1.0f;         // size multiplier, kept until the next reset
    Uint32 rng = 1u;
    // The score a death just finished with, for main to put on the board, and
    // -1 when there is none. It is carried out through the rebuild rather than
    // step() being handed the board, which would put a file in the simulation.
    int last_run = -1;
    // Which row the finished run took, or -1 for a run that did not place.
    // main works it out as it posts the score, and the tally reads it to know
    // whether it has anywhere to go when it is done counting.
    int last_rank = -1;
    float tally = 0.0f; // what the count-up has reached, on its way to `eaten`
    // How long the charge bar is drawn, as a multiple of `squeeze.bar_width`.
    // It chases what `recharge` says it should be rather than being read off it,
    // so a grab that has just been spent runs the track out to its new length
    // instead of the track simply being longer the next time it is looked at.
    float bar_span = 1.0f;
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

// The size the square counts as being: its own, or — while a squeeze has it
// shut on the ball — the one it will spring back to when the key is let go.
// Anything measuring the frame the game is really played in asks for this
// rather than for where the walls happen to be this frame.
float resting_w(const World& w) { return w.squeeze > 0.0f ? w.hold_w : w.square_w; }
float resting_h(const World& w) { return w.squeeze > 0.0f ? w.hold_h : w.square_h; }

// That frame as a box on the field: where the walls will be once the grip lets
// go, which is the size it was taken at laid back around the ball exactly where
// it sat in it. Spawns that have to stay off the square measure against this,
// or a squeeze would open out onto whatever landed in the room it gave up.
SDL_FRect resting_frame(const World& w) {
    if (w.squeeze <= 0.0f) {
        return SDL_FRect{w.square_x, w.square_y, w.square_w, w.square_h};
    }
    return SDL_FRect{w.circle_x - w.hold_w * w.hold_fx,
                     w.circle_y - w.hold_h * w.hold_fy, w.hold_w, w.hold_h};
}

// Diamonds swell the ball by `circle_growth_per_diamond` for the rest of the
// run, so every pass that cares about its size — bouncing, drawing, bursting —
// has to ask for the radius
// rather than assume it. Growth stacks and never lapses, so it is capped at a
// ball the square can still hold: past that the confine pass would have nowhere
// left to put it. The cap measures the resting frame rather than the current
// one, or a squeeze closing the walls in would take the ball down with them:
// the grip holds the ball, it never presses it.
float ball_radius(const World& w, const Config& cfg) {
    const float ceiling = kGrowCeil * std::min(resting_w(w), resting_h(w));
    return std::min(cfg.circle_diameter * 0.5f * w.grow, ceiling);
}

// What the game actually collides on: walls, pickups and the fit test all use
// this, while everything drawn uses ball_radius(). `circle.collider_scale` is
// what sets the two apart — under 1.0 the pink sinks into a wall on a bounce.
float ball_collider(const World& w, const Config& cfg) {
    return ball_radius(w, cfg) * cfg.circle_collider_scale;
}

// Is the ball wearing the star's ring? While the star has it, and for
// `star.linger` after it is let go — the ring is a loan that outlasts the star,
// so a ball dropped back into a field of hazards is not dropped into it bare.
bool ball_is_ringed(const World& w) {
    return w.phase == Phase::StarHold || w.ring > 0.0f;
}

// Is the ring drawn this frame? Solid for all but its last `kRingBlink`
// seconds, then blinking them away — and never blinking while the star still
// has the ball, which has no clock running against it. `World::ring` counts
// down, which is the whole of what a blink needs. It ends on a dark beat, so
// the ring is already gone from the eye by the time it is gone from the world.
bool ring_is_shown(const World& w) {
    if (!ball_is_ringed(w)) return false;
    if (w.phase == Phase::StarHold || w.ring > kRingBlink) return true;
    // The window is cut into an even number of equal slots rather than sampled
    // against a free-running rate, so the blink opens on a whole beat and closes
    // on a dark one however the two constants are set — a rate that did not
    // divide the window would start with a flicker and could end lit.
    const float slot = (kRingBlink - w.ring) / kRingBlink *
                       static_cast<float>(kRingBlinks * 2);
    return (static_cast<int>(slot) & 1) == 0;
}

// The ring the star puts around the ball while it has it. Both radii are grown
// off the ball, so the whole thing scales with every diamond, and the thickness
// has a floor so it stays a ring rather than a hairline on a small one.
//
// Drawing and the hazard test both come through here, for the same reason
// hazard_lobes() exists: what a crossing breaks on has to be the thing you can
// see it break on.
void star_ring(const World& w, const Config& cfg, float* outer, float* inner) {
    const float radius = ball_radius(w, cfg);
    const float mid  = radius * (1.0f + kStarRingGap);
    const float half = std::max(radius * kStarRingWidth, kStarRingMin) * 0.5f;
    *outer = mid + half;
    *inner = mid - half;
}

// A straight lerp between two colors, alpha and all.
SDL_Color blend_color(SDL_Color from, SDL_Color to, float t) {
    const auto channel = [t](Uint8 a, Uint8 b) {
        return static_cast<Uint8>(std::lround(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t));
    };
    return SDL_Color{channel(from.r, to.r), channel(from.g, to.g),
                     channel(from.b, to.b), channel(from.a, to.a)};
}

// Fills the window with overlapping squares. Built once per world rather than
// per frame, so the pattern holds still while the game runs over it, and comes
// back different after a reset or a reload.
void build_backdrop(World& w, const Config& cfg) {
    // Coarsen the grid until it fits the pool. A window big enough to need this
    // gets bigger tiles rather than a backdrop with holes in it.
    float cell = kBackCell;
    int cols = 1, rows = 1;
    for (int attempt = 0; attempt < 16; ++attempt) {
        cols = std::max(static_cast<int>(std::ceil(cfg.window_w / cell)), 1);
        rows = std::max(static_cast<int>(std::ceil(cfg.window_h / cell)), 1);
        if (cols * rows <= kBackMax) break;
        cell *= 1.3f;
    }

    w.back_count = 0;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const float size = cell * rand_range(w, kBackSizeMin, kBackSizeMax);
            const float mid_x = (static_cast<float>(col) + 0.5f) * cell +
                                rand_range(w, -kBackJitter, kBackJitter) * cell;
            const float mid_y = (static_cast<float>(row) + 0.5f) * cell +
                                rand_range(w, -kBackJitter, kBackJitter) * cell;

            // Which way this one leans, and then how far. Both ends are dark,
            // so the mix can run the whole way without lifting the field.
            const bool violet = rand_range(w, 0.0f, 1.0f) < kBackVioletShare;
            const float mix = rand_range(w, 0.0f, 1.0f);

            Tile& t = w.back[static_cast<size_t>(w.back_count++)];
            t.x = mid_x;
            t.y = mid_y;
            t.size = size;
            t.orbit = cell * kBackOrbit;
            // Every tile turns at the same rate; only where each one starts
            // differs, which is what keeps the field from moving as one sheet.
            t.phase = rand_range(w, 0.0f, kTwoPi);
            t.color = blend_color(kBackDeep, violet ? kBackViolet : kBackLift, mix);
        }
    }

    // Laid down in the order they were built, every circle would sit over the
    // one up and left of it and the grid would come back as a shingle — one
    // light source, one direction, the whole field leaning the same way. The
    // shuffle is only over the draw order, so nothing about the coverage
    // changes; it is which of two overlapping circles is on top that stops
    // being a function of where they are.
    for (int i = w.back_count - 1; i > 0; --i) {
        const int j = static_cast<int>(next_rand(w) % static_cast<Uint32>(i + 1));
        std::swap(w.back[static_cast<size_t>(i)], w.back[static_cast<size_t>(j)]);
    }
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

    w.recharge = cfg.squeeze_recharge; // the first fill costs what the file says

    w.rng = SDL_GetTicks() * 2654435761u + 1u; // don't replay the same spawns
    w.diamond_wait = rand_range(w, kDiamondGapMin, kDiamondGapMax);
    w.hex_wait     = rand_range(w, cfg.hexagon_gap_min, cfg.hexagon_gap_max);
    w.star_wait    = rand_range(w, cfg.star_gap_min, cfg.star_gap_max);
    w.triangle_wait = rand_range(w, cfg.moving_hazard_gap_min, cfg.moving_hazard_gap_max);
    w.still_wait    = rand_range(w, cfg.still_hazard_gap_min, cfg.still_hazard_gap_max);
    w.shard_tint    = cfg.moving_hazard_color;
    build_backdrop(w, cfg);
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
    // A forfeit empties the row a dot at a time from here. It runs wherever the
    // falling does — which is everywhere — so the row goes on emptying itself
    // through the burst and the fade, with nothing else left running.
    if (w.draining) {
        w.drain_wait -= dt;
        if (w.drain_wait <= 0.0f) {
            drop_next_dot(w);
            w.drain_wait = kDrainGap;
            if (dots_left(w) == 0) w.draining = false;
        }
    }

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
// reach the one on the field and hand out the boost. Reports the moment one is
// taken, which is what a squeeze keys its own reckoning off.
bool update_diamond(World& w, const Config& cfg, float dt) {
    // The opening is just the ball bouncing: nothing spawns until the player
    // first drives the square. The timer holds rather than drains while it
    // waits — like the hazard and star gates — so the first diamond arrives a
    // full gap after that push, not the instant it lands. Nothing can be on
    // the field to collect yet, so there is nothing else to run down here.
    if (!w.started) return false;

    if (!w.diamond.active) {
        w.diamond_wait -= dt;
        if (w.diamond_wait <= 0.0f) {
            spawn_diamond(w, cfg);
            w.diamond_wait = rand_range(w, kDiamondGapMin, kDiamondGapMax);
        }
        return false;
    }

    const float dx = w.circle_x - w.diamond.x;
    const float dy = w.circle_y - w.diamond.y;
    const float reach = ball_collider(w, cfg) + kDiamondReach;
    if (dx * dx + dy * dy > reach * reach) return false;

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
    return true;
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
    // Judged against the frame the square springs back to, not a grip closed
    // around the ball: one placed inside that would be collected for nothing
    // the moment the walls opened out again.
    const SDL_FRect frame = resting_frame(w);
    if (x > frame.x - margin && x < frame.x + frame.w + margin &&
        y > frame.y - margin && y < frame.y + frame.h + margin) {
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
    // The ground won back is measured off the resting frame, so one taken while
    // a squeeze has the walls shut counts the ground the shrink took rather
    // than the grip — and since grow_square() doesn't run during a squeeze, it
    // is paid out once the frame is open again.
    w.grow_to_w = resting_w(w) + (cfg.square_w - resting_w(w)) * kHexRecovery;
    w.grow_to_h = resting_h(w) + (cfg.square_h - resting_h(w)) * kHexRecovery;

    // The grab is won back on exactly the same terms as the ground, and by the
    // same line: half the gap between what a fill costs now and what the first
    // one cost. The run has two ratchets on it — the walls closing and the bar
    // getting dearer — and the hexagon is the one answer to both.
    w.recharge += (cfg.squeeze_recharge - w.recharge) * kHexRecovery;

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

        // Not in the square, nor close enough to overlap its walls — and the
        // square here is the one it springs back to, so a squeeze does not open
        // out onto a triangle planted in the room it gave up.
        const SDL_FRect frame = resting_frame(w);
        if (x > frame.x - size && x < frame.x + frame.w + size &&
            y > frame.y - size && y < frame.y + frame.h + size) {
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
// How long until the next hazard of a kind. The draw is scaled down by however
// long the run has been leaning on the player — `World::pressure`, which starts
// running at the third diamond and does not stop — so both kinds come oftener
// and oftener from there, down to `kHazardRampFloor` of what the config asks.
// It is applied where the wait is picked rather than to the config itself, so
// the file keeps meaning what it says and an R reload still reports its own
// numbers; and the clock is world state, which a reset clears, so every life
// starts off the pressure again and earns its way back. At zero pressure the
// scale is exactly 1, so this is the ordinary gap until the ramp arms itself.
float hazard_gap(World& w, float low, float high) {
    const float gap = rand_range(w, low, high);
    const float lean = std::min(w.pressure / kHazardRampTime, 1.0f);
    return gap * (1.0f - (1.0f - kHazardRampFloor) * lean);
}

bool update_triangles(World& w, const Config& cfg, float dt) {
    update_shards(w.tri_shards, cfg, dt);

    // The third diamond starts the run leaning, and this is the clock it leans
    // by. It is advanced here rather than in step() so it runs in exactly the
    // phases the hazards themselves do — a shake, a burst or a fade is not time
    // the run gets to hold against the player.
    if (w.eaten >= kHazardRampAt) w.pressure += dt;

    // Both hazards are earned: neither appears until the tally along the bottom
    // has reached its own threshold, and each timer holds rather than running
    // down behind the scenes, so the first one comes a full interval after the
    // qualifying diamond rather than the instant it is eaten.
    if (w.eaten >= cfg.moving_hazard_unlock) {
        w.triangle_wait -= dt;
        if (w.triangle_wait <= 0.0f) {
            w.triangle_wait =
                hazard_gap(w, cfg.moving_hazard_gap_min, cfg.moving_hazard_gap_max);
            spawn_triangle(w, cfg);
        }
    }

    if (w.eaten >= cfg.still_hazard_unlock) {
        w.still_wait -= dt;
        if (w.still_wait <= 0.0f) {
            w.still_wait =
                hazard_gap(w, cfg.still_hazard_gap_min, cfg.still_hazard_gap_max);
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

        // While the ring is up, a hazard meets the ring and not the ball. It
        // still comes apart, but on the yellow, and the pink inside is
        // untouched — which it has to be while the star has it, a ball parked
        // on a star having no say in where it is, and which is the whole of
        // what the ring is worth in the seconds after. Either kind breaks on
        // it: the ring is a property of the ball, and nothing about it knows
        // or cares which sort of red it just met.
        //
        // The guard is the ring's whole outer radius rather than the band
        // alone, so nothing can step over it between one tick and the next and
        // find the pink.
        const bool guarded = ball_is_ringed(w);
        float guard = ball_collider(w, cfg);
        if (guarded) {
            float outer = 0.0f, inner = 0.0f;
            star_ring(w, cfg, &outer, &inner);
            // Never tighter than the ball it is covering: a high
            // circle.collider_scale can put the pink out past its own ring, and
            // a guard inside that would burst the crossing later than the ball
            // would have felt it.
            guard = std::max(outer, guard);
        }

        // Bigger triangles reach further, so the test is per hazard.
        const float reach = guard + hazard_size(t, cfg) * kTriangleHit;
        float xs[2], ys[2];
        const int lobes = hazard_lobes(t, cfg, xs, ys);
        for (int i = 0; i < lobes; ++i) {
            const float dx = xs[i] - w.circle_x;
            const float dy = ys[i] - w.circle_y;
            if (dx * dx + dy * dy > reach * reach) continue;

            fan_shards(w.tri_shards, t.x, t.y, hazard_size(t, cfg) * 0.5f, kTriShardSpeed);
            w.shard_tint = hazard_color(t, cfg);
            t.active = false;
            if (!guarded) struck = true; // the ring took it instead
            break;
        }
    }
    return struck;
}

// What the player is asking for this tick, gathered from the keyboard and the
// pad together and settled into one thing, so nothing downstream has to know
// which of them it came from. `push` is a direction and a reach in one, never
// longer than 1 — the keys give its corners, the stick everything in between.
struct Input {
    float push_x = 0.0f, push_y = 0.0f;
    bool  grip = false;
    // A key or pad button *going down* this frame. It is the one thing here
    // read from events rather than polled: everything else wants a held key,
    // and this wants the press — a direction still held from the run that just
    // ended would otherwise dismiss the table the instant it appeared.
    bool  confirm = false;
};

// An axis comes back as a signed 16-bit reach; this is that as a fraction.
float axis_unit(SDL_GameController* pad, SDL_GameControllerAxis axis) {
    return static_cast<float>(SDL_GameControllerGetAxis(pad, axis)) / 32767.0f;
}

// Reads both at once and adds them, so a pad never has to be chosen over the
// keyboard — either can drive, and holding both is still just a direction.
Input read_input(SDL_GameController* pad) {
    Input in;

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) in.push_x -= 1.0f;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) in.push_x += 1.0f;
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) in.push_y -= 1.0f;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) in.push_y += 1.0f;
    in.grip = keys[SDL_SCANCODE_SPACE] != 0;

    if (pad) {
        // The stick is read as a vector and cut as one, so the deadzone is a
        // circle around the middle rather than a cross through it: cut per axis
        // and a stick held near a diagonal loses whichever axis is closer to
        // center, which bends the direction away from the one being asked for.
        const float sx = axis_unit(pad, SDL_CONTROLLER_AXIS_LEFTX);
        const float sy = axis_unit(pad, SDL_CONTROLLER_AXIS_LEFTY);
        const float reach = std::sqrt(sx * sx + sy * sy);
        if (reach > kStickDead) {
            // What is left of the range is stretched back over a full 0..1, or
            // the square would jump to a fifth of its push the moment the stick
            // cleared the deadzone and there would be no gentle push at all.
            const float live =
                std::min((reach - kStickDead) / (1.0f - kStickDead), 1.0f);
            in.push_x += sx / reach * live;
            in.push_y += sy / reach * live;
        }

        // Either trigger, or any of the four face buttons: the grab is the one
        // thing the pad does besides steer, so it is on everything that falls
        // under a thumb or a finger.
        if (axis_unit(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > kTriggerPull ||
            axis_unit(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerPull ||
            SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) ||
            SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) ||
            SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) ||
            SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) {
            in.grip = true;
        }
    }

    // Never longer than 1. This is the same cap the keyboard's diagonal always
    // got — (1, 1) comes back out as the old 0.707 apiece — now doing for the
    // stick and for the two of them held together as well.
    const float reach = std::sqrt(in.push_x * in.push_x + in.push_y * in.push_y);
    if (reach > 1.0f) {
        in.push_x /= reach;
        in.push_y /= reach;
    }
    return in;
}

// Drives the square from whatever the player is on and reports whether they
// were pushing it this tick — not whether it happens to be in motion. The
// damage rule keys off that, so a wall still coasting after the key is let go
// is free, the way an untouched wall always has been.
//
// The push sets a direction to accelerate along rather than a position; letting
// go hands the square to friction, which at the configured rate scrubs off a
// full turn of speed in a fraction of a second.
bool move_square(World& w, const Config& cfg, const Input& in, float dt) {
    const float reach = std::sqrt(in.push_x * in.push_x + in.push_y * in.push_y);
    const bool pushing = reach > 0.0f;

    // A grab handles differently from open driving. The frame is carrying the
    // ball and being asked to put it somewhere, so it answers harder, settles
    // harder and tops out lower — the three read together as precision. The
    // momentum is scaled rather than taken away: it is still a weight being
    // moved, and a grip that snapped to a stop would read as a cursor.
    const bool grip = w.phase == Phase::Squeeze;
    const float accel = cfg.square_acceleration * (grip ? cfg.grip_acceleration : 1.0f);
    const float drag  = cfg.square_friction * (grip ? cfg.grip_friction : 1.0f);
    const float top   = cfg.square_speed * (grip ? cfg.grip_speed : 1.0f);

    if (pushing) {
        w.square_vx += in.push_x * accel * dt;
        w.square_vy += in.push_y * accel * dt;

        // Cap the vector, not each axis, or a diagonal would outrun a straight
        // line. How far the stick is over is a top speed as well as a push, so
        // a stick eased halfway over tops out halfway — the keyboard is always
        // at full reach, so for it this is the cap it always had.
        //
        // Coming *down* to a lower cap is friction's work rather than a snap.
        // At the keyboard's cap the overshoot is a fraction of what friction
        // takes in a tick, so that path lands on exactly the old number; it is
        // easing a stick off that this is for, which should read like letting
        // go rather than like hitting something.
        const float cap = top * reach;
        const float speed =
            std::sqrt(w.square_vx * w.square_vx + w.square_vy * w.square_vy);
        if (speed > cap && speed > 0.0f) {
            // Never above the configured top speed whatever happens, since a
            // friction set below the acceleration would otherwise let a tick
            // gain more than the next one gives back and the cap would leak.
            const float shed = drag * dt;
            const float target = std::min(std::max(speed - shed, cap), top);
            const float trim = target / speed;
            w.square_vx *= trim;
            w.square_vy *= trim;
        }
    } else {
        // Friction takes a fixed bite per second and never overshoots into
        // reverse, so the square coasts to a stop rather than rebounding.
        const float speed =
            std::sqrt(w.square_vx * w.square_vx + w.square_vy * w.square_vy);
        const float shed = drag * dt;
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

// Puts the frame where a squeeze `t` of the way in leaves it: the size runs
// from what it was held at down to a grip on the ball, and the ball's place
// inside it runs to dead center. It is anchored on the ball rather than on the
// box's own center — the walls come to the ball, which is what makes it read as
// a grab and not as the ball being drawn to the middle — which is why this is
// the one resize that doesn't go through resize_square(). At `t` of 0 it
// restores exactly the frame the grab interrupted.
void apply_squeeze(World& w, const Config& cfg, float t) {
    // Where the walls stop: just clear of the ball, and there they stay — the
    // ball keeps its size in here, so the grip has nothing to follow down.
    // Never wider than what was held, so a grip on a small ball in an
    // already-tight frame can't push the walls back out.
    const float grip = (ball_radius(w, cfg) + kSqueezeGap) * 2.0f;
    const float next_w = w.hold_w + (std::min(grip, w.hold_w) - w.hold_w) * t;
    const float next_h = w.hold_h + (std::min(grip, w.hold_h) - w.hold_h) * t;
    const float frac_x = w.hold_fx + (0.5f - w.hold_fx) * t;
    const float frac_y = w.hold_fy + (0.5f - w.hold_fy) * t;

    w.square_w = next_w;
    w.square_h = next_h;

    // The ball keeps its place on the field and the frame is laid around it.
    // Where the window won't let the frame go, the ball is carried the rest of
    // the way with it, so the two stay locked however hard it is driven at a
    // side — the frame has hold of it, and a frame stopped dead takes the ball
    // it is holding with it.
    const float want_x = w.circle_x - next_w * frac_x;
    const float want_y = w.circle_y - next_h * frac_y;
    const float put_x = std::clamp(want_x, 0.0f,
                                   std::max(static_cast<float>(cfg.window_w) - next_w, 0.0f));
    const float put_y = std::clamp(want_y, 0.0f,
                                   std::max(static_cast<float>(cfg.window_h) - next_h, 0.0f));
    w.circle_x += put_x - want_x;
    w.circle_y += put_y - want_y;
    w.square_x = put_x;
    w.square_y = put_y;
}

// How near the grip is to taking the ball: 0 until it has been held longer than
// it can take, then up to 1 over the `squeeze.crush` seconds it spends shaking.
// Nothing about the size of anything rides on this — it sets how hard the ball
// rattles, and that is the whole of the warning. Scaled by how closed the frame
// is, since the walls are what it is shaking against, so letting go quiets it
// as they open.
float squeeze_strain(const World& w, const Config& cfg) {
    const float fuse = std::clamp((w.squeeze_time - cfg.squeeze_warn) / cfg.squeeze_crush,
                                  0.0f, 1.0f);
    return fuse * w.squeeze;
}

// Takes hold. The size to give back and where the ball sits in the frame are
// recorded now and nothing else is disturbed, so everything the squeeze goes on
// to do is undone by running it back to zero. `fuse` is how much of the ball's
// patience is already spent: nothing on an ordinary grab, all of it on a ball
// snatched out of a chase, which shakes from the moment the walls reach it.
//
// The ball need not be inside the frame at all. `hold_fx`/`hold_fy` are just
// where it sits in the frame's span, outside `[0, 1]` as readily as in, so a
// grab made while the ball is out at a star has the frame leave its ground and
// close on the ball where it is — and hands back exactly that ground on the way
// out, since the same two numbers run the frame both ways.
void begin_squeeze(World& w, float fuse) {
    w.hold_w  = w.square_w;
    w.hold_h  = w.square_h;
    w.hold_fx = (w.square_w > 0.0f) ? (w.circle_x - w.square_x) / w.square_w : 0.5f;
    w.hold_fy = (w.square_h > 0.0f) ? (w.circle_y - w.square_y) / w.square_h : 0.5f;
    w.squeeze = 0.0f;
    w.squeeze_time = fuse;
    w.charge = 0.0f; // spent, and no second grab until it has filled again
}

// Lets go of it, wherever it had got to. Winding back out is the ordinary way
// this ends; this is the one way out for the ball, which springs the frame open
// around it in a single frame — it is about to be rattled about anyway.
void release_squeeze(World& w, const Config& cfg) {
    apply_squeeze(w, cfg, 0.0f);
    w.squeeze = 0.0f;
    w.squeeze_time = 0.0f;
    // Letting go is what makes the next fill dearer, and the track lengthens
    // here with it — at the start of the wait rather than at the end. Growing
    // it when a fill *completed* meant the bar reached its end and then jumped
    // wider in the same instant, which read as the bar undoing itself; this way
    // the grab is paid for at the moment it is spent and the longer track is
    // there to be filled.
    w.recharge = std::min(w.recharge * cfg.squeeze_recharge_growth, kRechargeCeil);
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
void step(World& w, const Config& cfg, const Input& in, float dt) {
    w.drift += dt; // the backdrop turns through everything, shakes and fades too
    update_dots(w, cfg, dt);
    const float aim_error = update_look(w, cfg, dt);

    // A frame that isn't going anywhere fades back, so the eye is drawn to the
    // one that is. Only the phases that actually drive it count as moving —
    // during a shake the velocity is merely stale. It stays solid through a
    // collision, though: the wall that landed the hit holds its weight until
    // the ball has finished reacting to it — and through a squeeze, where a
    // frame with the ball in its teeth is doing the most work it ever does.
    const bool drivable = w.phase == Phase::Play || w.phase == Phase::Squeeze ||
                          w.phase == Phase::StarLook ||
                          w.phase == Phase::StarSeek || w.phase == Phase::StarHold;
    const bool solid = w.phase == Phase::Shake || w.phase == Phase::Burst ||
                       w.phase == Phase::Squeeze;
    const bool driven = drivable && (w.square_vx != 0.0f || w.square_vy != 0.0f);
    const float alpha_target = (solid || driven) ? 1.0f : cfg.square_idle_alpha;
    const float alpha_step = kSquareFadeRate * dt;
    w.square_alpha += std::clamp(alpha_target - w.square_alpha, -alpha_step, alpha_step);

    // The title screen goes the moment the run begins — eased, so the first
    // tap reads as taking hold of the square rather than as switching a screen
    // off. `started` never comes back down within a world, so this only ever
    // runs the one way.
    const float title_step = kTitleFadeRate * dt;
    w.title += std::clamp((w.started ? 0.0f : 1.0f) - w.title, -title_step, title_step);
    if (w.grace > 0.0f) w.grace -= dt;
    if (w.ring  > 0.0f) w.ring  -= dt;

    // The grab's charge fills back only while the frame is not using it, so a
    // long hold is not also a free recharge — the wait is between one grab and
    // the next, not something a held key can run down behind the scenes. Every
    // fill that lands makes the next one dearer, so `World::recharge` is carried
    // as the duration itself rather than worked out from a tally: a hexagon
    // settles it back part of the way, and half of a gap is not a whole number
    // of fills.
    if (w.phase != Phase::Squeeze && w.charge < 1.0f) {
        w.charge = std::min(w.charge + dt / std::max(w.recharge, 0.0001f), 1.0f);
    }

    // The track runs out to whatever the wait now costs — quickly, since it is
    // the announcement of the price rather than the paying of it, and it happens
    // with the charge at zero so there is no filled part to stretch with it. It
    // runs *in* by the same rule when a hexagon settles the wait back down.
    const float span_to = cfg.squeeze_recharge > 0.0f
                              ? w.recharge / cfg.squeeze_recharge
                              : 1.0f;
    w.bar_span += (span_to - w.bar_span) * std::min(1.0f, kBarGrowRate * dt);

    w.timer += dt;

    switch (w.phase) {
    case Phase::Play: {
        const bool square_moving = move_square(w, cfg, in, dt);
        if (square_moving) w.started = true; // the run proper begins here

        // Space takes hold of the ball. Like everything else, it waits on the
        // player having taken the square first, so the opening is still just a
        // ball bouncing in a full-size frame — and on the charge being full,
        // which is the bar along the top of the window.
        if (w.started && w.charge >= 1.0f && in.grip) {
            begin_squeeze(w, 0.0f);
            w.phase = Phase::Squeeze;
            w.timer = 0.0f;
            break;
        }

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

    case Phase::Squeeze: {
        // The frame has the ball. It closes while the key is held and opens
        // back out when it is let go, and the ball goes wherever the frame goes
        // for as long as it lasts: it doesn't move under its own steam, and
        // nothing confines it, because there is no gap left to confine it in.
        // Its velocity is untouched throughout, so letting go puts it back on
        // the heading it was taken off.
        const bool gripping = in.grip;

        const float was_x = w.square_x;
        const float was_y = w.square_y;
        move_square(w, cfg, in, dt);
        w.circle_x += w.square_x - was_x; // the ball rides the frame
        w.circle_y += w.square_y - was_y;

        // Wind the grip on or off before anything measures the ball, so the
        // whole tick sees one size for it.
        const float travel = dt / cfg.squeeze_close;
        w.squeeze = std::clamp(w.squeeze + (gripping ? travel : -travel), 0.0f, 1.0f);
        apply_squeeze(w, cfg, w.squeeze);

        age_boost(w, dt);
        const bool ate = update_diamond(w, cfg, dt);
        update_hexagon(w, cfg, dt);
        const bool struck = update_triangles(w, cfg, dt);

        // The clock runs with the grip and unwinds with it, so letting go part
        // way buys back exactly the time it costs to take hold again. A diamond
        // taken in the grip is what the grab is worth and what it costs at once:
        // it pays out as it always does, and the ball starts to give on the spot
        // rather than after the wait.
        w.squeeze_time = std::max(w.squeeze_time + (gripping ? dt : -dt), 0.0f);
        if (gripping && ate) w.squeeze_time = std::max(w.squeeze_time, cfg.squeeze_warn);

        // Held to the end is not a hit but the run, and it goes straight to the
        // burst — there is no rattle to play, since the ball has been shaking
        // for `squeeze.crush` already. The frame keeps its grip through it: it
        // is what did this, and letting go at the last moment is exactly what
        // the player did not do, so the ball goes off inside a frame still shut
        // on it and only the reset behind the fade opens it. The row is not
        // taken all at once either — `World::draining` knocks the dots loose one
        // after another from here, after the ball itself has gone.
        if (gripping && w.squeeze_time >= cfg.squeeze_warn + cfg.squeeze_crush) {
            burst_ball(w, cfg);
            w.flash = false;
            w.draining = true;
            w.drain_wait = kDrainFirst;
            w.phase = Phase::Burst;
            w.timer = 0.0f;
            break;
        }

        // A hazard still reaches the ball in here, and a ball held still is in
        // no position to dodge one. That one is the ordinary hit and costs the
        // one dot it always costs, and the frame lets go so the ball has room
        // to react to it.
        if (struck) {
            release_squeeze(w, cfg);
            w.flash = false; // not a wall's doing, so no flash
            w.phase = Phase::Shake;
            w.timer = 0.0f;
            break;
        }

        // Let go and wound all the way back out: the frame is the one the grab
        // interrupted, down to where the ball sits in it — inside or out — and
        // whatever it was taken from carries on. A star still on the field means
        // it was a chase, which is the same rule a shake recovers by; and since
        // the ball is handed back exactly where the frame found it, the chase
        // does not count a crossing for having been interrupted.
        if (!gripping && w.squeeze <= 0.0f) {
            release_squeeze(w, cfg);
            w.phase = w.star.active ? Phase::StarSeek : Phase::Play;
            w.timer = 0.0f;
        }
        break;
    }

    case Phase::StarLook: {
        // Dead stop while the eye comes around. The square is still the
        // player's, so the same edge-crossing rule as the chase applies.
        const bool was_inside = ball_inside_square(w, cfg);

        move_square(w, cfg, in, dt);
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
        // The frame can take the ball out of a chase as well, and it does not
        // have to catch up with it first — the grab closes on the ball wherever
        // it has got to. A ball already out of the square is already in trouble,
        // though, so this one starts at the end of its patience: it shakes from
        // the moment the walls reach it and there is only `squeeze.crush` of it.
        if (w.charge >= 1.0f && in.grip) {
            begin_squeeze(w, cfg.squeeze_warn);
            w.phase = Phase::Squeeze;
            w.timer = 0.0f;
            break;
        }

        // Out of the square and straight to the star. Nothing confines the ball
        // here, so the wall doesn't stop it — but crossing that wall still costs
        // a dot, the same as a moving wall catching it in Play.
        const bool was_inside = ball_inside_square(w, cfg);

        move_square(w, cfg, in, dt);
        age_boost(w, dt);
        update_diamond(w, cfg, dt);
        update_hexagon(w, cfg, dt);
        grow_square(w, cfg, dt);
        const bool struck = update_triangles(w, cfg, dt);
        const float chase = cfg.circle_speed * cfg.star_seek_speed;
        const bool arrived = home_ball(w, cfg, w.star.x, w.star.y, chase, dt);

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

        // A straight line to the star is never longer than the window's own
        // diagonal, so that is the trip to allow for; the slack covers the tick
        // the last step lands on. At `circle.speed` of 0 there is no arriving at
        // all, which is what the floor is for.
        const float across = std::sqrt(
            static_cast<float>(cfg.window_w) * static_cast<float>(cfg.window_w) +
            static_cast<float>(cfg.window_h) * static_cast<float>(cfg.window_h));
        const float allowance = (chase > 0.0f)
            ? std::max(across / chase * kStarTripSlack, kStarTripMin)
            : kStarTripMin;

        if (arrived || w.timer >= allowance) {
            // Only reachable by the floor above, and the hold has to happen at
            // the star: parked anywhere else it reads as the ball catching
            // something it never got to.
            if (!arrived) {
                w.circle_x = w.star.x;
                w.circle_y = w.star.y;
            }
            w.phase = Phase::StarHold;
            w.timer = 0.0f;
        }
        break;
    }

    case Phase::StarHold: {
        // Caught: the ball parks where the star is and the star spins on that
        // spot for `star.hold`, then lets it go on a fresh diagonal.
        //
        // This is the one phase a wall may pass over the ball for nothing. The
        // hold is what asks the player to bring the frame out to meet the ball,
        // and it is checked for exactly that when the star lets go, so charging
        // for the crossing would be charging for the very thing being demanded
        // — a ball sat on a spinning star cannot dodge, and there is no way to
        // get the frame around it that does not cross it. `StarLook` and
        // `StarSeek` still count crossings: there the ball is the player's to
        // keep clear of, and it is only here that it is the star's.
        move_square(w, cfg, in, dt);
        age_boost(w, dt);
        update_diamond(w, cfg, dt);
        update_hexagon(w, cfg, dt);
        grow_square(w, cfg, dt);
        const bool struck = update_triangles(w, cfg, dt);
        w.star.spin += cfg.star_spin_speed * kTwoPi * dt;

        // A hazard still lands, and the star's ring is still what it has to
        // get past to do it.
        if (struck) {
            w.flash = false; // a hazard's shake is the red one's, never a wall's
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
            // The ring goes with it. A ball let go at the star's own moment has
            // no say in what the field looks like when it lands, so it keeps
            // its cover for a few seconds of its own.
            w.ring = cfg.star_linger;
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
        // A forfeit is still handing over the row while the shards fly, so hold
        // here until the last one has been knocked loose — it can go on falling
        // into the fade like any other, but the taking of it has to be seen.
        if ((!update_shards(w.shards, cfg, dt) && !w.draining) ||
            w.timer >= kShardTimeout) {
            // The run is over here. Its score is handed over before the table
            // is shown rather than at the rebuild, so the run that just ended
            // is on the table it is shown — which is the whole point of
            // showing it here. `eaten` is still the run's; the reset is two
            // phases away.
            w.last_run = w.eaten;
            w.phase = Phase::EndFade;
            w.timer = 0.0f;
        }
        break;

    case Phase::EndFade:
        // The field going. This is the one fade that does not rebuild at the
        // end of it: the run is about to be counted, and `eaten` is the thing
        // being counted, so make_world() waits until the ending is over.
        if (w.timer >= kFadeOutTime) {
            w.phase = Phase::EndBlack;
            w.timer = 0.0f;
        }
        break;

    case Phase::EndBlack:
        if (w.timer >= kEndBlackTime) {
            w.phase = Phase::Tally;
            w.timer = 0.0f;
            w.tally = 0.0f;
        }
        break;

    case Phase::Tally: {
        // Up out of the black, then the number climbs. The rate is a floor on
        // the *time* as much as a speed: a run worth three diamonds would be
        // over before it read as counting at all, so a short score is counted
        // slower rather than not counted.
        const float score = static_cast<float>(w.eaten);
        const float rate = std::max(kTallyRate, score / kTallyMin);
        if (w.timer >= kTallyFade) w.tally = std::min(w.tally + rate * dt, score);
        // The hold is measured from the number landing, not from the phase
        // starting, so the beat is the same however long the count took.
        if (w.tally >= score && w.timer >= kTallyFade + kTallyHold) {
            // A run that placed goes to its row; one that did not is simply
            // over, and the next game is what follows.
            w.phase = w.last_rank >= 0 ? Phase::ToBoard : Phase::FadeOut;
            w.timer = 0.0f;
        }
        break;
    }

    case Phase::ToBoard:
        if (w.timer >= kToBoardTime) {
            w.phase = Phase::Board;
            w.timer = 0.0f;
        }
        break;

    case Phase::Board:
        // The table stays until it is dismissed. Nothing moves through it but
        // the backdrop and whatever dots are still falling, and both of those
        // run outside this switch.
        if (w.timer >= kBoardArm && in.confirm) {
            w.phase = Phase::FadeOut;
            w.timer = 0.0f;
        }
        break;

    case Phase::FadeOut:
        // Rebuild behind the black, so fading back up reveals a fresh game —
        // and reveals it on the title screen, which is where a run now ends up
        // once its score has been counted out. The fresh world's own defaults
        // are the whole of it: `started` false and `title` at 1, the same wait
        // the very first world opens on. A run is something taken up rather
        // than something you are dropped back into.
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
    if (w.phase == Phase::FadeOut || w.phase == Phase::EndFade) {
        amount = w.timer / kFadeOutTime;
    } else if (w.phase == Phase::Black || w.phase == Phase::EndBlack) {
        amount = 1.0f;
    } else if (w.phase == Phase::FadeIn) {
        amount = 1.0f - w.timer / kFadeInTime;
    } else if (w.phase == Phase::Tally) {
        // The count comes up out of the black the way a world does.
        amount = 1.0f - w.timer / kTallyFade;
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
// Bloom comes off the lit things only. Nothing dimmer than this contributes to
// it, which is what keeps a dark field dark: without a floor the whole frame
// blooms, the backdrop lifts itself into a gray one, and `glow` stops meaning
// what it says. The backdrop's own colors are kept under this on purpose.
constexpr Uint8 kGlowFloor = 0x40;

// -- Text --------------------------------------------------------------------
// The title screen is the only text in the game, and it is baked rather than
// drawn: each line is rasterized once, when the screen is built, into a white
// texture carried by its alpha — the same arrangement as the backdrop's disc —
// and tinted and faded at draw time. So the font is only open while the screen
// is being built, and a frame costs one textured quad per line.

struct Label {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0; // of the ink, cropped tight, so centering is on the letters
};

// One glyph of a laid-out line: which, and where its pen sits.
struct Glyph {
    int   index = 0;
    float x = 0.0f;
};

// One codepoint off the front of a UTF-8 string, advancing `i` past it. A
// malformed byte comes back as itself, so a title typed in plain ASCII is never
// the worse for a stray one.
Uint32 next_codepoint(const std::string& s, size_t* i) {
    const unsigned char lead = static_cast<unsigned char>(s[*i]);
    int extra = 0;
    Uint32 cp = lead;
    if      ((lead & 0xE0) == 0xC0) { extra = 1; cp = lead & 0x1F; }
    else if ((lead & 0xF0) == 0xE0) { extra = 2; cp = lead & 0x0F; }
    else if ((lead & 0xF8) == 0xF0) { extra = 3; cp = lead & 0x07; }
    for (int k = 1; k <= extra; ++k) {
        const size_t at = *i + static_cast<size_t>(k);
        const unsigned char next = at < s.size() ? static_cast<unsigned char>(s[at]) : 0;
        if ((next & 0xC0) != 0x80) { // cut off, or not a continuation: a stray
            ++*i;
            return lead;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    *i += static_cast<size_t>(extra) + 1;
    return cp;
}

// Lays a line along a baseline: where each glyph's pen sits, and how far the
// last one reaches. Kerning is the font's own; `tracking` is laid on top of it,
// in pixels, since display type at a title's size wants more air between the
// letters than a font's text metrics give it.
float layout_text(const stbtt_fontinfo& font, const std::string& text, float scale,
                  float tracking, std::vector<Glyph>* out) {
    float pen = 0.0f;
    int prev = 0;
    size_t i = 0;
    while (i < text.size()) {
        const Uint32 cp = next_codepoint(text, &i);
        const int index = stbtt_FindGlyphIndex(&font, static_cast<int>(cp));
        if (prev) {
            pen += static_cast<float>(stbtt_GetGlyphKernAdvance(&font, prev, index)) * scale +
                   tracking;
        }
        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&font, index, &advance, &bearing);
        if (out) out->push_back(Glyph{index, pen});
        pen += static_cast<float>(advance) * scale;
        prev = index;
    }
    return pen;
}

// How wide a line comes to at a pixel height — what the title's fitting needs
// to know before anything is rasterized. Advances scale linearly with the
// height, so measuring once at any size says how big the line can be.
float measure_text(const stbtt_fontinfo& font, const std::string& text,
                   float pixel_height, float tracking) {
    const float scale = stbtt_ScaleForPixelHeight(&font, pixel_height);
    return layout_text(font, text, scale, tracking * pixel_height, nullptr);
}

// Rasterizes a line into a texture. Each glyph is drawn on its own and laid in
// with a max, so two that overlap — a tight pair, a kern that tucks one under
// another — union rather than the second clobbering the first. The result is
// cropped to its ink, so a label's box is the letters and nothing else.
Label bake_text(SDL_Renderer* renderer, const stbtt_fontinfo& font,
                const std::string& text, float pixel_height, float tracking) {
    Label label;
    const float scale = stbtt_ScaleForPixelHeight(&font, pixel_height);
    std::vector<Glyph> glyphs;
    layout_text(font, text, scale, tracking * pixel_height, &glyphs);
    if (glyphs.empty()) return label;

    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    const int baseline = static_cast<int>(std::ceil(static_cast<float>(ascent) * scale));
    const int height = baseline + static_cast<int>(std::ceil(static_cast<float>(-descent) * scale)) + 2;

    // The bitmap has to hold every glyph's box, which can poke out past its
    // own advance on either side.
    int left = 0, right = 0;
    for (const Glyph& g : glyphs) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&font, g.index, scale, scale, &x0, &y0, &x1, &y1);
        const int pen = static_cast<int>(std::lround(g.x));
        left  = std::min(left, pen + x0);
        right = std::max(right, pen + x1);
    }
    const int width = right - left + 2;
    if (width <= 2 || height <= 2) return label;

    std::vector<Uint8> coverage(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    std::vector<Uint8> glyph;
    for (const Glyph& g : glyphs) {
        if (g.index == 0) continue; // the font has no such glyph; leave its advance empty
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&font, g.index, scale, scale, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue; // a space

        glyph.assign(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
        stbtt_MakeGlyphBitmap(&font, glyph.data(), gw, gh, gw, scale, scale, g.index);

        const int ox = static_cast<int>(std::lround(g.x)) + x0 - left + 1;
        const int oy = baseline + y0 + 1;
        for (int y = 0; y < gh; ++y) {
            const int ty = oy + y;
            if (ty < 0 || ty >= height) continue;
            for (int x = 0; x < gw; ++x) {
                const int tx = ox + x;
                if (tx < 0 || tx >= width) continue;
                Uint8& dst = coverage[static_cast<size_t>(ty) * static_cast<size_t>(width) +
                                      static_cast<size_t>(tx)];
                dst = std::max(dst, glyph[static_cast<size_t>(y) * static_cast<size_t>(gw) +
                                          static_cast<size_t>(x)]);
            }
        }
    }

    // Crop to the ink, with a pixel of clear around it for the filter to
    // sample. An all-caps line in a face with room for descenders would
    // otherwise center a third of the way down its own empty space.
    int ink_left = width, ink_top = height, ink_right = -1, ink_bottom = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!coverage[static_cast<size_t>(y) * static_cast<size_t>(width) +
                          static_cast<size_t>(x)]) continue;
            ink_left   = std::min(ink_left, x);
            ink_right  = std::max(ink_right, x);
            ink_top    = std::min(ink_top, y);
            ink_bottom = std::max(ink_bottom, y);
        }
    }
    if (ink_right < ink_left) return label; // nothing but spaces
    ink_left   = std::max(ink_left - 1, 0);
    ink_top    = std::max(ink_top - 1, 0);
    ink_right  = std::min(ink_right + 1, width - 1);
    ink_bottom = std::min(ink_bottom + 1, height - 1);
    label.w = ink_right - ink_left + 1;
    label.h = ink_bottom - ink_top + 1;

    std::vector<Uint32> pixels(static_cast<size_t>(label.w) * static_cast<size_t>(label.h));
    for (int y = 0; y < label.h; ++y) {
        for (int x = 0; x < label.w; ++x) {
            const Uint32 alpha = coverage[static_cast<size_t>(ink_top + y) * static_cast<size_t>(width) +
                                          static_cast<size_t>(ink_left + x)];
            pixels[static_cast<size_t>(y) * static_cast<size_t>(label.w) + static_cast<size_t>(x)] =
                (alpha << 24) | 0x00FFFFFFu; // white, carried by its alpha
        }
    }

    label.tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STATIC, label.w, label.h);
    if (!label.tex) {
        SDL_Log("no texture for \"%s\" (%s)", text.c_str(), SDL_GetError());
        label = Label{};
        return label;
    }
    SDL_UpdateTexture(label.tex, nullptr, pixels.data(),
                      label.w * static_cast<int>(sizeof(Uint32)));
    SDL_SetTextureScaleMode(label.tex, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(label.tex, SDL_BLENDMODE_BLEND);
    return label;
}

void free_label(Label& l) {
    if (l.tex) SDL_DestroyTexture(l.tex);
    l = Label{};
}

// A label centered on a point, in a color, at a strength. The corner is
// rounded to a pixel so the letters land 1:1 on the frame rather than being
// resampled across a half-pixel seam.
// Everything here is baked to the size it is drawn at, with one exception: the
// group that counts a run out shrinks onto its row, and a size that moves
// cannot be baked. It is baked at each end instead and scaled only in between,
// so both of the sizes it rests at are still crisp.
void draw_label_scaled(SDL_Renderer* renderer, const Label& l, float cx, float cy,
                       SDL_Color tint, float alpha, float scale) {
    if (!l.tex || alpha <= 0.0f || scale <= 0.0f) return;
    SDL_SetTextureColorMod(l.tex, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(l.tex, static_cast<Uint8>(std::lround(
        std::clamp(alpha, 0.0f, 1.0f) * 255.0f)));
    const float w = static_cast<float>(l.w) * scale;
    const float h = static_cast<float>(l.h) * scale;
    const SDL_FRect dest{std::round(cx - w * 0.5f), std::round(cy - h * 0.5f), w, h};
    SDL_RenderCopyF(renderer, l.tex, nullptr, &dest);
}

void draw_label(SDL_Renderer* renderer, const Label& l, float cx, float cy,
                SDL_Color tint, float alpha) {
    draw_label_scaled(renderer, l, cx, cy, tint, alpha, 1.0f);
}

bool read_file(const std::string& path, std::vector<unsigned char>* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !out->empty();
}

// ── The high score board ─────────────────────────────────────────────────────
// Five scores, best first, 0 standing for a row nobody has reached. Every
// failure here is a log line and nothing more: a board that cannot be read is
// an empty one, a board that cannot be written is a run that went unrecorded,
// and neither is worth refusing to play over.
struct Scores {
    std::array<int, kBoardRows> best{};
};

// Empty if SDL has no folder to offer, which every caller reads as "no board".
std::string scores_path() {
    char* dir = SDL_GetPrefPath(kPrefOrg, kPrefApp);
    if (!dir) {
        SDL_Log("no place to keep the score board (%s)", SDL_GetError());
        return std::string();
    }
    std::string path = std::string(dir) + kScoresFile;
    SDL_free(dir);
    return path;
}

void load_scores(Scores& s) {
    s = Scores{};
    const std::string path = scores_path();
    if (path.empty()) return;
    std::ifstream file(path);
    if (!file) return; // no file yet is a first run, not a fault
    try {
        const nlohmann::json root = nlohmann::json::parse(file, nullptr, true, true);
        const nlohmann::json best = root.value("best", nlohmann::json::array());
        const int count = std::min(static_cast<int>(best.size()), kBoardRows);
        for (int i = 0; i < count; ++i) {
            s.best[i] = std::max(best[i].get<int>(), 0);
        }
    } catch (const std::exception& e) {
        SDL_Log("the score board could not be read (%s); starting a fresh one", e.what());
        s = Scores{};
        return;
    }
    // The file's order is not trusted: sorting here is what lets a board that
    // was edited by hand still read the way a board is meant to.
    std::sort(s.best.begin(), s.best.end(), [](int a, int b) { return a > b; });
}

void save_scores(const Scores& s) {
    const std::string path = scores_path();
    if (path.empty()) return;
    std::ofstream file(path);
    if (!file) {
        SDL_Log("the score board could not be written to %s", path.c_str());
        return;
    }
    nlohmann::json root;
    root["best"] = s.best;
    file << root.dump(2) << '\n';
}

// Puts a finished run on the board and says which row it took, or -1 for one
// that did not place — which is what the ending reads to know whether the count
// has anywhere to fly to. A run that ate nothing is not a score: the board is
// for what was reached, and 0 is where everyone starts.
int record_score(Scores& s, int score) {
    if (score <= 0 || score <= s.best[kBoardRows - 1]) return -1;
    s.best[kBoardRows - 1] = score;
    std::sort(s.best.begin(), s.best.end(), [](int a, int b) { return a > b; });
    save_scores(s);
    // Where it came to rest. Ties are scanned from the bottom, so a run that
    // only matches an older one takes the row under it: the score that got
    // there first keeps the better place.
    for (int i = kBoardRows - 1; i >= 0; --i) {
        if (s.best[static_cast<size_t>(i)] == score) return i;
    }
    return -1;
}

// One size of the number face. The digits are baked apart and set on a fixed
// advance rather than laid out as a word, because the count-up's number
// changes every few frames and proportional digits would shuffle the whole of
// it sideways on every tick.
struct NumFace {
    std::array<Label, 10> digit{};
    Label ex;         // the X between the diamond and the number
    float adv = 0.0f; // the widest digit plus its gap
    float h   = 0.0f; // a digit's height, which the group's layout is measured in
};

void free_face(NumFace& f) {
    for (Label& l : f.digit) free_label(l);
    free_label(f.ex);
    f = NumFace{};
}

void bake_face(NumFace& f, SDL_Renderer* renderer, const stbtt_fontinfo& font,
               float size) {
    free_face(f);
    for (int d = 0; d < 10; ++d) {
        f.digit[static_cast<size_t>(d)] =
            bake_text(renderer, font, std::to_string(d), size, 0.0f);
        const Label& l = f.digit[static_cast<size_t>(d)];
        f.h   = std::max(f.h, static_cast<float>(l.h));
        f.adv = std::max(f.adv, static_cast<float>(l.w));
    }
    f.ex = bake_text(renderer, font, "X", size * kGroupExSize, 0.0f);
    f.adv += f.h * kDigitGap;
}

int digit_count(int value) {
    int n = 1;
    for (int v = value; v >= 10; v /= 10) ++n;
    return n;
}

// A diamond, an X and a number, measured in digit heights so the one layout
// serves the size it counts at and the size it comes to rest at alike.
float group_width(const NumFace& f, int value, float scale) {
    return (kGroupDiaW * 2.0f + kGroupGap * 2.0f) * f.h * scale +
           static_cast<float>(f.ex.w) * scale +
           f.adv * scale * static_cast<float>(digit_count(value));
}

// Hung off its left edge rather than its center, so a row always starts where
// the group landing on it has to arrive, whatever the number turns out to be.
void draw_group(SDL_Renderer* renderer, const NumFace& f, int value, float left_x,
                float cy, float scale, float alpha, SDL_Color ink) {
    if (alpha <= 0.0f || scale <= 0.0f) return;
    const float clamped = std::clamp(alpha, 0.0f, 1.0f);
    const float dia_hw = kGroupDiaW * f.h * scale;
    const float dia_hh = kGroupDiaH * f.h * scale;
    const float gap    = kGroupGap * f.h * scale;
    float x = left_x;
    SDL_Color green = kDiamondColor;
    green.a = static_cast<Uint8>(std::lround(clamped * 255.0f));
    set_draw_color(renderer, green);
    fill_diamond(renderer, x + dia_hw, cy, dia_hw, dia_hh);
    x += dia_hw * 2.0f + gap;
    draw_label_scaled(renderer, f.ex, x + static_cast<float>(f.ex.w) * scale * 0.5f, cy,
                      ink, clamped, scale);
    x += static_cast<float>(f.ex.w) * scale + gap;
    const int count = digit_count(value);
    for (int i = count - 1; i >= 0; --i) {
        int digit = value;
        for (int drop = 0; drop < i; ++drop) digit /= 10;
        draw_label_scaled(renderer, f.digit[static_cast<size_t>(digit % 10)],
                          x + f.adv * scale * 0.5f, cy, ink, clamped, scale);
        x += f.adv * scale;
    }
}

struct Screen {
    SDL_Texture* frame = nullptr; // the field, drawn at config size
    SDL_Texture* half  = nullptr; // halfway down to the blur
    SDL_Texture* glow  = nullptr; // quarter size, added back for phosphor bloom
    SDL_Texture* lines = nullptr; // 1 x height scanline mask
    SDL_Texture* disc  = nullptr; // one white circle, tinted per backdrop tile
    // How the floor is taken off the bloom. NONE if the backend has no
    // subtract, in which case the glow is the old indiscriminate one.
    SDL_BlendMode take_floor = SDL_BLENDMODE_NONE;
    // The title screen's lines, baked once. Any of them can be empty — no
    // font, no texture, a blank subtitle — and the screen simply has less on it.
    Label title, subtitle, prompt, version;
    // The keys, one block either side of the field.
    Label keys_left_head, keys_left, keys_right_head, keys_right;
    // The board. Its numbers are not baked: they are set from `big`/`small`, a
    // digit at a time, which is what lets the count-up change its number every
    // few frames and the row it lands on be drawn by the very same code.
    Label board_head, board_prompt;
    std::array<Label, kBoardRows> board_rank{};
    NumFace big, small; // the size the run is counted at, and the size a row is
};

void free_screen(Screen& s) {
    if (s.frame) SDL_DestroyTexture(s.frame);
    if (s.half)  SDL_DestroyTexture(s.half);
    if (s.glow)  SDL_DestroyTexture(s.glow);
    if (s.lines) SDL_DestroyTexture(s.lines);
    if (s.disc)  SDL_DestroyTexture(s.disc);
    free_label(s.title);
    free_label(s.subtitle);
    free_label(s.prompt);
    free_label(s.keys_left_head);
    free_label(s.keys_left);
    free_label(s.keys_right_head);
    free_label(s.keys_right);
    free_label(s.version);
    free_label(s.board_head);
    free_label(s.board_prompt);
    for (Label& l : s.board_rank) free_label(l);
    free_face(s.big);
    free_face(s.small);
    s = Screen{};
}

// Opens the face the screen is set in: the one built into the exe unless
// `title.font` names a file, which is looked for where config.json is and read
// whole — stb_truetype works straight off the bytes, so the buffer has to
// outlive every glyph baked from it, which is why it is the caller's to hold.
// A file that is missing or not a font is logged and the built-in one stands in
// for it, so the screen always has its words. False is the one case with no
// face at all, and every caller reads it as having nothing to bake.
bool open_face(const Config& cfg, std::vector<unsigned char>& bytes,
               stbtt_fontinfo& font) {
    const unsigned char* data = kFontData;
    bytes.clear();
    if (!cfg.title_font.empty()) {
        for (const std::string& path : search_paths(cfg.title_font)) {
            if (read_file(path, &bytes)) break;
        }
        if (bytes.empty()) {
            SDL_Log("no font at %s; using the built-in one", cfg.title_font.c_str());
        } else {
            data = bytes.data();
        }
    }
    if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        if (data == kFontData) {
            SDL_Log("the built-in font failed to load; the title screen has no text");
            return false;
        }
        SDL_Log("%s is not a font stb_truetype can read; using the built-in one",
                cfg.title_font.c_str());
        data = kFontData;
        bytes.clear();
        if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0))) return false;
    }
    return true;
}

// Bakes every fixed thing on the screen. Nothing here depends on a score any
// more — the numbers are set from the faces at draw time — so this runs once at
// startup and again on R, and never in between.
void build_labels(Screen& s, SDL_Renderer* renderer, const Config& cfg) {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo font;
    if (!open_face(cfg, bytes, font)) return;

    // The name is fitted to the window: baked at `kTitleSize` unless that
    // would run it past `kTitleWidth` of the width, in which case it is baked
    // smaller rather than squashed at draw time — a texture scaled down would
    // smear where one baked to size stays crisp.
    float size = kTitleSize;
    const float wide = measure_text(font, cfg.title_text, size, kTitleTrack);
    const float room = static_cast<float>(cfg.window_w) * kTitleWidth;
    if (wide > room && wide > 0.0f) size *= room / wide;

    s.title    = bake_text(renderer, font, cfg.title_text, size, kTitleTrack);
    s.subtitle = bake_text(renderer, font, cfg.title_subtitle, kSubtitleSize, kSubtitleTrack);
    s.prompt   = bake_text(renderer, font, kPromptText, kPromptSize, kPromptTrack);
    s.keys_left_head  = bake_text(renderer, font, kKeysLeftHead, kKeysHeadSize,
                                  kKeysHeadTrack);
    s.keys_left       = bake_text(renderer, font, kKeysLeftText, kKeysSize, kKeysTrack);
    s.keys_right_head = bake_text(renderer, font, kKeysRightHead, kKeysHeadSize,
                                  kKeysHeadTrack);
    s.keys_right      = bake_text(renderer, font, kKeysRightText, kKeysSize, kKeysTrack);
    s.version  = bake_text(renderer, font, kVersionText, kVersionSize, kVersionTrack);
    s.board_prompt = bake_text(renderer, font, kBoardPromptText, kBoardPromptSize,
                               kBoardPromptTrack);
    s.board_head = bake_text(renderer, font, kBoardHeadText, kBoardHeadSize,
                             kBoardHeadTrack);
    for (int i = 0; i < kBoardRows; ++i) {
        s.board_rank[static_cast<size_t>(i)] =
            bake_text(renderer, font, std::to_string(i + 1) + ".", kBoardSize,
                      kBoardTrack);
    }
    // Both ends of the trip the count-up makes, each baked to the size it comes
    // to rest at, so only the moving part of it is ever scaled.
    bake_face(s.small, renderer, font, kBoardSize);
    bake_face(s.big, renderer, font, kBoardSize * kTallyScale);
}

// Sized off the config, so a reload rebuilds it. A failure here leaves `frame`
// null, which every pass below reads as "the game drew straight to the window"
// - a renderer that cannot hold a target still gets a game, just a flat one.
void build_screen(Screen& s, SDL_Renderer* renderer, const Config& cfg) {
    free_screen(s);
    build_labels(s, renderer, cfg); // before the frame: a flat game still has a title

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

    // One white disc, tinted and stretched per backdrop tile. Circles that big
    // drawn a row at a time would be tens of thousands of line calls a frame,
    // where this is one textured quad each; scaling it up is also what softens
    // the rim, which a row of spans could not do at all.
    {
        std::vector<Uint32> pixels(static_cast<size_t>(kDiscSize) * kDiscSize);
        const float mid = static_cast<float>(kDiscSize) * 0.5f;
        for (int y = 0; y < kDiscSize; ++y) {
            for (int x = 0; x < kDiscSize; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - mid;
                const float dy = static_cast<float>(y) + 0.5f - mid;
                // Coverage across the last pixel, so the edge is not a staircase.
                const float edge = mid - std::sqrt(dx * dx + dy * dy);
                const float on = std::clamp(edge + 0.5f, 0.0f, 1.0f);
                const Uint32 alpha = static_cast<Uint32>(std::lround(on * 255.0f));
                pixels[static_cast<size_t>(y) * kDiscSize + static_cast<size_t>(x)] =
                    (alpha << 24) | 0x00FFFFFFu; // white, carried by its alpha
            }
        }
        s.disc = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STATIC, kDiscSize, kDiscSize);
        if (s.disc) {
            SDL_UpdateTexture(s.disc, nullptr, pixels.data(), kDiscSize * sizeof(Uint32));
            SDL_SetTextureScaleMode(s.disc, SDL_ScaleModeLinear);
            SDL_SetTextureBlendMode(s.disc, SDL_BLENDMODE_BLEND);
        }
    }

    // Subtracting the bloom's floor needs a blend the backend may not have, so
    // ask once here rather than per frame. dst - src, clamped at zero for free.
    const SDL_BlendMode take = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_REV_SUBTRACT,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    if (SDL_SetRenderDrawBlendMode(renderer, take) == 0) {
        s.take_floor = take;
    } else {
        SDL_Log("no subtract blend (%s); the glow will lift the whole frame",
                SDL_GetError());
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND); // back to the fade's

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
        // Take the floor off before blurring, so only what was already lit is
        // left to spread. This is what a shader would do with a comparison and
        // what there is instead of one: a subtract over the whole target, which
        // clamps at zero on its own, leaving the dark of the frame at nothing.
        copy_into(renderer, screen.half, screen.frame);
        if (screen.take_floor != SDL_BLENDMODE_NONE) {
            SDL_SetRenderDrawBlendMode(renderer, screen.take_floor);
            SDL_SetRenderDrawColor(renderer, kGlowFloor, kGlowFloor, kGlowFloor, 0);
            SDL_RenderFillRect(renderer, nullptr);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        }

        // Two halvings: each is a bilinear filter averaging a 2x2 block, which
        // buys a box blur for the price of two blits. Going straight down to a
        // quarter would skip three pixels in four instead of averaging them.
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

// A row's line, and the left edge its group is hung from. Both the board and
// the trip that lands on it come through here, so what the group flies to is
// by construction where the row draws.
float board_row_y(const Config& cfg, int rank) {
    return static_cast<float>(cfg.window_h) *
           (kBoardY + kBoardHeadGap + kBoardStep * static_cast<float>(rank));
}

// Where a row's two parts sit. The table is centered on the window as a whole
// block rather than each row being hung off the middle: a row is a rank and
// then a group, which is nothing like symmetric, so centering the middle of a
// row would sit the table off to one side of its own heading. Measured from
// the widest rank and the widest group actually on the table, so it is the ink
// that is centered and not some nominal column.
struct BoardLayout {
    float rank_right = 0.0f; // ranks are right-aligned to here
    float group_left = 0.0f; // groups are hung from here
};

BoardLayout board_layout(const Config& cfg, const Screen& s, const Scores& scores) {
    float rank_w = 0.0f, group_w = 0.0f;
    for (int i = 0; i < kBoardRows; ++i) {
        if (scores.best[static_cast<size_t>(i)] <= 0) continue;
        rank_w = std::max(rank_w,
                          static_cast<float>(s.board_rank[static_cast<size_t>(i)].w));
        group_w = std::max(group_w, group_width(s.small,
                                                scores.best[static_cast<size_t>(i)], 1.0f));
    }
    const float total = rank_w + kBoardGap + group_w;
    const float left = static_cast<float>(cfg.window_w) * 0.5f - total * 0.5f;
    return BoardLayout{left + rank_w, left + rank_w + kBoardGap};
}

// The table. Drawn at whatever it is worth being seen at: 0 while the count is
// still the whole screen, 1 once the group has come home.
void draw_board(SDL_Renderer* renderer, const Screen& screen, const Config& cfg,
                const Scores& scores, float alpha) {
    if (alpha <= 0.0f || scores.best[0] <= 0) return;
    const float mid = static_cast<float>(cfg.window_w) * 0.5f;
    const BoardLayout at = board_layout(cfg, screen, scores);
    draw_label(renderer, screen.board_head, mid,
               static_cast<float>(cfg.window_h) * kBoardY, kBoardHeadColor, alpha);
    for (int i = 0; i < kBoardRows; ++i) {
        if (scores.best[static_cast<size_t>(i)] <= 0) continue;
        const float line_y = board_row_y(cfg, i);
        const Label& rank = screen.board_rank[static_cast<size_t>(i)];
        draw_label(renderer, rank, at.rank_right - static_cast<float>(rank.w) * 0.5f,
                   line_y, kBoardColor, alpha);
        draw_group(renderer, screen.small, scores.best[static_cast<size_t>(i)],
                   at.group_left, line_y, 1.0f, alpha, kBoardColor);
    }
}

// The ending, from the count coming up out of the black to the table waiting to
// be dismissed. The field is gone by now — the backdrop is the whole of what is
// behind this — so it is drawn in the field's place rather than over it.
void draw_ending(SDL_Renderer* renderer, const Screen& screen, const World& w,
                 const Config& cfg, const Scores& scores) {
    const float mid = static_cast<float>(cfg.window_w) * 0.5f;
    const float tally_y = static_cast<float>(cfg.window_h) * kTallyY;
    const int shown = static_cast<int>(w.tally);

    if (w.phase == Phase::Tally) {
        // Centered while it is the only thing on the screen. It is hung off its
        // left edge like every other group, so the center is taken off its width.
        draw_group(renderer, screen.big, shown,
                   mid - group_width(screen.big, shown, 1.0f) * 0.5f, tally_y,
                   1.0f, 1.0f, kBoardColor);
        return;
    }

    if (w.phase == Phase::ToBoard) {
        // The trip. The group is baked big and the row is baked small, so the
        // scale runs to the ratio between them rather than to some number of
        // its own — it arrives exactly the size the row it lands on is drawn.
        const float t = std::clamp(w.timer / kToBoardTime, 0.0f, 1.0f);
        const float ease = t * t * (3.0f - 2.0f * t);
        const float end_scale = screen.big.h > 0.0f ? screen.small.h / screen.big.h
                                                    : 1.0f;
        const float scale = 1.0f + (end_scale - 1.0f) * ease;
        const float from_x = mid - group_width(screen.big, shown, 1.0f) * 0.5f;
        const float x =
            from_x + (board_layout(cfg, screen, scores).group_left - from_x) * ease;
        const float y = tally_y + (board_row_y(cfg, w.last_rank) - tally_y) * ease;
        // The table comes up under it, and the group hands over to the row it
        // is landing on: by the end the two are the same size in the same place,
        // so the crossfade is what makes the swap invisible.
        draw_board(renderer, screen, cfg, scores, ease);
        const float handover =
            std::clamp((1.0f - t) / kToBoardFade, 0.0f, 1.0f);
        draw_group(renderer, screen.big, shown, x, y, scale, handover, kBoardColor);
        return;
    }

    if (w.phase == Phase::FadeOut) {
        // The ending's way out. Whatever the last beat was showing stays put
        // while the black comes over it: the field is a whole game away by now,
        // and popping it back for the length of a fade would undo the ending
        // one frame before it finishes. Which beat that was is `last_rank` —
        // the table if the run placed, the count it was left on if it did not —
        // and it holds until the rebuild at the end of this very phase.
        if (w.last_rank >= 0) {
            draw_board(renderer, screen, cfg, scores, 1.0f);
        } else {
            draw_group(renderer, screen.big, shown,
                       mid - group_width(screen.big, shown, 1.0f) * 0.5f, tally_y,
                       1.0f, 1.0f, kBoardColor);
        }
        return;
    }

    // Phase::Board — the table on its own, waiting.
    draw_board(renderer, screen, cfg, scores, 1.0f);
    if (w.timer >= kBoardArm) {
        // It breathes the way the title's prompt does, and for the same reason:
        // a line that is waiting on you should look like it. It comes up only
        // once the table is listening, so it never asks for a key it will ignore.
        const float breath = 0.5f + 0.5f * std::sin(w.drift * kPromptPulse * kTwoPi);
        draw_label(renderer, screen.board_prompt, mid,
                   static_cast<float>(cfg.window_h) * kBoardPromptY, cfg.square_color,
                   kPromptFloor + (1.0f - kPromptFloor) * breath);
    }
}

void render(SDL_Renderer* renderer, const Screen& screen, const World& w,
            const Config& cfg, const Scores& scores) {
    // All of this lands on the off-screen frame; the tube pass puts it up.
    SDL_SetRenderTarget(renderer, screen.frame);
    set_draw_color(renderer, cfg.background_color);
    SDL_RenderClear(renderer);

    // The backdrop goes down first, under the whole field. It covers the clear
    // completely, so `background.color` is what shows through the gaps — and
    // there are none: each tile is big enough to cover its own cell from
    // anywhere on the circle it walks, which is what `kBackSizeMin` is for.
    const float turn = w.drift * kBackSpin * kTwoPi;
    for (int i = 0; i < w.back_count; ++i) {
        const Tile& t = w.back[static_cast<size_t>(i)];
        const float mid_x = t.x + std::cos(turn + t.phase) * t.orbit;
        const float mid_y = t.y + std::sin(turn + t.phase) * t.orbit;
        const SDL_FRect tile{mid_x - t.size * 0.5f, mid_y - t.size * 0.5f,
                             t.size, t.size};
        if (screen.disc) {
            SDL_SetTextureColorMod(screen.disc, t.color.r, t.color.g, t.color.b);
            SDL_RenderCopyF(renderer, screen.disc, nullptr, &tile);
        } else { // no disc to stretch; the field is still covered, just square
            set_draw_color(renderer, t.color);
            SDL_RenderFillRectF(renderer, &tile);
        }
    }

    // Once the run is being counted out, the backdrop is the whole of what is
    // behind it: no square, no ball, no HUD. The ending is drawn in the field's
    // place rather than over it, which is what the plain background asks for and
    // saves the field a pass it would only be covered up for.
    if (w.phase == Phase::Tally || w.phase == Phase::ToBoard ||
        w.phase == Phase::Board || w.phase == Phase::FadeOut) {
        draw_ending(renderer, screen, w, cfg, scores);
        if (const Uint8 fade = fade_alpha(w); fade > 0) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, fade);
            SDL_RenderFillRect(renderer, nullptr);
        }
        present_screen(renderer, screen, cfg);
        return;
    }

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
        } else if (w.phase == Phase::Squeeze) {
            // The ball starts to strain against the walls, harder the nearer the
            // grip comes to taking it, and that is the only warning there is —
            // nothing else about it changes. It runs the other way up from a
            // hit's rattle, which starts hard and settles.
            const float strain = squeeze_strain(w, cfg);
            offset_x = kSqueezeShake * strain * std::sin(w.squeeze_time * kSqueezeRate);
            offset_y = kSqueezeShake * strain *
                       std::sin(w.squeeze_time * kSqueezeRate * 1.7f) * 0.6f;
        }
        set_draw_color(renderer, ball_color(w, cfg));
        fill_circle(renderer, w.circle_x + offset_x, w.circle_y + offset_y,
                    ball_radius(w, cfg));
        fill_eye(renderer, w, w.circle_x + offset_x, w.circle_y + offset_y,
                 ball_radius(w, cfg));

        // Caught: while the star has the ball, the ball wears a ring in the
        // star's color. Drawn with the ball rather than with the star, so it
        // takes the same rattle and the star itself still lands on top of it.
        if (ring_is_shown(w)) {
            // Same two radii the hazard test measures against, so the ring a
            // hazard breaks on is the ring you can see it break on. Always at
            // full strength — it blinks out rather than fading, since a ring
            // faded to nothing would be at its least visible exactly when
            // knowing whether it is still there is worth the most.
            float outer = 0.0f, inner = 0.0f;
            star_ring(w, cfg, &outer, &inner);
            set_draw_color(renderer, cfg.star_color);
            fill_ring(renderer, w.circle_x + offset_x, w.circle_y + offset_y,
                      outer, inner);
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

    // HUD: the grab's charge and the hits left along the top, the diamonds eaten
    // along the bottom. Drawn last of the field so nothing — square, ball,
    // hazard or star — can cover them; only the fade goes over. It comes up as
    // the title screen goes down, through the same number: a full bar and three
    // lives mean nothing over a run that has not begun.
    const float hud = 1.0f - w.title;
    const auto faded = [hud](SDL_Color c) {
        c.a = static_cast<Uint8>(std::lround(static_cast<float>(c.a) * hud));
        return c;
    };

    // The charge bar: a dark gray track with the charge laid over it in the
    // square's own color, since it is the square's to spend. It fills both ways
    // from the middle, the way the tally along the bottom grows, so the two HUD
    // rows read as one family. Full means the frame can take hold of the ball.
    // The bar grows with what a fill costs rather than filling more slowly.
    // `squeeze.bar_width` is the width of the *first* fill, scaled by how much
    // dearer this one has become; since the filled part is the width times
    // `charge` and `charge` climbs at `dt / recharge`, a width proportional to
    // `recharge` makes the two cancel and the edge advances at the same pixels a
    // second all run. What a run has cost itself is then something on the screen
    // rather than only something felt.
    // It cannot grow past the window. Past that point the bar is as long as it
    // gets and the fill does slow again, which is the same bargain
    // `kRechargeCeil` makes: a long wait rather than a wall.
    const float bar_w = std::min(cfg.squeeze_bar_width * w.bar_span,
                                 static_cast<float>(cfg.window_w) * kBarMaxWidth);
    const float bar_x = static_cast<float>(cfg.window_w) * 0.5f - bar_w * 0.5f;
    const SDL_Rect track{
        static_cast<int>(std::lround(bar_x)), static_cast<int>(std::lround(kBarTop)),
        static_cast<int>(std::lround(bar_w)),
        static_cast<int>(std::lround(kBarHeight)),
    };
    set_draw_color(renderer, faded(kBarTrack));
    SDL_RenderFillRect(renderer, &track);

    const int filled = static_cast<int>(std::lround(static_cast<float>(track.w) * w.charge));
    if (filled > 0) {
        const SDL_Rect charged{track.x + (track.w - filled) / 2, track.y, filled, track.h};
        set_draw_color(renderer, faded(cfg.square_color));
        SDL_RenderFillRect(renderer, &charged);
    }

    set_draw_color(renderer, faded(cfg.circle_color)); // the dots match the ball
    for (const Dot& d : w.dots) {
        if (!d.gone) fill_circle(renderer, d.x, d.y, kDotRadius);
    }

    // The tally is centered on the window, so it opens outward as it fills.
    const int fits = std::max(cfg.window_w / static_cast<int>(kScoreSpacing) - 1, 1);
    const int shown = std::min(w.eaten, fits);
    const float row_x = static_cast<float>(cfg.window_w) * 0.5f -
                        kScoreSpacing * static_cast<float>(shown - 1) * 0.5f;
    const float row_y = static_cast<float>(cfg.window_h) - kScoreBottom;
    set_draw_color(renderer, faded(kDiamondColor));
    for (int i = 0; i < shown; ++i) {
        fill_diamond(renderer, row_x + kScoreSpacing * static_cast<float>(i), row_y,
                     kDiamondHalfW * kScoreScale, kDiamondHalfH * kScoreScale);
    }

    // The title screen, over the field and under the fade, so it fades up with
    // the first world and goes on its own once the run begins. The name is in
    // the ball's color and the prompt in the square's — the two things the
    // player is about to be given — and the prompt breathes, since a line that
    // is waiting on you should look like it. `drift` is its clock, being the one
    // that runs through everything.
    if (w.title > 0.0f) {
        const float mid_x = static_cast<float>(cfg.window_w) * 0.5f;
        const float breath = 0.5f + 0.5f * std::sin(w.drift * kPromptPulse * kTwoPi);
        const float pulse = kPromptFloor + (1.0f - kPromptFloor) * breath;
        draw_label(renderer, screen.title, mid_x,
                   static_cast<float>(cfg.window_h) * kTitleY, cfg.circle_color, w.title);
        draw_label(renderer, screen.subtitle, mid_x,
                   static_cast<float>(cfg.window_h) * kSubtitleY, kSubtitleColor, w.title);
        draw_label(renderer, screen.prompt, mid_x,
                   static_cast<float>(cfg.window_h) * kPromptY, cfg.square_color,
                   w.title * pulse);
        // A block a hand, out in the margins. The headings take the square's
        // color, both being things it does; the keys themselves stay gray, the
        // way the one line they replace was.
        const float keys_y = static_cast<float>(cfg.window_h) * kKeysY;
        const float keys_under = keys_y + static_cast<float>(cfg.window_h) * kKeysGap;
        const float left_x  = static_cast<float>(cfg.window_w) * kKeysX;
        const float right_x = static_cast<float>(cfg.window_w) * (1.0f - kKeysX);
        draw_label(renderer, screen.keys_left_head, left_x, keys_y,
                   cfg.square_color, w.title);
        draw_label(renderer, screen.keys_left, left_x, keys_under, kHintColor, w.title);
        draw_label(renderer, screen.keys_right_head, right_x, keys_y,
                   cfg.square_color, w.title);
        draw_label(renderer, screen.keys_right, right_x, keys_under, kHintColor, w.title);
        // The version sits in the corner rather than on the center line, so
        // it is anchored by its far edge: the margin is from the ink to the
        // window's edge whatever the number's length.
        draw_label(renderer, screen.version,
                   static_cast<float>(cfg.window_w) - kVersionMargin -
                       static_cast<float>(screen.version.w) * 0.5f,
                   static_cast<float>(cfg.window_h) - kVersionMargin -
                       static_cast<float>(screen.version.h) * 0.5f,
                   kVersionColor, w.title);
    }

    // The table, and the one thing on the screen that is a run's ending rather
    // than part of it. It comes after the HUD so the scrim puts that down along
    // with the field — for these few seconds the numbers are what is lit — and
    // before the fade, which is what takes it away. Each row is hung off a
    // gutter rather than centered: the rank anchored by its right edge and the
    // score by its left, the same far-edge anchoring the version corner uses,
    // so the digits line up instead of wandering as they change.
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

    // The pad comes up on its own, and its failing is not the game's problem:
    // everything it does the keyboard does too, so a machine that cannot bring
    // the subsystem up gets a logged line and a game, not a dead start.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("no controller support: %s", SDL_GetError());
    }

    // One pad at a time: the first one found drives, and if it is unplugged the
    // next one to arrive takes over. Nothing here is two-player, so there is
    // nothing to be had from tracking more than the one.
    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        pad = SDL_GameControllerOpen(i);
        if (pad) {
            SDL_Log("controller: %s", SDL_GameControllerName(pad));
            break;
        }
    }

    Config cfg = load_and_report();

    // The game opens on the glass. Fullscreen is the mode it is meant to be
    // played in and the one the tube was drawn for, so it is where it starts
    // rather than somewhere F has to be pressed to get to; F still toggles,
    // which puts a window one key away. The flag is declared up here because
    // the window is created by it and the scaling check below reads it.
    bool fullscreen = true;

    SDL_Window* window = SDL_CreateWindow(
        "IDAIDAIDA", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cfg.window_w, cfg.window_h,
        SDL_WINDOW_SHOWN | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // The cursor goes with the mode, by the same rule the toggle follows.
    SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);

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
    if (!fullscreen && (out_w != cfg.window_w || out_h != cfg.window_h)) {
        SDL_Log("asked for %dx%d, drawing to %dx%d: the picture is being scaled",
                cfg.window_w, cfg.window_h, out_w, out_h);
    }

    // The whole game is laid out against cfg.window_w/h, and it draws into a
    // frame of exactly that size; the tube pass is what fits the frame to the
    // window. So fullscreen scales the picture rather than widening the field,
    // and nothing above present_screen() knows which one it is looking at.
    // The board outlives both the run and the world, so it is read once here
    // and kept beside the screen — World is what a death throws away.
    Scores scores;
    load_scores(scores);

    Screen screen;
    build_screen(screen, renderer, cfg);

    World world = make_world(cfg);

    bool running = true;
    Uint64 previous = SDL_GetPerformanceCounter();
    double accumulator = 0.0;

    // Anything that went down, for the one thing that wants a press rather than
    // a held key. It is held until a tick has actually read it rather than
    // cleared every frame: the loop is a fixed 120 Hz accumulator, so a frame
    // on a faster display can run no tick at all, and a press landing on one of
    // those would otherwise be dropped.
    bool confirmed = false;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_CONTROLLERDEVICEADDED) {
                if (!pad) {
                    pad = SDL_GameControllerOpen(event.cdevice.which);
                    if (pad) SDL_Log("controller: %s", SDL_GameControllerName(pad));
                }
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
                // `which` is an instance id on removal and a device index on
                // arrival, which is why the two are matched differently.
                if (pad && event.cdevice.which ==
                               SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                    SDL_GameControllerClose(pad);
                    pad = nullptr;
                }
            } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (event.key.keysym.scancode == SDL_SCANCODE_F) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(
                        window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    // Nothing in the game is pointed at, and fullscreen is the
                    // mode you sit back for — a pointer parked over the picture
                    // is the one thing on the glass that is not the game. It
                    // comes back with the window, where it is the desktops
                    // again and wanted for the title bar.
                    SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
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
                } else {
                    // Escape, F and R already mean something; everything else
                    // is free to be the any in `press any key`.
                    confirmed = true;
                }
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                confirmed = true;
            }
        }

        const Uint64 now = SDL_GetPerformanceCounter();
        double frame = static_cast<double>(now - previous) /
                       static_cast<double>(SDL_GetPerformanceFrequency());
        previous = now;
        accumulator += std::min(frame, kMaxFrame);

        Input in = read_input(pad);
        in.confirm = confirmed;
        bool stepped = false;
        while (accumulator >= kFixedStep) {
            stepped = true;
            step(world, cfg, in, static_cast<float>(kFixedStep));
            accumulator -= kFixedStep;
            // A run that just ended, taken and cleared here so it is posted
            // once. Only a score that lands is worth baking the board for.
            // A run that just ended, taken and cleared here so it is posted
            // once. The rank goes back to the world, which is what the count-up
            // reads to know whether it has a row to fly to. Nothing is baked:
            // the board's numbers are set from the faces at draw time.
            if (world.last_run >= 0) {
                world.last_rank = record_score(scores, world.last_run);
                world.last_run = -1;
            }
        }
        if (stepped) confirmed = false; // read by a tick, so it is spent

        render(renderer, screen, world, cfg, scores);
    }

    free_screen(screen);
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
