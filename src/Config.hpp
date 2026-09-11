#pragma once

#include <SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

// Every tunable in the game. Defaults here are what you get if config.json is
// missing or a key is absent, so the game always runs.
struct Config {
    int window_w = 800;
    int window_h = 600;

    SDL_Color background_color{0xAD, 0xD8, 0xE6, 0xFF};

    float     square_w     = 260.0f;
    float     square_h     = 260.0f;
    // The square is driven, not teleported: `acceleration` builds speed up to
    // `speed`, and `friction` takes it away again once the keys are let go.
    // Friction well above acceleration is what makes it feel like it stops dead.
    float     square_speed        = 260.0f;  // top speed, pixels per second
    float     square_acceleration = 1400.0f; // pixels per second squared
    float     square_friction     = 4200.0f; // pixels per second squared
    SDL_Color square_color{0x7F, 0xFF, 0xD4, 0xFF};
    float     square_outline = 3.0f;   // border thickness; the square is hollow
    // How solid the frame is when it is sitting still, 1 being fully opaque.
    float     square_idle_alpha = 0.5f;

    // The square closes in as the game runs: `shrink_rate` pixels off its width
    // and height every second of open play, down to `min_size`. It holds while a
    // diamond boost is up and while a star has the ball.
    float square_shrink_rate = 6.0f;
    float square_min_size    = 90.0f;

    // The grab: holding space closes the frame in around the circle and keeps
    // it there. `close` is how long the walls take to come all the way in — and
    // to open back out when the key is let go — `warn` how long the ball takes
    // it before it starts to give, and `crush` how much longer it lasts after
    // that before the grip has it. `recharge` is the wait between one grab and
    // the next — the bar along the top of the window — which only fills once
    // the frame has let go, so a long hold is not also a free recharge.
    float squeeze_close = 0.25f;
    float squeeze_warn  = 1.6f;
    float squeeze_crush = 1.2f;
    float squeeze_recharge = 6.0f;
    // And every fill makes the next one dearer, as a multiplier on the last:
    // 1.0 is a grab that costs the same all run, 1.35 is a third again each
    // time. Like the circle's growth it stacks and only a reset takes it back.
    float squeeze_recharge_growth = 1.8f;
    // How wide the bar itself is drawn, centered along the top of the window.
    float squeeze_bar_width = 160.0f;

    float     circle_diameter = 48.0f;
    float     circle_speed    = 340.0f; // pixels per second
    SDL_Color circle_color{0xFF, 0x69, 0xB4, 0xFF};

    // How much bigger the circle gets per green diamond, as a multiplier on its
    // current size: 1.0 is no growth at all, 1.25 is a quarter wider each time.
    // The growth stacks and lasts until the game resets.
    float circle_growth_per_diamond = 1.25f;

    // The circle collides on this fraction of its drawn size, so at 0.95 the
    // pink sinks a little into a wall instead of stopping short of it. Above
    // 1.0 it collides wider than it looks.
    float circle_collider_scale = 0.95f;

    // Start position as a fraction of the square's interior: 0 = touching the
    // left/top wall, 1 = touching the right/bottom wall, 0.5 = centered. Kept
    // relative so it stays meaningful when the square or circle is resized.
    float circle_start_x = 0.5f;
    float circle_start_y = 0.5f;

    // The star: how often one shows up, and the shape of the chase it starts.
    float     star_size    = 14.0f; // outer radius, in pixels
    SDL_Color star_color{0xFF, 0xD1, 0x3B, 0xFF};
    float     star_gap_min = 12.0f; // seconds between one star and the next
    float     star_gap_max = 20.0f;
    // Clear space kept between a star and the edge of the window, on top of the
    // star's own radius. Trimmed at spawn if the window is too small for it.
    float star_edge_margin = 48.0f;
    // The beat after a star appears: the ball sits still facing the way it was
    // going, and only then does the eye turn and the chase begin.
    float star_pause      = 0.6f;
    // Fraction of `circle_speed` on the way out to a star — of the circle's top
    // speed, not of whatever it happens to be carrying, so a diamond boost does
    // not make the chase faster.
    float star_seek_speed = 0.9f;
    float star_hold       = 3.0f;   // seconds the ball is held at the star
    float star_spin_speed = 1.0f;   // revolutions per second during that hold
    // Stars hold off until this many diamonds have been eaten, the same way the
    // hazards do. 0 means they are out from the first tick.
    int   star_unlock_diamonds = 3;

