---
title: Tesserae Dashboard
nav_order: 16
---

# Tesserae dashboard client

Paints a server-rendered [Tesserae](https://github.com/dmellok/tesserae) dashboard as the reader's sleep screen.

<p align="center">
  <img src="./images/tesserae/dashboard.png" alt="A Tesserae dashboard painted on an Xteink X4" width="300" />
</p>

This lives on the `tesserae-dev` branch only and is not upstream. See the note at the top of the [README](../README.md) for why.

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

<p align="center">
  <img src="./images/tesserae/settings.png" alt="The Tesserae Dashboard settings screen" width="300" />
</p>

> This screenshot predates the `Refresh style` row and shows seven options rather than eight. The table below is current.

| Setting | Meaning |
|---|---|
| Server URL | Tesserae base URL. Changing it drops the stored pairing, since the token belongs to the server that issued it. |
| Use as sleep screen | Master enable. |
| Always fetch fresh | Force a server-side re-render on every sleep instead of a conditional request. Off by default: it costs a full download each time rather than a 304. |
| Refresh style | What the viewer shows while it fetches. `Verbose` names each step, `Simple` shows one message, `Keep current` leaves the dashboard up until the new frame lands. |
| Fallback screen | What to paint when the dashboard can't be fetched. |
| Status | Not Set / Not paired / Approve in Tesserae / Paired. |
| Test now | Fetch and preview a real frame without waiting for a sleep. |
| Forget pairing | Shown only when paired. |

**View dashboard** opens the dashboard full-screen while the reader is awake, rather than waiting for a sleep. It paints the cached frame straight away, which costs no radio time, and only goes to the network when you ask it to.

| Button | In the viewer |
|---|---|
| Select | Re-render the current dashboard |
| Up / Down | Step backwards / forwards through a bound rotation |
| Back | Exit |

Up and Down report `left` / `right` to the server, which its default button map binds to the previous and next step of a rotation. With no rotation bound to the device the server has nothing to step and returns the current frame, so the buttons do no harm.

Two power-button shortcuts are available under *Settings → Controls*:

| Action | What it does |
|---|---|
| **View dashboard** | Opens the viewer. Cached frame paints immediately; Select refreshes, Back exits |
| **Refresh Dashboard** | Discards the cached frame and sleeps, so the next fetch asks the server to re-render |

Map either to the **short press**, whose default is `Ignore`. The long press defaults to `Sleep`, which is worth keeping.

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
| Xteink X3 | 792×528 | 52,272 B | 104,544 B | Working. Confirmed on hardware in grayscale. |
| Seeed Sticky | 800×480 | 48,000 B | 96,000 B | Firmware confirmed on hardware. Dashboard blocked on the catalog entry. |
| Xteink X4 Pro | 800×480 | 48,000 B | 96,000 B | Untested. Same panel and controller as the X4, so it should work. |

Frame sizes are read from the live panel rather than baked in. The X3 and X4 share one binary and are told apart by the probe `HalGPIO::begin()` already does, so nothing needs selecting between them at build time. The Sticky and X4 Pro are separate ESP32-S3 builds (`-e sticky`, `-e x4-pro`) and resolve their kind from the board macro instead.

Server-side SKUs are `xteink_x4`, `xteink_x4_gray`, `xteink_x3`, `xteink_x3_gray`, `xteink_x4_pro`, `seeed_sticky` and `seeed_sticky_gray`.

**On the Sticky specifically:** CrossInk itself runs on the hardware, but the dashboard cannot pair until `seeed_sticky` / `seeed_sticky_gray` land in the Tesserae catalog ([dmellok/tesserae#245](https://github.com/dmellok/tesserae/pull/245)). The panel is the same 800×480 SSD1677 as the X4 and de-link, driven through the same framebuffer path, so the packed frame is byte-identical and only the announced kind differs. The orientation in that catalog entry is inherited from `xteink_x4` rather than observed: CrossInk's Portrait transform is board-independent and both board profiles ship `NO_FLIP`, so the same 180° offset should apply. If a dashboard paints upside-down there, the entry needs `portrait` and the SDK board profile probably needs `ROTATE_180`. CrossInk's own Sticky profile also lists the SD-over-shared-SPI arbitration as inferred from the vendor demo.

**On the X3 specifically:** confirmed working in grayscale on a UC8279d unit. Two things that were inherited guesses are now measured: `portrait_flipped` is correct despite the different controller family, and `esp32_gray2_bin` drives UC8253 / UC8279d silicon even though it is written for UC8179-class panels. The X3 needs the OEM preconditioning settle pass between the base frame and the planes, which the firmware issues unconditionally since it is a no-op on the X4.

Two gaps remain. The **mono** path has not run on an X3, only grayscale; it is the same frame at half the size through a renderer already confirmed on the X4, so the risk is small but non-zero. And only the **UC8279d** production run has been seen working; earlier X3s ship a UC8253, which the firmware detects at boot and feeds the same buffer.

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
