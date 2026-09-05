# Bouncer_CPP

A pink circle bounces around inside a dark gray square that you push around an
800x600 light blue window.

## Build

```powershell
# Configure (first time only — fetches and builds SDL2, takes a few minutes)
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B Bouncer_CPP_build

# Build Release
& "C:\Program Files\CMake\bin\cmake.exe" --build Bouncer_CPP_build --config Release

# Run
.\Bouncer_CPP_build\Release\Bouncer_CPP.exe
```

## Controls

| Key | Action |
| --- | --- |
| Arrows / WASD | Move the square |
| R | Reload `config.json` and reset |
| Esc | Quit |

The square is clamped to the window, and the circle is always confined to the
square — drive a wall into the ball and it gets knocked away.

## config.json

Lives at the project root and is read at startup (and on **R**), so changing a
value needs no rebuild. Every field is optional; anything missing or malformed
falls back to the built-in default rather than failing the load.

```json
{
  "window":     { "width": 800, "height": 600 },
  "background": { "color": "#ADD8E6" },
  "square":     { "width": 260, "height": 260, "speed": 260, "color": "#3A3A3A" },
  "circle":     { "diameter": 48, "speed": 340, "color": "#FF69B4",
                  "start_x": 0.5, "start_y": 0.5 }
}
```

- `speed` is in pixels per second, for the square and the circle alike.
- `start_x` / `start_y` place the circle within the square at startup (and on
  **R**) as a fraction of the square's interior: `0` rests against the left/top
  wall, `1` against the right/bottom wall, `0.5` is centered. Values outside
  `[0, 1]` are clamped. They are relative rather than absolute pixels so the
  start point stays valid when you resize the square or the circle.
- Colors accept `#RRGGBB`, `#RRGGBBAA`, or the same without the `#`.
- The square is clamped to at least the circle's diameter and at most the window
  size; the circle's diameter has a 2px floor.