    // Green diamonds keep this much clear of the window edge, on top of their
    // own size: the HUD rows live top and bottom, and a pickup pinned to a side
    // is a chore to reach. Trimmed at spawn to what a small window can spare.
    float diamond_edge_margin_x = 60.0f;
    float diamond_edge_margin_y = 42.0f;

    // The hexagon: the square's own pickup, and the only answer to the shrink.
    // Driving the square onto one wins back half the ground it has lost.
    float     hexagon_size    = 14.0f; // circumradius, in pixels
    SDL_Color hexagon_color{0x7F, 0xFF, 0xD4, 0xFF};
    float     hexagon_spin_speed = 0.12f; // revolutions per second, while it waits
    float     hexagon_grow_rate = 120.0f; // pixels per second the square opens back up
    float     hexagon_gap_min = 8.0f;  // seconds between one being taken and the next
    float     hexagon_gap_max = 16.0f;
    int       hexagon_unlock  = 7;     // diamonds eaten before any appear

    // Two red hazards, tuned apart but costing the circle the same hit. Each
    // holds off until `unlock_diamonds` have been eaten, so the opening minutes
    // can be just the circle, the square and the pickups; 0 means it is out
    // from the first tick.

    // A pair of triangles, drawn like a fast-forward button, crossing the
    // window through its center from a random direction.
    float     moving_hazard_size    = 16.0f;  // circumradius, in pixels
    SDL_Color moving_hazard_color{0xE2, 0x3A, 0x2E, 0xFF};
    float     moving_hazard_speed   = 220.0f; // pixels per second
    // A translucent strip marks the line it will take, this many seconds before
    // it sets off down it.
    float     moving_hazard_warn    = 2.0f;
    float     moving_hazard_gap_min = 7.0f;   // seconds between crossings
    float     moving_hazard_gap_max = 15.0f;
    int       moving_hazard_unlock  = 5;

    // One upright triangle planted on the field, gone again after `life`. It
    // spawns at its own size, half again, or double, evenly drawn.
    float     still_hazard_size    = 16.0f;
    SDL_Color still_hazard_color{0xE2, 0x3A, 0x2E, 0xFF};
    float     still_hazard_gap_min = 5.0f;
    float     still_hazard_gap_max = 11.0f;
    float     still_hazard_arm     = 2.0f; // harmless outline before it arms
    float     still_hazard_life    = 4.0f; // dangerous seconds after that
    int       still_hazard_unlock  = 0;

    // The tube. The field is drawn at window size and only then put on the
    // window through a curved mesh, so all of this is presentation: it changes
    // what the game looks like, never where anything is or what it touches.
    bool  crt_enabled    = true;
    float crt_curvature  = 0.10f;  // how far the glass bows; 0 is a flat panel
    float crt_scanlines  = 0.22f;  // how dark the gap between lines goes
    float crt_line_gap   = 3.0f;   // pixels from one scanline to the next
    float crt_vignette   = 0.35f;  // how far the corners fall off
    float crt_glow       = 0.45f;  // phosphor bloom, lifted off a blurred copy
    float crt_aberration = 0.003f; // red/blue split at the rim, as a fraction
                                   // of the picture — 0 at the center either way
};

