# Changelog

## [1.0.0] — initial release

### Features
- X11 + PipeWire spectrum visualizer
- Mirrored display: bass centre, treble edges
- ARGB transparency via 32-bit X visual (requires compositor)
- Click-through input shape (XShape)
- Auto hide on silence, auto show when audio resumes
- Gamemode integration — suspends entirely during games
- Always-on-top mode (`-t` / `--on-top`)
- Xresources theming (`audiov.color`, `audiov.background`, `audiov.alpha`, `*.foreground` fallback)
- Double-buffered rendering (off-screen Pixmap + XCopyArea)
- Accurate `clock_nanosleep` frame pacing
- Logarithmic frequency axis with strict unique-bin assignment (no flat bass bars)
- Peak-bin selection + quadratic treble boost for even frequency response
- Per-bar gravity physics (snap up, fall with acceleration)
