#include "network/TesseraeClient.h"

#ifdef CROSSINK_TESSERAE

#include "TesseraeStore.h"

const char* TesseraeClient::resultToString(const Result result) {
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::NoConfig:
      return "not configured";
    case Result::NotPaired:
      return "not paired";
    case Result::AwaitingApproval:
      return "awaiting approval";
    case Result::NetworkError:
      return "network error";
    case Result::AuthError:
      return "auth rejected";
    case Result::NoFrame:
      return "no frame rendered";
    case Result::NotModified:
      return "not modified";
    case Result::BadFrameSize:
      return "bad frame size";
  }
  return "unknown";
}

#ifdef SIMULATOR

// The simulator has no esp_http_client. Paint a frame dropped at
// fs_/tesserae_frame.bin instead, so the unpack/paint path and every
// fallback branch can be exercised without hardware or a live server.
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

TesseraeClient::Result TesseraeClient::discover() { return Result::Ok; }

TesseraeClient::Result TesseraeClient::fetchFrameInfo(FrameInfo& out, const bool forceRefresh) {
  out.url = "/tesserae_frame.bin";
  out.renderId = "simulator";
  out.format = "bin";
  out.panelW = 800;
  out.panelH = 480;

  // Mirror the server's conditional-request behaviour so the cache-repaint
  // path is reachable here: a matching cached render_id, absent a forced
  // refresh, is exactly when a real server would answer 304.
  if (!forceRefresh && TESSERAE_STORE.getLastRenderId() == out.renderId) {
    return Result::NotModified;
  }
  return Result::Ok;
}

TesseraeClient::Result TesseraeClient::postStatus() { return Result::Ok; }

TesseraeClient::Result TesseraeClient::downloadFrameToFile(const std::string& url, const char* destPath,
                                                           const size_t expectedBytes) {
  if (destPath == nullptr) return Result::BadFrameSize;

  HalFile in;
  if (!Storage.openFileForRead("TSR", url, in)) {
    LOG_INF("TSR", "No simulator frame at %s; falling back", url.c_str());
    return Result::NetworkError;
  }

  HalFile out;
  if (!Storage.openFileForWrite("TSR", std::string(destPath), out)) {
    in.close();
    LOG_ERR("TSR", "Could not open %s for write", destPath);
    return Result::NetworkError;
  }

  uint8_t chunk[1024];
  size_t copied = 0;
  while (copied < expectedBytes) {
    const size_t want = std::min(sizeof(chunk), expectedBytes - copied);
    const size_t read = in.read(chunk, want);
    if (read == 0) break;
    out.write(chunk, read);
    copied += read;
  }
  in.close();
  out.close();

  if (copied != expectedBytes) {
    LOG_ERR("TSR", "Simulator frame is %zu bytes, expected %zu", copied, expectedBytes);
    Storage.remove(destPath);
    return Result::BadFrameSize;
  }
  return Result::Ok;
}

TesseraeClient::Result TesseraeClient::downloadFrame(const std::string& url, uint8_t* dest, const size_t capacity,
                                                     const size_t expectedBytes) {
  if (dest == nullptr || capacity < expectedBytes) return Result::BadFrameSize;

  HalFile file;
  if (!Storage.openFileForRead("TSR", url, file)) {
    LOG_INF("TSR", "No simulator frame at %s; falling back", url.c_str());
    return Result::NetworkError;
  }
  const size_t read = file.read(dest, expectedBytes);
  file.close();

  if (read != expectedBytes) {
    LOG_ERR("TSR", "Simulator frame short read: %zu of %zu bytes", read, expectedBytes);
    return Result::BadFrameSize;
  }
  return Result::Ok;
}

#else  // !SIMULATOR

#include <ArduinoJson.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>

#include "esp_http_client.h"
#include "network/WifiPowerSaveGuard.h"