namespace config_detail {

// Accepts "#RRGGBB", "RRGGBB", "#RRGGBBAA" and "RRGGBBAA". Anything else keeps
// the fallback rather than failing the whole load — a typo in one color should
// not cost you the other three.
inline SDL_Color parse_hex_color(const std::string& text, SDL_Color fallback) {
    std::string hex = text;
    if (!hex.empty() && hex.front() == '#') hex.erase(0, 1);
    if (hex.size() != 6 && hex.size() != 8) return fallback;

    unsigned long value = 0;
    for (char c : hex) {
        int digit;
        if (c >= '0' && c <= '9')      digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return fallback;
        value = (value << 4) | static_cast<unsigned long>(digit);
    }

    SDL_Color out{};
    if (hex.size() == 6) {
        out.r = static_cast<Uint8>((value >> 16) & 0xFF);
        out.g = static_cast<Uint8>((value >> 8) & 0xFF);
        out.b = static_cast<Uint8>(value & 0xFF);
        out.a = 0xFF;
    } else {
        out.r = static_cast<Uint8>((value >> 24) & 0xFF);
        out.g = static_cast<Uint8>((value >> 16) & 0xFF);
        out.b = static_cast<Uint8>((value >> 8) & 0xFF);
        out.a = static_cast<Uint8>(value & 0xFF);
    }
    return out;
}

inline nlohmann::json sub_object(const nlohmann::json& root, const char* key) {
    if (root.contains(key) && root[key].is_object()) return root[key];
    return nlohmann::json::object();
}

inline SDL_Color color_field(const nlohmann::json& obj, SDL_Color fallback) {
    if (!obj.contains("color") || !obj["color"].is_string()) return fallback;
    return parse_hex_color(obj["color"].get<std::string>(), fallback);
}

} // namespace config_detail

// config.json lives at the project root. Look there whether the game was
// started from the root (cwd hit) or double-clicked in build/Release
// (the exe-relative hits), so there is only ever one file to edit.
inline std::vector<std::string> config_search_paths() {
    std::vector<std::string> paths{"config.json"};

    if (char* base = SDL_GetBasePath()) {
        const std::string dir = base;
        SDL_free(base);
        // SDL_GetBasePath() ends with a separator; ".." hops cover the
        // build/<Config>/ nesting that CMake generators produce.
        paths.push_back(dir + "config.json");
        paths.push_back(dir + "../config.json");
        paths.push_back(dir + "../../config.json");
    }
    return paths;
}

