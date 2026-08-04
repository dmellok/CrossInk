---
title: Tesserae Dashboard
nav_order: 16
---

# Tesserae dashboard client

Paints a server-rendered [Tesserae](https://github.com/dmellok/tesserae) dashboard as the reader's sleep screen.

This lives on the `tesserae-client` branch only and is not upstream. See the note at the top of the [README](../README.md) for why.

## How it works

The dashboard is not a wake-cycle mode. There is no timer and no background polling: the radio comes up **only** when the reader goes to sleep, either because you pressed power or because it timed out. It fetches a frame, paints it, and drops the radio again.

That means the refresh rate is exactly how often you put the reader down. It is the only shape that makes sense on a 650 mAh battery shared with actual reading, and it inherits the firmware's existing panel-controller detection and ghosting handling rather than reimplementing them.

The server does all the rendering. Tesserae composes the dashboard, dithers it, and packs it into the panel's native buffer. The firmware decodes nothing:

| Mode | Renderer | Frame | Paint |
|---|---|---|---|
| Mono | `esp32_bw_bin` | width × height / 8 | copy into the framebuffer, one FULL refresh |
| Grayscale | `esp32_gray2_bin` | width × height / 4 (4 levels) | base frame plus LSB/MSB planes, three passes |

Grayscale is the default (`-DCROSSINK_TESSERAE_GRAYSCALE`). It looks considerably better on anything with photos or shading, at the cost of double the download and three panel passes per paint. Drop the flag for the mono path.

Both formats are packed exactly the way `GfxRenderer` already reads bitmaps, so unpacking is bit manipulation and nothing more.

## Setup

1. **Save a WiFi network** under *Settings → System → WiFi Networks*. The sleep path only uses stored credentials; there is no picker on the way into sleep.
2. **Settings → System → Tesserae Dashboard**, set the **Server URL** (e.g. `http://192.168.1.50:8765`).
3. Turn **Use as sleep screen** on.
4. Press **Test now**. Expect *"Approve in Tesserae"* the first time.
5. In Tesserae: *Settings → Devices*, find the device in the Discovered strip, click **Register**. The device id is derived from the MAC, e.g. `crossink_638468`.
6. **Test now** again. It fetches a real frame and previews it full-screen; any button dismisses it.
7. **Settings → Display → Sleep Screen → Tesserae**.

Pairing is zero-touch and covers MAC auto-claim, so a reflash silently re-acquires the existing pairing.

## Settings

| Setting | Meaning |
|---|---|
| Server URL | Tesserae base URL. Changing it drops the stored pairing, since the token belongs to the server that issued it. |
| Use as sleep screen | Master enable. |
| Always fetch fresh | Force a server-side re-render on every sleep instead of a conditional request. Off by default: it costs a full download each time rather than a 304. |
| Fallback screen | What to paint when the dashboard can't be fetched. |
| Status | Not Set / Not paired / Approve in Tesserae / Paired. |
| Test now | Fetch and preview a real frame without waiting for a sleep. |
| Forget pairing | Shown only when paired. |

There is also a **Refresh Dashboard** action for the short/long power-button shortcuts (*Settings → Controls*). It discards the cached frame and sleeps, so the next fetch asks the server to re-render.

## Caching and refresh behaviour

The last painted frame is cached at `/.crosspoint/tesserae_frame.bin`. On the next sleep the client sends `If-None-Match`; an unchanged dashboard comes back `304` and repaints from cache, skipping the download entirely.

Unlike a wake-cycle client this **cannot** skip the paint on a 304. Between sleeps the panel is showing the reader UI, not the previous dashboard, so there is nothing on the glass worth preserving. The saving is the download and its radio airtime, not the refresh.

A normal sleep does not make the server re-render. `?button=refresh` (sent by *Test now*, the Refresh Dashboard shortcut, or *Always fetch fresh*) does, mapping to the server's `refresh` action. Otherwise the dashboard changes when a schedule or rotation fires server-side.

## Failure handling

Every failure falls through to the fallback sleep screen: no WiFi, unreachable server, unapproved pairing, expired token, wrong frame length, torn cache. A dashboard failure never leaves a blank or half-drawn panel.

The frame length is validated against the exact expected byte count before anything is painted, and downloads land in a temporary file that is only renamed into place once the full length has arrived, so a dropped transfer cannot replace a good cached frame with a torn one.

## Hardware support

| Device | Panel | Mono frame | Grayscale frame | Status |
|---|---|---|---|---|
| Xteink X4 | 800×480 | 48,000 B | 96,000 B | Working. Confirmed on hardware, both modes. |
| Xteink X3 | 792×528 | 52,272 B | 104,544 B | Implemented, **untested**. See below. |
| Xteink X4 Pro | 800×480 | 48,000 B | 96,000 B | Untested. Same panel and controller as the X4, so it should work. |

Frame sizes are read from the live panel rather than baked in, and the announced kind is resolved at runtime from the X3/X4 probe `HalGPIO::begin()` already does. One binary drives both, so nothing needs selecting at build time.

Server-side SKUs are `xteink_x4`, `xteink_x4_gray`, `xteink_x3`, `xteink_x3_gray` and `xteink_x4_pro`.

**On the X3 specifically:** nobody has tested this. Two things are inherited assumptions rather than measurements. `portrait_flipped` is carried over from the X4, but the X3 uses a different controller family (UC8253 / UC8279d rather than SSD1677) and its framebuffer scan origin may not match; if a dashboard renders upside-down, switch the SKU to `portrait`. And `esp32_gray2_bin` is documented as targeting UC8179-class panels in their 4-gray mode, so grayscale on an X3 is the less certain of the two. Mono is the safer starting point.

## Not built yet

- The 6-digit pairing-code path. Only zero-touch discover and MAC auto-claim are implemented.
- Touch. Tesserae's protocol supports it (the client sends a raw stroke; the server classifies the gesture and hit-tests it), but the X4 has no digitiser. Plausible on the X4 Pro.
- `next_poll_s` is clamped to 30 s–7 days and stored, but nothing schedules on it; the refresh cadence is the user's sleep habit.
- `sleep_until` / `next_sleep_s` are deliberately not published. They drive the server's smart-sync JIT render, which needs a predictable wake time, and this device wakes when a human picks it up.
- Battery impact over weeks of real use is unmeasured.

## Files

New, all behind `CROSSINK_TESSERAE`:

- `src/TesseraeStore.*` — pairing, server URL, cached render id, settings. SD-card JSON alongside the other `PersistableStore`s; the bearer token is MAC-obfuscated on disk like OPDS passwords.
- `src/TesseraeFrame.*` — download, cache and paint, shared by the sleep screen and the settings preview so they cannot drift.
- `src/network/TesseraeClient.*` — discover / frame / status on `esp_http_client`.
- `src/network/WifiAutoConnect.*` — headless connect from saved credentials.
- `src/activities/settings/TesseraeSettingsActivity.*` — the settings screen.
