#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

#ifdef CROSSINK_TESSERAE

// Bounds for the server-supplied poll cadence. Tesserae's own schema allows
// 30s..7d; a bad value here would mean a flat battery, so the client clamps
// locally regardless of what the server sends.
constexpr uint32_t TESSERAE_POLL_MIN_S = 30;
constexpr uint32_t TESSERAE_POLL_MAX_S = 604800;  // 7 days
constexpr uint32_t TESSERAE_POLL_DEFAULT_S = 900;

// Exact size of a native 1-bpp mono frame for the X4's 800x480 panel. The
// Tesserae esp32_bw_bin renderer emits width*height/8 bytes, MSB-first,
// bit-set = white, which is byte-identical to the CrossInk framebuffer.
constexpr size_t TESSERAE_FRAME_BYTES_MONO = 48000;

// 4-level grayscale frame from the esp32_gray2_bin renderer: width*height/4
// bytes, 4 px/byte, MSB-first, 0b00 = black .. 0b11 = white. Same packing
// GfxRenderer's own 2-bpp bitmap reader uses, so gray levels map straight onto
// its GRAYSCALE_LSB / GRAYSCALE_MSB planes.
constexpr size_t TESSERAE_FRAME_BYTES_GRAY = 96000;

#ifdef CROSSINK_TESSERAE_GRAYSCALE
constexpr bool TESSERAE_GRAYSCALE = true;
constexpr size_t TESSERAE_FRAME_BYTES = TESSERAE_FRAME_BYTES_GRAY;
#else
constexpr bool TESSERAE_GRAYSCALE = false;
constexpr size_t TESSERAE_FRAME_BYTES = TESSERAE_FRAME_BYTES_MONO;
#endif

// Last painted frame, kept so a 304 can repaint without re-downloading.
// Unlike a wake-cycle client, this sleep screen cannot simply skip the paint on
// an unchanged frame: between sleeps the panel holds the reader UI, not the
// previous dashboard, so there is nothing on the glass worth preserving. The
// cache saves the 48 KB download and its radio airtime, not the refresh.
constexpr char TESSERAE_FRAME_CACHE_PATH[] = "/.crosspoint/tesserae_frame.bin";

/**
 * Singleton holding the Tesserae pairing state and cached frame identity.
 *
 * Stored on the SD card next to the other CrossInk stores rather than in NVS,
 * matching OpdsServerStore. The bearer token is XOR-obfuscated with the
 * device MAC and base64-encoded before writing, same as OPDS passwords.
 */
class TesseraeStore : public PersistableStore<TesseraeStore> {
 private:
  std::string serverUrl;   // e.g. "http://192.168.1.50:8765" (no trailing slash)
  std::string deviceId;    // assigned by the server at registration
  std::string token;       // bearer token; plaintext in memory, obfuscated on disk
  std::string lastRenderId;  // ETag of the last painted frame
  uint32_t pollIntervalS = TESSERAE_POLL_DEFAULT_S;
  bool enabled = false;
  // When set, every sleep asks the server to re-render rather than sending a
  // conditional request. Costs a full download each time instead of a 304.
  bool alwaysFresh = false;
  // Sleep screen to paint when the dashboard can't be fetched. Held here
  // rather than in CrossPointSettings so the binary settings layout is
  // untouched; stores a CrossPointSettings::SLEEP_SCREEN_MODE value.
  uint8_t fallbackSleepScreen = 0;  // CrossPointSettings::DARK

  TesseraeStore() = default;

  friend class PersistableStore<TesseraeStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/tesserae.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool loadFromFile();

  const std::string& getServerUrl() const { return serverUrl; }
  const std::string& getDeviceId() const { return deviceId; }
  const std::string& getToken() const { return token; }
  const std::string& getLastRenderId() const { return lastRenderId; }
  uint32_t getPollIntervalS() const { return pollIntervalS; }
  bool isEnabled() const { return enabled; }
  uint8_t getFallbackSleepScreen() const { return fallbackSleepScreen; }
  bool isAlwaysFresh() const { return alwaysFresh; }

  // True once we hold everything needed to call the authenticated endpoints.
  bool isPaired() const { return !serverUrl.empty() && !deviceId.empty() && !token.empty(); }

  bool setServerUrl(const std::string& url);
  bool setEnabled(bool value);
  bool setPairing(const std::string& id, const std::string& bearerToken);
  bool setLastRenderId(const std::string& renderId);
  bool setPollIntervalS(uint32_t seconds);
  bool setFallbackSleepScreen(uint8_t mode);
  bool setAlwaysFresh(bool value);
  bool clearPairing();

  // Clamp a server-supplied cadence into the locally-enforced window.
  static uint32_t clampPollIntervalS(uint32_t seconds);
};

#define TESSERAE_STORE TesseraeStore::getInstance()

#endif  // CROSSINK_TESSERAE