// Returns the defaults on any failure; `loaded_from` reports the file actually
// used (empty when none was), and `error` explains a malformed file.
inline Config load_config(std::string* loaded_from = nullptr,
                          std::string* error = nullptr) {
    Config cfg;
    if (loaded_from) loaded_from->clear();
    if (error) error->clear();

    for (const std::string& path : config_search_paths()) {
        std::ifstream file(path);
        if (!file) continue;

        nlohmann::json root;
        try {
            // Parsed with comments allowed. The file is meant to be annotated
            // and retuned by hand, and a note next to a value is half of what
            // makes it tunable — strict JSON would throw the whole file out
            // over one of them.
            root = nlohmann::json::parse(file, nullptr, true, true);
        } catch (const std::exception& e) {
            if (error) *error = path + ": " + e.what();
            return cfg;
        }
        if (!root.is_object()) {
            if (error) *error = path + ": top level is not a JSON object";
            return cfg;
        }

        using namespace config_detail;
        const nlohmann::json window     = sub_object(root, "window");
        const nlohmann::json background = sub_object(root, "background");
        const nlohmann::json square     = sub_object(root, "square");
        const nlohmann::json circle     = sub_object(root, "circle");
        const nlohmann::json star       = sub_object(root, "star");
        const nlohmann::json diamond    = sub_object(root, "diamond");
        const nlohmann::json hexagon    = sub_object(root, "hexagon");
        const nlohmann::json squeeze    = sub_object(root, "squeeze");
        const nlohmann::json moving_hazard = sub_object(root, "moving_hazard");
        const nlohmann::json still_hazard  = sub_object(root, "still_hazard");
        const nlohmann::json crt           = sub_object(root, "crt");

        cfg.window_w = window.value("width",  cfg.window_w);
        cfg.window_h = window.value("height", cfg.window_h);

        cfg.background_color = color_field(background, cfg.background_color);

        cfg.square_w     = square.value("width",  cfg.square_w);
        cfg.square_h     = square.value("height", cfg.square_h);
        cfg.square_speed        = square.value("speed",        cfg.square_speed);
        cfg.square_acceleration = square.value("acceleration", cfg.square_acceleration);
        cfg.square_friction     = square.value("friction",     cfg.square_friction);
        cfg.square_color = color_field(square, cfg.square_color);
        cfg.square_outline     = square.value("outline",     cfg.square_outline);
        cfg.square_idle_alpha  = square.value("idle_alpha",  cfg.square_idle_alpha);
        cfg.square_shrink_rate = square.value("shrink_rate", cfg.square_shrink_rate);
        cfg.square_min_size    = square.value("min_size",    cfg.square_min_size);

        cfg.squeeze_close = squeeze.value("close", cfg.squeeze_close);
        cfg.squeeze_warn  = squeeze.value("warn",  cfg.squeeze_warn);
        cfg.squeeze_crush = squeeze.value("crush", cfg.squeeze_crush);
        cfg.squeeze_recharge = squeeze.value("recharge", cfg.squeeze_recharge);
        cfg.squeeze_recharge_growth =
            squeeze.value("recharge_growth", cfg.squeeze_recharge_growth);
        cfg.squeeze_bar_width = squeeze.value("bar_width", cfg.squeeze_bar_width);

        cfg.circle_diameter = circle.value("diameter", cfg.circle_diameter);
        cfg.circle_speed    = circle.value("speed",    cfg.circle_speed);
        cfg.circle_color    = color_field(circle, cfg.circle_color);
        cfg.circle_start_x  = circle.value("start_x", cfg.circle_start_x);
        cfg.circle_start_y  = circle.value("start_y", cfg.circle_start_y);
        cfg.circle_growth_per_diamond =
            circle.value("growth_per_diamond", cfg.circle_growth_per_diamond);
        cfg.circle_collider_scale =
            circle.value("collider_scale", cfg.circle_collider_scale);

        cfg.star_size       = star.value("size",       cfg.star_size);
        cfg.star_color      = color_field(star, cfg.star_color);
        cfg.star_gap_min    = star.value("gap_min",    cfg.star_gap_min);
        cfg.star_gap_max    = star.value("gap_max",    cfg.star_gap_max);
        cfg.star_edge_margin = star.value("edge_margin", cfg.star_edge_margin);
        cfg.star_pause      = star.value("pause",      cfg.star_pause);
        cfg.star_seek_speed = star.value("seek_speed", cfg.star_seek_speed);
        cfg.star_hold       = star.value("hold",       cfg.star_hold);
        cfg.star_spin_speed = star.value("spin_speed", cfg.star_spin_speed);
        cfg.star_unlock_diamonds =
            star.value("unlock_diamonds", cfg.star_unlock_diamonds);

        cfg.diamond_edge_margin_x =
            diamond.value("edge_margin_x", cfg.diamond_edge_margin_x);
        cfg.diamond_edge_margin_y =
            diamond.value("edge_margin_y", cfg.diamond_edge_margin_y);

        cfg.hexagon_size    = hexagon.value("size",    cfg.hexagon_size);
        cfg.hexagon_color   = color_field(hexagon, cfg.hexagon_color);
        cfg.hexagon_spin_speed = hexagon.value("spin_speed", cfg.hexagon_spin_speed);
        cfg.hexagon_grow_rate = hexagon.value("grow_rate", cfg.hexagon_grow_rate);
        cfg.hexagon_gap_min = hexagon.value("gap_min", cfg.hexagon_gap_min);
        cfg.hexagon_gap_max = hexagon.value("gap_max", cfg.hexagon_gap_max);
        cfg.hexagon_unlock  = hexagon.value("unlock_diamonds", cfg.hexagon_unlock);

        cfg.moving_hazard_size    = moving_hazard.value("size",    cfg.moving_hazard_size);
        cfg.moving_hazard_color   = color_field(moving_hazard, cfg.moving_hazard_color);
        cfg.moving_hazard_speed   = moving_hazard.value("speed",   cfg.moving_hazard_speed);
        cfg.moving_hazard_warn    = moving_hazard.value("warn",    cfg.moving_hazard_warn);
        cfg.moving_hazard_gap_min = moving_hazard.value("gap_min", cfg.moving_hazard_gap_min);
        cfg.moving_hazard_gap_max = moving_hazard.value("gap_max", cfg.moving_hazard_gap_max);
        cfg.moving_hazard_unlock  =
            moving_hazard.value("unlock_diamonds", cfg.moving_hazard_unlock);

        cfg.still_hazard_size    = still_hazard.value("size",    cfg.still_hazard_size);
        cfg.still_hazard_color   = color_field(still_hazard, cfg.still_hazard_color);
        cfg.still_hazard_gap_min = still_hazard.value("gap_min", cfg.still_hazard_gap_min);
        cfg.still_hazard_gap_max = still_hazard.value("gap_max", cfg.still_hazard_gap_max);
        cfg.still_hazard_arm     = still_hazard.value("arm",     cfg.still_hazard_arm);
        cfg.still_hazard_life    = still_hazard.value("life",    cfg.still_hazard_life);
        cfg.still_hazard_unlock  =
            still_hazard.value("unlock_diamonds", cfg.still_hazard_unlock);

        cfg.crt_enabled    = crt.value("enabled",    cfg.crt_enabled);
        cfg.crt_curvature  = crt.value("curvature",  cfg.crt_curvature);
        cfg.crt_scanlines  = crt.value("scanlines",  cfg.crt_scanlines);
        cfg.crt_line_gap   = crt.value("line_gap",   cfg.crt_line_gap);
        cfg.crt_vignette   = crt.value("vignette",   cfg.crt_vignette);
        cfg.crt_glow       = crt.value("glow",       cfg.crt_glow);
        cfg.crt_aberration = crt.value("aberration", cfg.crt_aberration);

        if (loaded_from) *loaded_from = path;
        break;
    }

    // Clamp to values the simulation can actually run with.
    cfg.window_w        = std::max(cfg.window_w, 160);
    cfg.window_h        = std::max(cfg.window_h, 120);
    cfg.circle_diameter = std::max(cfg.circle_diameter, 2.0f);
    cfg.square_w        = std::clamp(cfg.square_w, cfg.circle_diameter, static_cast<float>(cfg.window_w));
    cfg.square_h        = std::clamp(cfg.square_h, cfg.circle_diameter, static_cast<float>(cfg.window_h));
    cfg.square_speed    = std::max(cfg.square_speed, 0.0f);
    cfg.square_acceleration = std::max(cfg.square_acceleration, 0.0f);
    cfg.square_friction     = std::max(cfg.square_friction, 0.0f);
    // A border thicker than half the square would close it over entirely.
    cfg.square_outline = std::clamp(cfg.square_outline, 1.0f,
                                    std::min(cfg.square_w, cfg.square_h) * 0.5f);
    cfg.square_idle_alpha  = std::clamp(cfg.square_idle_alpha, 0.0f, 1.0f);
    cfg.square_shrink_rate = std::max(cfg.square_shrink_rate, 0.0f);
    // The floor still has to hold the circle, and can't be bigger than the
    // square it is a floor for.
    cfg.square_min_size = std::clamp(cfg.square_min_size, cfg.circle_diameter,
                                     std::min(cfg.square_w, cfg.square_h));
    // At zero the frame would snap shut and back open with no travel to read,
    // and the grab is meant to be something you watch closing.
    cfg.squeeze_close = std::max(cfg.squeeze_close, 0.02f);
    cfg.squeeze_warn  = std::max(cfg.squeeze_warn, 0.0f);
    // Long enough that the shake is a warning rather than an announcement.
    cfg.squeeze_crush = std::max(cfg.squeeze_crush, 0.1f);
    // The bar is divided by this, and a grab with no wait behind it is no cost.
    cfg.squeeze_recharge = std::max(cfg.squeeze_recharge, 0.1f);
    // Below 1.0 a grab would get cheaper the more it was used, which is not
    // what it is for; the ceiling keeps a steep one from running away.
    cfg.squeeze_recharge_growth = std::clamp(cfg.squeeze_recharge_growth, 1.0f, 4.0f);
    // Wide enough that the fill is still readable, never wider than the window.
    cfg.squeeze_bar_width =
        std::clamp(cfg.squeeze_bar_width, 20.0f, static_cast<float>(cfg.window_w));
    cfg.circle_speed    = std::max(cfg.circle_speed, 0.0f);
    cfg.circle_start_x  = std::clamp(cfg.circle_start_x, 0.0f, 1.0f);
    cfg.circle_start_y  = std::clamp(cfg.circle_start_y, 0.0f, 1.0f);
    // Below 1.0 a diamond would shrink the circle, which is not what it is for.
    cfg.circle_growth_per_diamond = std::max(cfg.circle_growth_per_diamond, 1.0f);
    // A vanishing collider would tunnel straight through the walls.
    cfg.circle_collider_scale = std::clamp(cfg.circle_collider_scale, 0.05f, 2.0f);
    cfg.star_size       = std::max(cfg.star_size, 2.0f);
    cfg.star_gap_min    = std::max(cfg.star_gap_min, 0.0f);
    cfg.star_gap_max    = std::max(cfg.star_gap_max, cfg.star_gap_min); // min wins ties
    cfg.star_edge_margin = std::max(cfg.star_edge_margin, 0.0f);
    cfg.star_pause      = std::max(cfg.star_pause, 0.0f);
    // At zero the ball would never cover the ground to the star, so keep a floor.
    cfg.star_seek_speed = std::clamp(cfg.star_seek_speed, 0.05f, 4.0f);
    cfg.star_hold       = std::max(cfg.star_hold, 0.0f);
    cfg.star_unlock_diamonds = std::max(cfg.star_unlock_diamonds, 0);
    cfg.diamond_edge_margin_x = std::max(cfg.diamond_edge_margin_x, 0.0f);
    cfg.diamond_edge_margin_y = std::max(cfg.diamond_edge_margin_y, 0.0f);

    cfg.hexagon_size    = std::max(cfg.hexagon_size, 2.0f);
    // At zero the square would never actually reach the size it was given.
    cfg.hexagon_grow_rate = std::max(cfg.hexagon_grow_rate, 1.0f);
    cfg.hexagon_gap_min = std::max(cfg.hexagon_gap_min, 0.0f);
    cfg.hexagon_gap_max = std::max(cfg.hexagon_gap_max, cfg.hexagon_gap_min);
    cfg.hexagon_unlock  = std::max(cfg.hexagon_unlock, 0);

    cfg.moving_hazard_size = std::max(cfg.moving_hazard_size, 2.0f);
    // At a standstill one would never cross, and never free its slot for the
    // next, so keep it moving.
    cfg.moving_hazard_speed   = std::max(cfg.moving_hazard_speed, 10.0f);
    cfg.moving_hazard_warn    = std::max(cfg.moving_hazard_warn, 0.0f);
    cfg.moving_hazard_gap_min = std::max(cfg.moving_hazard_gap_min, 0.0f);
    cfg.moving_hazard_gap_max =
        std::max(cfg.moving_hazard_gap_max, cfg.moving_hazard_gap_min);
    cfg.moving_hazard_unlock  = std::max(cfg.moving_hazard_unlock, 0);

    cfg.still_hazard_size    = std::max(cfg.still_hazard_size, 2.0f);
    cfg.still_hazard_gap_min = std::max(cfg.still_hazard_gap_min, 0.0f);
    cfg.still_hazard_gap_max =
        std::max(cfg.still_hazard_gap_max, cfg.still_hazard_gap_min);
    cfg.still_hazard_arm    = std::max(cfg.still_hazard_arm, 0.0f);
    // Long enough to be seen and steered around.
    cfg.still_hazard_life   = std::max(cfg.still_hazard_life, 0.5f);
    cfg.still_hazard_unlock = std::max(cfg.still_hazard_unlock, 0);

    // Past about a quarter the picture folds in on itself at the corners.
    cfg.crt_curvature = std::clamp(cfg.crt_curvature, 0.0f, 0.25f);
    cfg.crt_scanlines = std::clamp(cfg.crt_scanlines, 0.0f, 1.0f);
    // Two rows is the least that can hold a line and a gap, and the floor has to
    // say so: the mask is a cosine sampled once a row, so at a period of exactly
    // one every sample lands on the same point of the wave and the lines come
    // out flat — a value the file would accept while it quietly did nothing.
    cfg.crt_line_gap   = std::clamp(cfg.crt_line_gap, 2.0f, 64.0f);
    cfg.crt_vignette   = std::clamp(cfg.crt_vignette, 0.0f, 1.0f);
    // The bloom is laid down at this as an alpha, so a full lift is the most
    // light there is to add.
    cfg.crt_glow       = std::clamp(cfg.crt_glow, 0.0f, 1.0f);
    // Enough to tint an edge, never enough to tear the picture into three.
    cfg.crt_aberration = std::clamp(cfg.crt_aberration, 0.0f, 0.05f);
    return cfg;
}
