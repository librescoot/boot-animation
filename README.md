# boot-animation

Renders a Lottie JSON animation to `/dev/fb0` using [ThorVG](https://www.thorvg.org/)'s software renderer. Designed as the boot splash for LibreScoot's Dashboard Computer (i.MX6 Cortex-A9, 480×480 RGB565 display).

## Features

- Software-rendered Lottie animation via ThorVG (no GPU required)
- Supports 16bpp (RGB565) and 32bpp (ARGB8888) framebuffers
- Scales animation proportionally to fit display resolution
- Loops indefinitely by default; `--once` plays once then holds the last frame
- Fade-to-black on exit (driven by SIGTERM)
- `sd_notify` integration (signals `READY=1` after the first loop completes)
- Animation selectable via kernel command line (`boot.animation=name`)

## Dependencies

- [ThorVG](https://github.com/thorvg/thorvg) with C API bindings (`-lthorvg`)
- `libc`, `libstdc++`, `libm`, `libpthread`

ThorVG must be built with Lottie support enabled.

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

The Yocto recipe in `meta-librescoot` builds via `pkg-config --cflags/--libs thorvg-1` and installs the binary to `/usr/bin/boot-animation`.

## Usage

```
boot-animation <lottie.json> [--fps N] [--fade-ms N] [--once]
```

| Option | Default | Description |
|--------|---------|-------------|
| `<lottie.json>` | *(required)* | Path to the Lottie animation file |
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

When run as a `Type=notify` systemd service, `boot-animation` sends `READY=1` via the `NOTIFY_SOCKET` after the first full animation loop completes. This lets downstream units (`dbc-dispatcher`, etc.) wait until at least one frame cycle has been displayed before starting.

### Kernel command line

The systemd service selects the animation file based on the `boot.animation` kernel parameter:

```
boot.animation=librescoot   → /usr/share/boot-animation/librescoot.json (default)
boot.animation=windowsxp    → /usr/share/boot-animation/windowsxp.json
```

If the parameter is absent, `librescoot` is used. The `librescoot` animation is played in `--once` mode (holds last frame until Flutter takes over); other animations loop indefinitely.

## Systemd Integration

The service is `Type=notify` and runs in `sysinit.target` before `multi-user.target`. It unbinds the fbcon VT console (`vtcon1`) before starting to prevent the kernel text console from overwriting the framebuffer.

`dbc-dispatcher.service` has an `After=boot-animation.service` drop-in so Flutter only starts after the boot animation signals ready.

## Framebuffer Notes

- The renderer always works in ARGB8888 internally (ThorVG requirement).
- For 16bpp displays, each rendered frame is converted to RGB565 before writing to the framebuffer.
- The animation is scaled uniformly (letterboxed) to fit the display dimensions reported by `FBIOGET_VSCREENINFO`.
