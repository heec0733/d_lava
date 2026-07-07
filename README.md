# lava — Terminal Lava Effect

A single-file, dependency-free (besides libm) C program that renders an animated
lava/plasma effect directly in your terminal using 24-bit ANSI colors, with
adaptive downsampling to keep FPS stable even on huge terminal windows or
weaker CPUs.

![demo](video.mp4)

## Demo

![lava](d_lava_logo.png)

```bash
gcc -O3 -funroll-loops -o lava lava.c -lm
./lava
```

Press `Ctrl+C` to stop — it restores your cursor and terminal state on exit.

## How It Works

The color/heat at every cell is a sum of four `sin()` waves (horizontal,
vertical, diagonal, and radial-distance) evaluated through a precomputed
lookup table (`SIN_LUT`) — no transcendental math calls happen in the render
loop. The resulting heat value is mapped to one of `COLOR_STEPS` (64)
quantized RGB colors, each with its own ASCII/block glyph, also precomputed
into a lookup table (`COLOR_LUT`).

The tricky part isn't computing the frame — the LUTs make that essentially
free — it's getting the frame *out* to the terminal fast enough. On a large
window, lava noise changes color on almost every cell, so a naive
per-character escape sequence can add up to hundreds of KB per frame, more
than a terminal emulator can parse and draw 30 times a second on modest
hardware.

To keep FPS stable regardless of terminal size or CPU:

- **Background color and cursor state are set once at startup**, not per
  line per frame.
- **`RESET_SEQ` is written once at the end of the frame**, not per line.
- **One `write()` per frame** into a reused buffer — no allocations in the
  render loop.
- **Adaptive block-size downsampling**: if frames consistently miss the
  `1/FPS` time budget, the renderer starts drawing `N x N` character blocks
  as a single color instead of computing heat per cell. This cuts both the
  number of computations and — more importantly for I/O — the number of
  color transitions written out. Each block row is assembled once into a
  small row buffer and then `memcpy`'d for every row of the block's height,
  with no recomputation.
- **Self-balancing target FPS**: block size grows after a few consecutive
  slow frames, and shrinks back toward 1 (full detail) after a streak of
  frames that finish comfortably under budget. The next-frame deadline is
  computed as "previous deadline + budget," never accumulated as debt — if
  a frame runs late, the next deadline is simply reset to "now" instead of
  trying to catch up, which avoids the frame-burst effect that would
  otherwise pile more load on a struggling CPU.

## Tuning

A few `#define`s near the top of `lava.c` control behavior:

| Constant | Default | What it does |
|---|---|---|
| `TARGET_FPS` | `30.0` | Target frame rate |
| `COLOR_STEPS` | `64` | Number of quantized color levels |
| `MAX_BLOCK_SIZE` | `8` | Largest downsampling block (in characters) |
| `SLOW_STREAK_TO_GROW` | `3` | Consecutive slow frames before block size increases |
| `FAST_STREAK_TO_SHRINK` | `30` | Consecutive fast frames before block size decreases |

## Installation

Clone the repo and build with `gcc`:

```bash
git clone https://github.com/heec0733/d_lava.git
cd d_lava
gcc -O3 -funroll-loops -o lava lava.c -lm && ./lava
```

Press `Ctrl+C` to stop.
## Requirements

- A terminal emulator with 24-bit (truecolor) ANSI support.
- `gcc` (or another C compiler) and `libm`.
