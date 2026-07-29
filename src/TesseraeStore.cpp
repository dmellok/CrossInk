#include "TesseraeStore.h"

#ifdef CROSSINK_TESSERAE

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>

namespace {
// Trailing slashes would produce "//api/v1/..." when concatenated. Normalise
// once at write time so every request path can assume no trailing slash.
std::string normalizeServerUrl(const std::string& url) {
  std::string normalized = url;
  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized;
}
}  // namespace

uint32_t TesseraeStore::clampPollIntervalS(const uint32_t seconds) {
  if (seconds < TESSERAE_POLL_MIN_S) return TESSERAE_POLL_MIN_S;
  if (seconds > TESSERAE_POLL_MAX_S) return TESSERAE_POLL_MAX_S;
  return seconds;
}

void TesseraeStore::toJson(JsonDocument& doc) const {
  doc["serverUrl"] = serverUrl;
  doc["deviceId"] = deviceId;
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
  doc["lastRenderId"] = lastRenderId;
  doc["pollIntervalS"] = pollIntervalS;
  doc["enabled"] = enabled;
  doc["fallbackSleepScreen"] = fallbackSleepScreen;
}

bool TesseraeStore::fromJson(JsonVariantConst doc) {
  serverUrl = normalizeServerUrl(doc["serverUrl"] | "");
  deviceId = doc["deviceId"] | "";
  lastRenderId = doc["lastRenderId"] | "";
  pollIntervalS = clampPollIntervalS(doc["pollIntervalS"] | TESSERAE_POLL_DEFAULT_S);
  enabled = doc["enabled"] | false;
  fallbackSleepScreen = doc["fallbackSleepScreen"] | 0;

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  token = obfuscation::deobfuscateFromBase64(doc["token_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::INVALID && !deviceId.empty()) {
    // A token we can't read is the same as no token: drop back to the pairing
    // flow rather than sending garbage the server will 401 on.
    LOG_ERR("TSR", "Stored Tesserae token unreadable; re-pairing required");
    token.clear();
  }

  LOG_DBG("TSR", "Loaded Tesserae config (enabled=%d paired=%d)", enabled ? 1 : 0, isPaired() ? 1 : 0);
  return true;
}

bool TesseraeStore::loadFromFile() {
  if (PersistableStore<TesseraeStore>::loadFromFile()) {
    return true;
  }
  // No file yet is the normal first-run state, not an error.
  return false;
}

bool TesseraeStore::setServerUrl(const std::string& url) {
  const std::string normalized = normalizeServerUrl(url);
  if (normalized == serverUrl) return true;
  serverUrl = normalized;
  // The old pairing belongs to the old server.
  deviceId.clear();
  token.clear();
  lastRenderId.clear();
  return saveToFile();
}

bool TesseraeStore::setEnabled(const bool value) {
  if (enabled == value) return true;
  enabled = value;
  return saveToFile();
}

bool TesseraeStore::setPairing(const std::string& id, const std::string& bearerToken) {
  if (id.empty() || bearerToken.empty()) {
    LOG_ERR("TSR", "Refusing to store empty Tesserae pairing");
    return false;
  }
  if (deviceId == id && token == bearerToken) return true;
  deviceId = id;
  token = bearerToken;
  // A new pairing invalidates any cached frame identity.
  lastRenderId.clear();
  return saveToFile();
}

bool TesseraeStore::setLastRenderId(const std::string& renderId) {
  if (lastRenderId == renderId) return true;
  lastRenderId = renderId;
  return saveToFile();
}

bool TesseraeStore::setPollIntervalS(const uint32_t seconds) {
  const uint32_t clamped = clampPollIntervalS(seconds);
  if (pollIntervalS == clamped) return true;
  pollIntervalS = clamped;
  return saveToFile();
}

bool TesseraeStore::setFallbackSleepScreen(const uint8_t mode) {
  if (fallbackSleepScreen == mode) return true;
  fallbackSleepScreen = mode;
  return saveToFile();
}

bool TesseraeStore::clearPairing() {
  if (deviceId.empty() && token.empty() && lastRenderId.empty()) return true;
  deviceId.clear();
  token.clear();
  lastRenderId.clear();
  return saveToFile();
}

#endif  // CROSSINK_TESSERAE
