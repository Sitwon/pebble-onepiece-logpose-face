# pebble-onepiece-logpose-face

> **Unofficial, fan-made project.** This watchface is not affiliated with,
> endorsed by, or connected to Toei Animation, Shueisha, or any One Piece
> intellectual property holder. One Piece and related marks are trademarks of
> their respective owners.

A **New World Log Pose** watchface for the Pebble Time 2 (emery) and Pebble
Round 2 (gabbro), inspired by One Piece. The whole screen is the log pose:
a wooden dome with three glass orbs — the top needle wobbles and settles like
an unstable island's magnetic field, the bottom-left orb shows the hour, and
the bottom-right orb shows the minutes.

## Building & running

```sh
pebble build                          # build for all targetPlatforms
pebble install --emulator emery       # install on the emery emulator
pebble install --phone <ip>           # install to a paired phone
```

## Target platforms

`targetPlatforms` in `package.json` controls which watches you build for. The
modern Pebble hardware is **emery** (Pebble Time 2), **gabbro** (Pebble Round
2), and **flint** (Pebble 2 Duo); the original Pebble platforms (aplite,
basalt, chalk, diorite) are included by default for backwards compatibility.

## Project layout

```
src/c/           C source for the watchface
src/pkjs/        PebbleKit JS (phone-side) source, if any
worker_src/c/    Background worker source, if any
resources/       Images, fonts, and other bundled resources
package.json     Project metadata (UUID, platforms, resources, message keys)
wscript          Build rules — usually no need to edit
```

This project is configured as a watchface (`pebble.watchapp.watchface` is
`true` in `package.json`), targeting the modern color platforms **emery**
(Pebble Time 2, 200x228) and **gabbro** (Pebble Round 2, 260x260 round).

## Documentation

Full SDK docs, tutorials, and API reference: <https://developer.repebble.com>
