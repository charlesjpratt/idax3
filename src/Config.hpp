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
    float     square_speed = 260.0f;   // pixels per second
    SDL_Color square_color{0x3A, 0x3A, 0x3A, 0xFF};

    // The square closes in as the game runs: `shrink_rate` pixels off its width
    // and height every second of open play, down to `min_size`. It holds while a
    // diamond boost is up and while a star has the ball.
    float square_shrink_rate = 6.0f;
    float square_min_size    = 90.0f;

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

    // Red triangles: they cross the window through its center, from a random
    // direction, and cost the circle a hit on contact.
    float     triangle_size    = 16.0f; // circumradius, in pixels
    SDL_Color triangle_color{0xE2, 0x3A, 0x2E, 0xFF};
    float     triangle_speed   = 220.0f; // pixels per second
    float     triangle_gap_min = 7.0f;   // seconds between one and the next
    float     triangle_gap_max = 15.0f;
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
            file >> root;
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
        const nlohmann::json triangle   = sub_object(root, "triangle");

        cfg.window_w = window.value("width",  cfg.window_w);
        cfg.window_h = window.value("height", cfg.window_h);

        cfg.background_color = color_field(background, cfg.background_color);

        cfg.square_w     = square.value("width",  cfg.square_w);
        cfg.square_h     = square.value("height", cfg.square_h);
        cfg.square_speed = square.value("speed",  cfg.square_speed);
        cfg.square_color = color_field(square, cfg.square_color);
        cfg.square_shrink_rate = square.value("shrink_rate", cfg.square_shrink_rate);
        cfg.square_min_size    = square.value("min_size",    cfg.square_min_size);

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

        cfg.triangle_size    = triangle.value("size",    cfg.triangle_size);
        cfg.triangle_color   = color_field(triangle, cfg.triangle_color);
        cfg.triangle_speed   = triangle.value("speed",   cfg.triangle_speed);
        cfg.triangle_gap_min = triangle.value("gap_min", cfg.triangle_gap_min);
        cfg.triangle_gap_max = triangle.value("gap_max", cfg.triangle_gap_max);

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
    cfg.square_shrink_rate = std::max(cfg.square_shrink_rate, 0.0f);
    // The floor still has to hold the circle, and can't be bigger than the
    // square it is a floor for.
    cfg.square_min_size = std::clamp(cfg.square_min_size, cfg.circle_diameter,
                                     std::min(cfg.square_w, cfg.square_h));
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
    cfg.triangle_size    = std::max(cfg.triangle_size, 2.0f);
    // At a standstill a triangle would never cross, and never make room for the
    // next one, so keep it moving.
    cfg.triangle_speed   = std::max(cfg.triangle_speed, 10.0f);
    cfg.triangle_gap_min = std::max(cfg.triangle_gap_min, 0.0f);
    cfg.triangle_gap_max = std::max(cfg.triangle_gap_max, cfg.triangle_gap_min);
    return cfg;
}
