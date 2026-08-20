#pragma once

#ifdef CROSSINK_TESSERAE

#include <cstdint>
#include <string>

class GfxRenderer;

/**
 * Download and paint of a Tesserae frame, shared by the sleep screen and the
 * settings screen's preview so both agree on the wire size, the cache and the
 * paint sequence.
 *
 * Frames always land on SD first. The 4-level grayscale frame is 96 KB and
 * cannot sit in RAM beside the framebuffer, and routing the 1-bpp frame the
 * same way costs one SD round-trip while giving the ETag cache for free.
 */
namespace TesseraeFrame {

// True when a cached frame exists that a 304 could actually be answered with.
// Requires a stored render_id too: without one there is nothing to send as
// If-None-Match, so a 304 can never arrive.
bool cacheIsUsable();

// Fetch the artefact into the cache. Validates the length against the wire
// size for the configured gamut and leaves any previous cache intact on
// failure. Returns false on any network, size or storage error.
bool download(const std::string& url);

// Paint the cached frame. Mono is a straight copy into the framebuffer plus a
// FULL refresh; grayscale is a base frame plus two plane passes, each
// re-reading the cache a band at a time. FULL matches every other sleep screen
// here and is what keeps the previous frame from ghosting through.
//
// turnOffScreen powers the panel down after the refresh, which is right on the
// way into deep sleep and wrong for the settings preview, where the user is
// about to press a button and get the list back.
//
// Returns false if the cache is missing, the wrong size, or the panel geometry
// doesn't match.
bool paint(GfxRenderer& renderer, bool turnOffScreen);

// True when the framebuffer geometry can accept a Tesserae frame at all. The
// X3's 792x528 panel packs a different number of bytes and no Tesserae SKU
// describes it yet.
bool panelIsSupported(const GfxRenderer& renderer);

}  // namespace TesseraeFrame

#endif  // CROSSINK_TESSERAE
