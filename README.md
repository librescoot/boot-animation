# boot-animation

Renders a Lottie JSON animation to `/dev/fb0` using [ThorVG](https://www.thorvg.org/)'s software renderer. Designed as the boot splash for Librescoot's Dashboard Computer (i.MX6 Cortex-A9, 480×480 RGB565 display).

Part of the [Librescoot](https://librescoot.org/) open-source platform.

## Features

- Plays prerendered frame streams, falling back to rasterising Lottie live
- Software-rendered Lottie animation via ThorVG (no GPU required)
- Supports RGB565 and XRGB8888/ARGB8888 framebuffers, including padded row strides
- Scales animation proportionally to fit display resolution
- Loops indefinitely by default; `--once` plays once then holds the last frame
- Fade-to-black on exit (driven by SIGTERM)
- `sd_notify` integration (signals `READY=1` after the first displayed frame)
- Animation selectable via kernel command line (`boot.animation=name`)

## Dependencies

- [ThorVG](https://github.com/thorvg/thorvg) with C API bindings (`-lthorvg`)
- zlib (`-lz`)
- `libc`, `libstdc++`, `libm`, `libpthread`

ThorVG must be built with Lottie support enabled.

## Prerendered streams

Rasterising Lottie is expensive. On the DBC a 480×480 frame costs around 80 ms
against a 40 ms budget, so the animation runs at roughly half speed and eats CPU
that the dashboard needs to start. Packing the frames at build time turns each
one into a decompress and a memcpy: about 6 ms idle, 12 ms while the dashboard
is loading, and the animation keeps its intended length.

`tools/lottie2stream` renders an animation with the same ThorVG setup the player
uses and writes a `.lsba` file:

```sh
make tools
bin/lottie2stream librescoot.json 480 480 25 librescoot.lsba
bin/lottie2stream windowsxp.json 480 480 25 windowsxp.lsba --loop
```

At runtime, given `foo.json`, the player looks for `foo.lsba` beside it and uses
it when the geometry matches the framebuffer. A stream can also be passed
directly; if it fails, the player tries the sibling `foo.json`, never the binary
stream itself, as the live source. RGB565 stream rows are copied to RGB565
framebuffers or expanded to XRGB8888/ARGB8888 while copying. Missing, malformed,
truncated, or mismatched streams fall back to rendering the JSON live. If both
paths fail, the last valid framebuffer contents are preserved and the process
exits unsuccessfully.

Streams are RGB565 with each frame compressed independently. That costs almost
nothing in size against compressing the whole sequence, and it keeps runtime
memory at a single frame while letting playback skip ahead when a decode
overruns its slot. For reference, 480×480 at 25 fps: 2.5 MiB for an 8 s
animation, 0.4 MiB for a 2 s loop.

For boot-time resource safety, the reader rejects streams with more than 4096
frames, a total duration beyond `UINT32_MAX` milliseconds (about 49 days), or
more than 16 MiB of payload. Each compressed frame must also fit a conservative
zlib compression bound derived from its uncompressed RGB565 size.
These operational maxima are comfortably above the shipped streams.

## Building

### For ARM (cross-compile, production)

```sh
make build-arm
```

Requires `arm-linux-gnueabihf-gcc` in `PATH` and ThorVG built for ARM.

Override ThorVG paths if needed:

```sh
make build-arm THORVG_SRC=/path/to/thorvg THORVG_BUILD=/path/to/thorvg/builddir
```

### For host (development)

```sh
make build-host
```

### Yocto / BitBake

The Yocto recipe in `meta-librescoot` builds via `pkg-config --cflags/--libs thorvg-1` and installs the binary to `/usr/bin/boot-animation`. It also builds `lottie2stream` against `thorvg-native` and packs a stream for each shipped animation into `/usr/share/boot-animation/`.

### Host tests

The dependency-free rendering, timing, and stream-validation tests do not need
ThorVG:

```sh
make test
```

They cover RGB565 and 32-bit conversion with padded framebuffer strides,
framebuffer bounds checks, absolute deadline arithmetic, playback/fade
boundaries, fallback path derivation, and malformed or oversized streams.
Caller-supplied `CPPFLAGS`, `CFLAGS`, and `LDFLAGS` are honored, including for
sanitizer builds through this target.

## Usage

```
boot-animation <lottie.json> [--fps N] [--fade-ms N] [--once]
```

| Option | Default | Description |
|--------|---------|-------------|
| `<lottie.json>` | *(required)* | Path to the Lottie animation, or to a `.lsba` stream |
| `--fps N` | animation's native FPS | Target render frame rate; also used if the animation reports zero duration |
| `--fade-ms N` | `1000` | Fade-to-black duration in milliseconds on exit |
| `--once` | off | Play once, hold the last frame, then wait for SIGTERM |

### Exit behaviour

On receiving SIGTERM (or SIGINT):

1. The current frame is held.
2. A fade-to-black runs over `--fade-ms` milliseconds.
3. The framebuffer is cleared to black.
4. The process exits.

A second SIGTERM during the fade aborts it immediately and exits.

### sd_notify

When run as a `Type=notify` systemd service, `boot-animation` sends `READY=1` via the `NOTIFY_SOCKET` after the first frame has been copied to the framebuffer. This lets downstream units (`dbc-dispatcher`, etc.) start only after a splash frame is visible.

### Kernel command line

The systemd service selects the animation file based on the `boot.animation` kernel parameter:

```
boot.animation=librescoot   → /usr/share/boot-animation/librescoot.json (default)
boot.animation=windowsxp    → /usr/share/boot-animation/windowsxp.json
```

If the parameter is absent, `librescoot` is used. The `librescoot` animation is played in `--once` mode (holds its last frame until the dashboard takes the display over); other animations loop indefinitely. Each shipped animation has a prerendered `.lsba` stream beside its JSON.

## Systemd Integration

The service is `Type=notify` and runs in `sysinit.target` before `multi-user.target`. It unbinds the fbcon VT console (`vtcon1`) before starting to prevent the kernel text console from overwriting the framebuffer.

`dbc-dispatcher.service` has an `After=boot-animation.service` drop-in so the dashboard only starts after the boot animation signals ready.

## Framebuffer Notes

- The player accepts RGB565 (`R5:G6:B5`) and 32-bit `R8:G8:B8` layouts with either an unused high byte (XRGB8888) or 8-bit alpha (ARGB8888). Other bitfield layouts are rejected before rendering.
- Copies honor `finfo.line_length`, visible offsets, virtual geometry, and mapped-memory bounds; row padding and non-visible framebuffer memory are left untouched.
- When rasterising live, ThorVG renders ARGB8888 internally and the player converts each visible row as needed. RGB565 streams are copied row by row or expanded to the selected 32-bit layout.
- Playback, live rendering, and fades use monotonic absolute deadlines. Late work skips obsolete frame or fade slots instead of extending the animation.
- The animation is scaled uniformly (letterboxed) to fit the visible display dimensions reported by `FBIOGET_VSCREENINFO`.

## Diagnostics

Startup logs identify the framebuffer layout and whether stream or live mode was
selected. One bounded summary is emitted at shutdown with the fallback reason,
shown and skipped animation/fade frames, missed deadlines, and maximum observed
lateness. Per-frame diagnostics are not emitted.

## License

This project is dual-licensed. The source code is available under the
[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License][cc-by-nc-sa].
The maintainers reserve the right to grant separate licenses for commercial distribution; please contact the maintainers to discuss commercial licensing.

[![CC BY-NC-SA 4.0][cc-by-nc-sa-image]][cc-by-nc-sa]

[cc-by-nc-sa]: http://creativecommons.org/licenses/by-nc-sa/4.0/
[cc-by-nc-sa-image]: https://licensebuttons.net/l/by-nc-sa/4.0/88x31.png
