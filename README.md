# Casio-style Watchface

An LCD/Casio-style digital watchface for the **Pebble Time 2** (`emery`, 200×228, colour).

## Features

- **7-segment LCD clock** rendered straight into the framebuffer — chamfered "on"
  segments with faint dithered "ghost" off-segments, just like a real LCD. 12h/24h
  aware, with **PM/AM** and a **Bluetooth** indicator.
- **Steps** — today's step count from the Health service.
- **Water intake** — today's total / goal, synced from the companion
  [Water Tracker](https://github.com/super3/water) app (TapSip). The watchface is a
  read-only subscriber over AppMessage; see *Water sync* below.
- **Battery** gauge.
- **Day / Date / Year** footer.
- Layered frame: a bold rounded outer border, full-width rails framing the battery
  panel, and each section drawn as its own 1px-bordered box.

The non-clock text uses Pebble's built-in **Gothic** family (labels regular, values
bold); the clock is a hand-rolled 7-segment renderer.

## Build & run

Requires the [Pebble tool](https://github.com/coredevices/pebble-tool) + SDK.

```bash
pebble build                          # build the .pbw
pebble install --emulator emery       # run in the emery emulator
pebble install --phone <watch-ip>     # install on a real watch
```

## Water sync

Water data lives in the TapSip watchapp / its Android companion
(`com.watertracker.widget`), not on this watchface — Pebble apps are sandboxed.
This face subscribes to the phone:

- It declares the same `companionApp` and mirrors TapSip's `messageKeys`
  (`TodayOz` = 10000, `GoalOz` = 10001, `RequestSync` = 10003, …).
- On launch / reconnect / every 5 min it sends `RequestSync`; the phone replies with
  `TodayOz` / `GoalOz`, which are persisted and displayed.

The phone serves it via `PebbleProtocol.PUSH_UUIDS` in the Water Tracker app, which
pushes today's snapshot to both UUIDs (TapSip + this watchface). The full contract is
in the Water Tracker repo's `PROTOCOL.md`.

## Layout

| File | Description |
|------|-------------|
| `src/c/main.c` | The whole watchface: frame/layout, 7-segment renderer, services, water AppMessage |
| `package.json` | App metadata, message keys, companion-app declaration |
| `wscript` | Pebble waf build script |

## Credits

Visual design inspired by Lignite's *Dashboard* watchface. Only the layout/border
*style* was referenced — no fonts, graphics, or code from that (paid, closed-source)
app are used here.
