# audiov

A lightweight desktop audio visualizer for X11 + PipeWire, inspired by [cava](https://github.com/karlstav/cava).

Sits at the bottom of your screen behind all windows (or on top, optionally), reacts to whatever is playing through PipeWire, and disappears automatically when nothing is playing.

```
▁▂▄▇█▇▄▂▁  ▁▂▄▇█▇▄▂▁
  treble ↑  ↑ treble
       bass ↑
```
*(mirrored: bass in the centre, treble fans out to both edges)*

## Features

- **Mirrored spectrum** — bass in the centre, treble fans out to both edges
- **Transparent bars** — ARGB window composited over your wallpaper
- **Click-through** — pointer events pass through; you can still click windows underneath
- **Auto hide/show** — disappears after a configurable silence timeout, reappears when audio resumes
- **Gamemode aware** — fully suspends when a game is running ([gamemode](https://github.com/FeralInteractive/gamemode))
- **Always-on-top mode** — optional `-t` / `--on-top` flag
- **Xresources theming** — colour and alpha driven by your existing colorscheme
- **Low CPU** — accurate `clock_nanosleep` frame pacing, FFT skipped when hidden, X11 calls throttled

## Dependencies

| Package | Arch Linux | Ubuntu / Debian | Fedora |
|---|---|---|---|
| PipeWire | `pipewire` | `libpipewire-0.3-dev` | `pipewire-devel` |
| X11 | `libx11` | `libx11-dev` | `libX11-devel` |
| Xext (shape) | `libxext` | `libxext-dev` | `libXext-devel` |
| Gamemode *(optional)* | `gamemode` | `gamemode` | `gamemode-devel` |

A compositor is required for transparency — [picom](https://github.com/yshui/picom), kwin, mutter, xcompmgr, etc.

## Install

### Arch Linux

```bash
sudo pacman -S pipewire libx11 libxext gamemode picom

git clone https://github.com/yourusername/audiov
cd audiov
make
make install   # → ~/.local/bin/audiov
```

### Ubuntu / Debian

```bash
sudo apt install build-essential libpipewire-0.3-dev libx11-dev libxext-dev pkg-config

git clone https://github.com/yourusername/audiov
cd audiov
make
make install
```

### Fedora

```bash
sudo dnf install gcc make pipewire-devel libX11-devel libXext-devel pkgconf

git clone https://github.com/yourusername/audiov
cd audiov
make
make install
```

## Usage

```
audiov [options]

  -t, --on-top     float above all other windows
      --no-on-top  force background mode (overrides CFG_ON_TOP compile default)
```

```bash
audiov          # background mode — sits behind all windows (default)
audiov -t       # always on top
```

Press **Ctrl-C** to quit.

### Autostart

```bash
# ~/.xprofile or ~/.xinitrc
picom &         # compositor (needed for transparency)
audiov &
```

## Configuration

All options live at the top of `audiov.c`. Edit and rebuild with `make`.

### Layout & behaviour

| Define | Default | Description |
|---|---|---|
| `CFG_NUM_BARS` | `100` | Number of frequency bars |
| `CFG_WIN_HEIGHT` | `200` | Visualizer height in pixels |
| `CFG_MARGIN_X` | `0` | Left/right margin from screen edge |
| `CFG_MARGIN_Y` | `0` | Gap from the bottom of the screen |
| `CFG_BAR_GAP` | `1` | Pixel gap between bars |
| `CFG_FPS` | `60` | Render framerate |
| `CFG_IDLE_SECS` | `3` | Seconds of silence before hiding |
| `CFG_SILENCE_RMS` | `0.0004` | RMS level below which counts as silence |
| `CFG_ON_TOP` | `0` | Default layer: `0`=behind, `1`=on top |

### Animation

| Define | Default | Description |
|---|---|---|
| `CFG_RISE_SPEED` | `0.85` | Bar rise speed (0–1, higher = snappier) |
| `CFG_GRAVITY` | `0.0018` | Bar fall acceleration per frame |
| `CFG_SCALE` | `220.0` | Amplitude multiplier (raise if bars are too short) |
| `CFG_FFT_SIZE` | `4096` | FFT window size — larger = more bass resolution |

### Colour (compile-time defaults)

| Define | Default | Description |
|---|---|---|
| `COL_BAR_A` | `0xb3` | Bar alpha (~70% opaque) |
| `COL_BAR_R/G/B` | `0xff/0x00/0x00` | Bar colour (red) |
| `COL_BG_A` | `0x00` | Background alpha (fully transparent) |

## Xresources theming

audiov reads from `~/.Xresources` on startup. Colours set here override the compile-time defaults without needing a recompile.

```xresources
! Bar colour — #rrggbb or #aarrggbb (aa = alpha, 00=transparent ff=opaque)
audiov.color:       #b3ff0000

! Or inherit your terminal foreground automatically
*.foreground:       #00e5ff

! Background wash behind the bars
audiov.background:  #00000000

! Background alpha alone (0–255)
audiov.alpha:       0
```

Apply without rebooting:
```bash
xrdb -merge ~/.Xresources && pkill audiov; audiov &
```

### Colorscheme presets

```xresources
! Nord
audiov.color: #b388c0f0

! Gruvbox orange
audiov.color: #b3fe8019

! Catppuccin Mocha — mauve
audiov.color: #b3cba6f5

! Tokyo Night — blue
audiov.color: #b37aa2f1

! Dracula — pink
audiov.color: #b3ff79c6
```

## Troubleshooting

**Window is invisible / no transparency**
Start your compositor before audiov:
```bash
picom &
audiov &
```

**"no 32-bit ARGB visual found"**
Same fix — a running compositor is required for 32-bit visuals.

**Bars not moving**
- Check PipeWire is running: `pw-top`
- Try raising `CFG_SCALE` (e.g. `400`)
- Make sure something is actually playing through PipeWire

**"pw_context_connect failed"**
```bash
systemctl --user start pipewire pipewire-pulse
```

**Flat bars in the bass**
`CFG_FFT_SIZE 4096` is already the default and should be enough. If you still see it, raise to `8192`.

**High CPU**
- Lower `CFG_FPS` to `30`
- Install `gamemode` so audiov suspends during games automatically

## How it works

1. **PipeWire capture** — connects to the default sink monitor (loopback), capturing whatever plays through your speakers without touching the microphone
2. **Ring buffer** — audio is written to a lock-protected circular buffer by the PipeWire callback thread
3. **FFT** — Cooley-Tukey with a Hann window; at 4096 samples and 44100 Hz each bin is ~10.7 Hz wide
4. **Bin mapping** — log-spaced from 40 Hz → 18 kHz; a strict-increment pass ensures every bar owns a unique FFT bin so low-frequency bars are never duplicates
5. **Treble compensation** — peak-bin selection (not averaging) per bar, plus a quadratic gain curve that boosts treble up to 3× to compensate for natural 1/f audio rolloff
6. **Physics** — bars snap up on transients (IIR rise), fall with per-bar gravity acceleration
7. **Mirror rendering** — bar `i` is drawn twice: once at `(N-1-i)` (left half, mirrored) and once at `(N+i)` (right half), giving bass at centre
8. **Double buffering** — all drawing targets an off-screen `Pixmap`; a single `XCopyArea` blit updates the window per frame
9. **Transparency** — 32-bit ARGB visual; the compositor alpha-blends the window over whatever is below it
10. **Click-through** — `XShapeCombineRectangles(ShapeInput, 0 rects)` makes the window invisible to pointer events

## License

MIT — see [LICENSE](LICENSE)
