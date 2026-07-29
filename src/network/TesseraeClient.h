#pragma once

#ifdef CROSSINK_TESSERAE

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Minimal Tesserae v1 device-API client.
 *
 * Speaks the subset of docs/dev/client-protocol.md that a sleep-screen paint
 * needs: zero-touch discover (path A / MAC auto-claim path C), then
 * GET /frame for metadata and an unauthenticated GET of the returned artefact
 * URL. Frames are the native 1-bpp mono .bin produced by the esp32_bw_bin
 * renderer: exactly TESSERAE_FRAME_BYTES bytes, MSB-first, bit-set = white,
 * which matches the CrossInk framebuffer byte for byte.
 *
 * Every call is blocking and expects an already-connected station interface.
 * Nothing here paints or touches the display.
 */
class TesseraeClient {
 public:
  enum class Result : uint8_t {
    Ok = 0,
    NoConfig,       // no server URL / not enabled
    NotPaired,      // discover has not yielded a token yet
    AwaitingApproval,  // server saw us; admin has not clicked Register
    NetworkError,   // DNS, connect, timeout, malformed response
    AuthError,      // 401 / 403 — token rejected
    NoFrame,        // 204: nothing rendered for this device yet
    NotModified,    // 304: cached render_id still current
    BadFrameSize,   // artefact length != TESSERAE_FRAME_BYTES
  };

  static const char* resultToString(Result result);

  struct FrameInfo {
    std::string url;
    std::string renderId;
    std::string format;
    uint16_t panelW = 0;
    uint16_t panelH = 0;
  };

  // POST /api/v1/device/discover. On success stores the pairing in
  // TesseraeStore. Returns AwaitingApproval while the admin hasn't registered
  // the device yet, which is a normal state, not a failure.
  static Result discover();

  // GET /api/v1/device/<id>/frame. Sends If-None-Match when a cached
  // render_id exists unless forceRefresh is set.
  static Result fetchFrameInfo(FrameInfo& out, bool forceRefresh = false);

  // GET <frame url> straight into dest. Requires capacity >= expectedBytes and
  // fails with BadFrameSize unless exactly expectedBytes arrive.
  static Result downloadFrame(const std::string& url, uint8_t* dest, size_t capacity, size_t expectedBytes);

  // GET <frame url> streamed to a file. Used for the 4-level grayscale frame,
  // which is 96 KB and cannot be held in RAM alongside the framebuffer; the
  // paint then reads it back a band at a time, once per plane. Writes to a
  // temporary path and renames on success so a failed transfer can never leave
  // a torn frame where a good one was.
  static Result downloadFrameToFile(const std::string& url, const char* destPath, size_t expectedBytes);

  // POST /api/v1/device/<id>/status. Reports battery / RSSI / IP / fw_version
  // so the device shows as online in Tesserae, and stores the clamped
  // next_poll_s the server sends back.
  //
  // Deliberately omits sleep_until / next_sleep_s: those drive the server's
  // smart-sync JIT render, which needs a predictable wake time. This device
  // wakes when a human picks it up, so any value we published would be a
  // fabrication and would train the scheduler on noise.
  static Result postStatus();
};

#endif  // CROSSINK_TESSERAE
