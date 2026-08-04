#include "TesseraeFrame.h"

#ifdef CROSSINK_TESSERAE

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "TesseraeStore.h"
#include "network/TesseraeClient.h"

namespace {

// Which plane of the 4-level grayscale composite a pass is building.
enum class GrayPlane : uint8_t { BwBase, Lsb, Msb };

// Expand one row of the packed 2-bpp frame into a 1-bpp plane.
//
// Level semantics match GfxRenderer's own 2-bpp bitmap reader exactly
// (0b00 black .. 0b11 white), so the plane rules are its rules:
//   BW base : white where val == 3
//   LSB     : set where val == 1
//   MSB     : set where val == 1 or val == 2
// Framebuffer polarity is bit-set = white, and each gray plane starts from a
// zeroed row, so a "set" here is a 1 bit either way.
void expandRowToPlane(const uint8_t* src, uint8_t* dstRow, const int rowBytes, const GrayPlane plane) {
  for (int byteIndex = 0; byteIndex < rowBytes; byteIndex++) {
    // Each destination byte is 8 pixels, which is 2 source bytes.
    const uint8_t s0 = src[byteIndex * 2];
    const uint8_t s1 = src[byteIndex * 2 + 1];
    uint8_t out = 0;
    for (int bit = 0; bit < 8; bit++) {
      const uint8_t packed = (bit < 4) ? s0 : s1;
      const uint8_t val = (packed >> (6 - ((bit % 4) * 2))) & 0x3;

      bool set = false;
      switch (plane) {
        case GrayPlane::BwBase:
          set = (val == 3);
          break;
        case GrayPlane::Lsb:
          set = (val == 1);
          break;
        case GrayPlane::Msb:
          set = (val == 1 || val == 2);
          break;
      }
      if (set) out |= static_cast<uint8_t>(0x80u >> bit);
    }
    dstRow[byteIndex] = out;
  }
}

// Build one plane across the whole panel from the cached 2-bpp frame. Streams
// a band at a time: the frame cannot sit in RAM next to the framebuffer, so
// each of the three passes re-reads it from SD.
bool buildGrayPlane(uint8_t* frameBuffer, const int panelWidth, const int panelHeight, const GrayPlane plane,
                    uint8_t* band, const int bandRows) {
  const int dstRowBytes = panelWidth / 8;
  const int srcRowBytes = panelWidth / 4;

  HalFile file;
  if (!Storage.openFileForRead("TSR", TESSERAE_FRAME_CACHE_PATH, file)) return false;

  bool ok = true;
  for (int y = 0; y < panelHeight; y += bandRows) {
    const int rows = std::min(bandRows, panelHeight - y);
    const size_t want = static_cast<size_t>(rows) * srcRowBytes;
    if (file.read(band, want) != want) {
      LOG_ERR("TSR", "Short read building gray plane at row %d", y);
      ok = false;
      break;
    }
    for (int row = 0; row < rows; row++) {
      expandRowToPlane(band + static_cast<size_t>(row) * srcRowBytes,
                       frameBuffer + static_cast<size_t>(y + row) * dstRowBytes, dstRowBytes, plane);
    }
  }

  file.close();
  return ok;
}

// Three-pass 4-level composite, matching renderBitmapSleepScreen()'s sequence
// for grey BMP sleep images. The base is a FULL refresh so the panel reaches
// the gray nudge pass from a clean B/W baseline: a HALF base leaves the
// previous frame's charge state behind and it shows through as ghosting.
bool paintGrayscale(GfxRenderer& renderer, uint8_t* frameBuffer, const bool turnOffScreen) {
  const int panelWidth = renderer.getDisplayWidth();
  const int panelHeight = renderer.getDisplayHeight();

  // 16 rows of an 800px panel is 3.2 KB: small against ~110 KB of headroom,
  // large enough that the frame is read in 30 chunks per pass rather than 480
  // tiny ones through the storage mutex.
  constexpr int bandRows = 16;
  const size_t bandBytes = static_cast<size_t>(bandRows) * (panelWidth / 4);
  auto band = makeUniqueNoThrow<uint8_t[]>(bandBytes);
  if (!band) {
    LOG_ERR("TSR", "Could not allocate %zu byte gray band", bandBytes);
    return false;
  }

  if (!buildGrayPlane(frameBuffer, panelWidth, panelHeight, GrayPlane::BwBase, band.get(), bandRows)) return false;
  renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);