namespace {

// Server-side kind this device announces itself as. Resolved at runtime: the
// X3 and X4 ship in one binary and are told apart by HalGPIO's boot probe, and
// they need different SKUs because their panels pack different frame sizes
// (800x480 -> 48000 bytes, 792x528 -> 52272). Every variant composes portrait
// and packs at its own landscape native stride; the _gray suffix selects the
// 2-bpp renderer.
//
// Override at build time for a panel this firmware also runs on but the catalog
// describes separately, e.g. -DCROSSINK_TESSERAE_KIND='"xteink_x4_pro"'.
const char* tesseraeKind() {
#ifdef CROSSINK_TESSERAE_KIND
  return CROSSINK_TESSERAE_KIND;
#else
  if (gpio.deviceIsX3()) {
    return TESSERAE_GRAYSCALE ? "xteink_x3_gray" : "xteink_x3";
  }
  return TESSERAE_GRAYSCALE ? "xteink_x4_gray" : "xteink_x4";
#endif
}

constexpr int REQUEST_TIMEOUT_MS = 15000;

// Header buffers only; response bodies stream through esp_http_client_read().
constexpr int HTTP_RX_BUFFER = 1024;
constexpr int HTTP_TX_BUFFER = 512;

// /frame metadata is a handful of short fields. Anything larger is a
// misconfigured endpoint, not a frame envelope.
constexpr size_t MAX_JSON_RESPONSE = 2048;

std::string deviceMac() {
  const String mac = WiFi.macAddress();
  return std::string(mac.c_str());
}

// Stable per-device id derived from the MAC, so a reflash re-announces under
// the same name and the server's MAC auto-claim (path C) recognises it.
std::string defaultDeviceId() {
  const std::string mac = deviceMac();
  std::string suffix;
  suffix.reserve(6);
  for (const char c : mac) {
    if (c != ':') suffix.push_back(static_cast<char>(tolower(c)));
  }
  if (suffix.size() > 6) suffix = suffix.substr(suffix.size() - 6);
  return "crossink_" + suffix;
}

struct Response {
  int status = 0;
  std::string body;
  std::string etag;
};

// Single blocking request. When body is non-empty the request is a POST with
// that JSON payload; otherwise a GET. Response body is captured up to
// MAX_JSON_RESPONSE bytes.
bool performJsonRequest(const std::string& url, const std::string& requestBody, const char* bearerToken,
                        const char* ifNoneMatch, Response& out) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = REQUEST_TIMEOUT_MS;
  config.buffer_size = HTTP_RX_BUFFER;
  config.buffer_size_tx = HTTP_TX_BUFFER;
  config.method = requestBody.empty() ? HTTP_METHOD_GET : HTTP_METHOD_POST;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("TSR", "esp_http_client_init failed for %s", url.c_str());
    return false;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
  esp_http_client_set_header(client, "Connection", "close");
  if (!requestBody.empty()) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
  }
  if (bearerToken != nullptr && bearerToken[0] != '\0') {
    const std::string authorization = std::string("Bearer ") + bearerToken;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
  }
  if (ifNoneMatch != nullptr && ifNoneMatch[0] != '\0') {
    // The server compares against the quoted ETag form it emitted.
    const std::string quoted = std::string("\"") + ifNoneMatch + "\"";
    esp_http_client_set_header(client, "If-None-Match", quoted.c_str());
  }

  bool ok = false;
  esp_err_t err = esp_http_client_open(client, static_cast<int>(requestBody.size()));
  if (err != ESP_OK) {
    LOG_ERR("TSR", "Connect failed for %s: %s", url.c_str(), esp_err_to_name(err));
  } else {
    bool wrote = true;
    if (!requestBody.empty()) {
      const int written = esp_http_client_write(client, requestBody.data(), requestBody.size());
      wrote = written == static_cast<int>(requestBody.size());
      if (!wrote) LOG_ERR("TSR", "Short request write: %d of %zu", written, requestBody.size());
    }

    if (wrote && esp_http_client_fetch_headers(client) >= 0) {
      out.status = esp_http_client_get_status_code(client);

      char etagBuffer[80] = {};
      char* etagValue = nullptr;
      if (esp_http_client_get_header(client, "ETag", &etagValue) == ESP_OK && etagValue != nullptr) {
        strncpy(etagBuffer, etagValue, sizeof(etagBuffer) - 1);
        // Strip the surrounding quotes so callers store the bare digest.
        char* start = etagBuffer;
        if (*start == '"') start++;
        const size_t length = strlen(start);
        if (length > 0 && start[length - 1] == '"') start[length - 1] = '\0';
        out.etag = start;
        free(etagValue);
      }

      char chunk[256];
      int read = 0;
      while ((read = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
        if (out.body.size() + static_cast<size_t>(read) > MAX_JSON_RESPONSE) {
          LOG_ERR("TSR", "Response body exceeded %zu bytes", MAX_JSON_RESPONSE);
          out.body.clear();
          break;
        }
        out.body.append(chunk, static_cast<size_t>(read));
      }
      ok = read >= 0;
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

TesseraeClient::Result statusToResult(const int status) {
  if (status == 401 || status == 403) return TesseraeClient::Result::AuthError;
  if (status == 204) return TesseraeClient::Result::NoFrame;
  if (status == 304) return TesseraeClient::Result::NotModified;
  if (status < 200 || status >= 300) return TesseraeClient::Result::NetworkError;
  return TesseraeClient::Result::Ok;
}

}  // namespace

TesseraeClient::Result TesseraeClient::discover() {
  if (!TESSERAE_STORE.isEnabled() || TESSERAE_STORE.getServerUrl().empty()) return Result::NoConfig;

  WifiPowerSaveGuard powerSaveGuard;

  const std::string deviceId = TESSERAE_STORE.getDeviceId().empty() ? defaultDeviceId() : TESSERAE_STORE.getDeviceId();

  JsonDocument request;
  request["device_id"] = deviceId;
  request["kind"] = tesseraeKind();
  // Live panel dimensions, native stride rather than the portrait composition
  // size the manifest declares. The registered kind's panel block wins
  // server-side, but if this ever lands on a generic auto-provisioning path
  // instead, the renderer falls back to packing at whatever dims it was told,
  // and native keeps that fallback correctly strided. Read from the display so
  // an X3 announces 792x528 rather than the X4's 800x480.
  request["panel_w"] = display.getDisplayWidth();
  request["panel_h"] = display.getDisplayHeight();
  request["gamut"] = "mono";
  request["fw_version"] = CROSSINK_VERSION;
  request["mac"] = deviceMac();

  std::string body;
  serializeJson(request, body);

  Response response;
  const std::string url = TESSERAE_STORE.getServerUrl() + "/api/v1/device/discover";
  if (!performJsonRequest(url, body, nullptr, nullptr, response)) return Result::NetworkError;

  if (response.status < 200 || response.status >= 300) {
    LOG_ERR("TSR", "Discover failed with HTTP %d", response.status);
    return Result::NetworkError;
  }

  JsonDocument parsed;
  if (deserializeJson(parsed, response.body) != DeserializationError::Ok) {
    LOG_ERR("TSR", "Discover response was not valid JSON");
    return Result::NetworkError;
  }

  if (!(parsed["registered"] | false)) {
    LOG_INF("TSR", "Discovered; awaiting admin approval in Tesserae");
    return Result::AwaitingApproval;
  }

  const char* token = parsed["device_token"] | "";
  const char* assignedId = parsed["device_id"] | deviceId.c_str();
  if (token[0] == '\0') {
    LOG_ERR("TSR", "Server reported registered but sent no token");
    return Result::NetworkError;
  }

  if (!TESSERAE_STORE.setPairing(assignedId, token)) {
    LOG_ERR("TSR", "Failed to persist Tesserae pairing");
    return Result::NetworkError;
  }

  LOG_INF("TSR", "Paired with Tesserae as %s", assignedId);
  return Result::Ok;
}

TesseraeClient::Result TesseraeClient::fetchFrameInfo(FrameInfo& out, const bool forceRefresh) {
  if (!TESSERAE_STORE.isEnabled() || TESSERAE_STORE.getServerUrl().empty()) return Result::NoConfig;
  if (!TESSERAE_STORE.isPaired()) return Result::NotPaired;

  WifiPowerSaveGuard powerSaveGuard;

  std::string url = TESSERAE_STORE.getServerUrl() + "/api/v1/device/" + TESSERAE_STORE.getDeviceId() + "/frame";
  const std::string& cachedRenderId = TESSERAE_STORE.getLastRenderId();

  // A forced refresh is a user-initiated request for the newest frame, which is
  // exactly what the protocol's `refresh` button action means. Reporting it
  // lets the server dispatch any bound action before it picks the frame, so the
  // artefact we get back already reflects the press.
  if (forceRefresh) {
    url += "?button=refresh";
  }

  Response response;
  if (!performJsonRequest(url, "", TESSERAE_STORE.getToken().c_str(),
                          forceRefresh ? nullptr : cachedRenderId.c_str(), response)) {
    return Result::NetworkError;
  }

  const Result status = statusToResult(response.status);
  if (status != Result::Ok) {
    if (status == Result::NetworkError) LOG_ERR("TSR", "Frame request returned HTTP %d", response.status);
    return status;
  }

  JsonDocument parsed;
  if (deserializeJson(parsed, response.body) != DeserializationError::Ok) {
    LOG_ERR("TSR", "Frame response was not valid JSON");
    return Result::NetworkError;
  }

  out.url = parsed["url"] | "";
  out.format = parsed["format"] | "";
  out.panelW = parsed["panel_w"] | 0;
  out.panelH = parsed["panel_h"] | 0;
  out.renderId = parsed["render_id"] | "";
  if (out.renderId.empty()) out.renderId = response.etag;

  if (out.url.empty()) {
    LOG_ERR("TSR", "Frame response carried no artefact URL");
    return Result::NetworkError;
  }
  return Result::Ok;
}

TesseraeClient::Result TesseraeClient::downloadFrameToFile(const std::string& url, const char* destPath,
                                                           const size_t expectedBytes) {
  if (destPath == nullptr) return Result::BadFrameSize;

  WifiPowerSaveGuard powerSaveGuard;

  // Land in a temp file and rename only once the full length has arrived, so a
  // dropped connection can never replace a good cached frame with a torn one.
  const std::string tempPath = std::string(destPath) + ".part";
  Storage.remove(tempPath.c_str());

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = REQUEST_TIMEOUT_MS;
  config.buffer_size = HTTP_RX_BUFFER;
  config.buffer_size_tx = HTTP_TX_BUFFER;
  config.method = HTTP_METHOD_GET;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("TSR", "esp_http_client_init failed for artefact");
    return Result::NetworkError;
  }
  esp_http_client_set_header(client, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
  esp_http_client_set_header(client, "Connection", "close");

  Result result = Result::NetworkError;
  const esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("TSR", "Artefact connect failed: %s", esp_err_to_name(err));
  } else {
    const int contentLength = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    if (status < 200 || status >= 300) {
      LOG_ERR("TSR", "Artefact request returned HTTP %d", status);
    } else if (contentLength > 0 && static_cast<size_t>(contentLength) != expectedBytes) {
      LOG_ERR("TSR", "Frame declares %d bytes, expected %zu", contentLength, expectedBytes);
      result = Result::BadFrameSize;
    } else {
      HalFile file;
      if (!Storage.openFileForWrite("TSR", tempPath, file)) {
        LOG_ERR("TSR", "Could not open %s for write", tempPath.c_str());
      } else {
        // 1 KB matches the HTTP receive buffer, so each read maps to roughly
        // one socket read and one SD write without a large scratch allocation.
        uint8_t chunk[1024];
        size_t received = 0;
        bool failed = false;
        while (received < expectedBytes) {
          const size_t want = std::min(sizeof(chunk), expectedBytes - received);
          const int read = esp_http_client_read(client, reinterpret_cast<char*>(chunk), static_cast<int>(want));
          if (read < 0) {
            LOG_ERR("TSR", "Frame read error after %zu bytes", received);
            failed = true;
            break;
          }
          if (read == 0) break;  // peer closed
          if (file.write(chunk, static_cast<size_t>(read)) != static_cast<size_t>(read)) {
            LOG_ERR("TSR", "SD write failed at %zu bytes", received);
            failed = true;
            break;
          }
          received += static_cast<size_t>(read);
        }
        file.close();

        if (failed) {
          Storage.remove(tempPath.c_str());
        } else if (received != expectedBytes) {
          LOG_ERR("TSR", "Frame truncated: %zu of %zu bytes", received, expectedBytes);
          Storage.remove(tempPath.c_str());
          result = Result::BadFrameSize;
        } else {
          Storage.remove(destPath);
          if (Storage.rename(tempPath.c_str(), destPath)) {
            result = Result::Ok;
          } else {
            LOG_ERR("TSR", "Could not move frame into place");
            Storage.remove(tempPath.c_str());
          }
        }
      }
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return result;
}

TesseraeClient::Result TesseraeClient::postStatus() {
  if (!TESSERAE_STORE.isEnabled() || TESSERAE_STORE.getServerUrl().empty()) return Result::NoConfig;
  if (!TESSERAE_STORE.isPaired()) return Result::NotPaired;

  WifiPowerSaveGuard powerSaveGuard;

  JsonDocument request;
  const uint16_t batteryPct = powerManager.getBatteryPercentage();
  if (batteryPct <= 100) request["battery_pct"] = batteryPct;
  request["rssi"] = WiFi.RSSI();
  request["ip"] = WiFi.localIP().toString().c_str();
  request["fw_version"] = CROSSINK_VERSION;

  std::string body;
  serializeJson(request, body);

  Response response;
  const std::string url =
      TESSERAE_STORE.getServerUrl() + "/api/v1/device/" + TESSERAE_STORE.getDeviceId() + "/status";
  if (!performJsonRequest(url, body, TESSERAE_STORE.getToken().c_str(), nullptr, response)) {
    return Result::NetworkError;
  }

  const Result status = statusToResult(response.status);
  if (status != Result::Ok) {
    if (status == Result::NetworkError) LOG_ERR("TSR", "Status POST returned HTTP %d", response.status);
    return status;
  }

  JsonDocument parsed;
  if (deserializeJson(parsed, response.body) != DeserializationError::Ok) {
    // The heartbeat landed; only the response was unreadable. Not worth
    // failing the sleep over.
    LOG_INF("TSR", "Status response was not valid JSON");
    return Result::Ok;
  }

  // Clamped locally regardless of what the server sends: a bad value here would
  // otherwise be persisted and surfaced as the device's cadence. Nothing
  // schedules on it in this firmware -- the dashboard refreshes when the user
  // sleeps the reader, never on a timer -- so it is recorded for display only.
  if (parsed["next_poll_s"].is<uint32_t>()) {
    const uint32_t requested = parsed["next_poll_s"].as<uint32_t>();
    const uint32_t clamped = TesseraeStore::clampPollIntervalS(requested);
    if (clamped != requested) {
      LOG_INF("TSR", "Server next_poll_s %lu clamped to %lu", static_cast<unsigned long>(requested),
              static_cast<unsigned long>(clamped));
    }
    TESSERAE_STORE.setPollIntervalS(clamped);
  }

  return Result::Ok;
}

TesseraeClient::Result TesseraeClient::downloadFrame(const std::string& url, uint8_t* dest, const size_t capacity,
                                                     const size_t expectedBytes) {
  if (dest == nullptr || capacity < expectedBytes) {
    LOG_ERR("TSR", "Frame buffer too small: %zu < %zu", capacity, expectedBytes);
    return Result::BadFrameSize;
  }

  WifiPowerSaveGuard powerSaveGuard;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = REQUEST_TIMEOUT_MS;
  config.buffer_size = HTTP_RX_BUFFER;
  config.buffer_size_tx = HTTP_TX_BUFFER;
  config.method = HTTP_METHOD_GET;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("TSR", "esp_http_client_init failed for artefact");
    return Result::NetworkError;
  }
  esp_http_client_set_header(client, "User-Agent", "CrossInk-ESP32-" CROSSINK_VERSION);
  esp_http_client_set_header(client, "Connection", "close");

  Result result = Result::NetworkError;
  const esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("TSR", "Artefact connect failed: %s", esp_err_to_name(err));
  } else {
    const int contentLength = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    if (status < 200 || status >= 300) {
      LOG_ERR("TSR", "Artefact request returned HTTP %d", status);
    } else if (contentLength > 0 && static_cast<size_t>(contentLength) != expectedBytes) {
      // Reject on the declared length before spending airtime on the body.
      LOG_ERR("TSR", "Frame declares %d bytes, expected %zu", contentLength, expectedBytes);
      result = Result::BadFrameSize;
    } else {
      size_t received = 0;
      bool readFailed = false;
      while (received < expectedBytes) {
        const int read = esp_http_client_read(client, reinterpret_cast<char*>(dest) + received,
                                              static_cast<int>(expectedBytes - received));
        if (read < 0) {
          readFailed = true;
          break;
        }
        if (read == 0) break;  // peer closed
        received += static_cast<size_t>(read);
      }

      if (readFailed) {
        LOG_ERR("TSR", "Frame read error after %zu bytes", received);
      } else if (received != expectedBytes) {
        LOG_ERR("TSR", "Frame truncated: %zu of %zu bytes", received, expectedBytes);
        result = Result::BadFrameSize;
      } else {
        // A frame longer than expected is as wrong as a short one; one extra
        // readable byte means the server packed a different geometry.
        char extra = 0;
        if (esp_http_client_read(client, &extra, 1) > 0) {
          LOG_ERR("TSR", "Frame longer than expected %zu bytes", expectedBytes);
          result = Result::BadFrameSize;
        } else {
          result = Result::Ok;
        }
      }
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return result;
}

#endif  // SIMULATOR

#endif  // CROSSINK_TESSERAE