  // X3 needs the OEM settle pass between the base frame and the planes; it is a
  // no-op on X4, so calling it unconditionally is correct for both.
  renderer.preconditionGrayscale();

  // Past this point the base frame is already on the panel, so a failure
  // leaves a readable 1-bit version rather than a broken screen.
  if (!buildGrayPlane(frameBuffer, panelWidth, panelHeight, GrayPlane::Lsb, band.get(), bandRows)) {
    LOG_ERR("TSR", "Gray LSB plane failed; leaving the base frame");
    return true;
  }
  renderer.copyGrayscaleLsbBuffers();

  if (!buildGrayPlane(frameBuffer, panelWidth, panelHeight, GrayPlane::Msb, band.get(), bandRows)) {
    LOG_ERR("TSR", "Gray MSB plane failed; leaving the base frame");
    return true;
  }
  renderer.copyGrayscaleMsbBuffers();

  // No setRenderMode() reset needed: planes are written into the framebuffer
  // directly rather than through drawPixel(), so the mode never changes.
  renderer.displayGrayBuffer(turnOffScreen);
  return true;
}

bool paintMono(GfxRenderer& renderer, uint8_t* frameBuffer, const size_t bufferSize,
               const bool turnOffScreen) {
  HalFile file;
  if (!Storage.openFileForRead("TSR", TESSERAE_FRAME_CACHE_PATH, file)) return false;
  const size_t expected = tesseraeMonoFrameBytes();
  const size_t read = file.read(frameBuffer, std::min(bufferSize, expected));
  file.close();

  if (read != expected) {
    LOG_ERR("TSR", "Cached frame is %zu bytes, expected %zu", read, expected);
    return false;
  }

  // FULL refresh, matching every other sleep screen on this branch: the panel
  // enters deep sleep from a clean baseline rather than carrying the previous
  // frame's charge state forward as ghosting.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH, turnOffScreen);
  return true;
}

}  // namespace

// Any panel whose width packs to whole bytes works: the frame is unpacked at
// the panel's own stride, so 800x480 and the X3's 792x528 are both fine. The
// width check matters because a width that is not a multiple of 8 would need
// row padding, which the wire format does not have.
bool TesseraeFrame::panelIsSupported(const GfxRenderer& renderer) {
  if (renderer.getFrameBuffer() == nullptr) return false;
  const uint16_t width = renderer.getDisplayWidth();
  return width > 0 && (width % 8) == 0 && renderer.getBufferSize() > 0;
}

bool TesseraeFrame::cacheIsUsable() {
  if (TESSERAE_STORE.getLastRenderId().empty()) return false;
  return Storage.exists(TESSERAE_FRAME_CACHE_PATH);
}

bool TesseraeFrame::download(const std::string& url) {
  const TesseraeClient::Result result =
      TesseraeClient::downloadFrameToFile(url, TESSERAE_FRAME_CACHE_PATH, tesseraeFrameBytes());
  if (result != TesseraeClient::Result::Ok) {
    LOG_ERR("TSR", "Frame download failed: %s", TesseraeClient::resultToString(result));
    return false;
  }
  return true;
}

bool TesseraeFrame::paint(GfxRenderer& renderer, const bool turnOffScreen) {
  if (!panelIsSupported(renderer)) {
    LOG_ERR("TSR", "Panel %ux%u cannot take a Tesserae frame", renderer.getDisplayWidth(),
            renderer.getDisplayHeight());
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (TESSERAE_GRAYSCALE) {
    return paintGrayscale(renderer, frameBuffer, turnOffScreen);
  }
  return paintMono(renderer, frameBuffer, renderer.getBufferSize(), turnOffScreen);
}

#endif  // CROSSINK_TESSERAE
