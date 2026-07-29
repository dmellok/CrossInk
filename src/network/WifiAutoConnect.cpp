#include "network/WifiAutoConnect.h"

#ifdef CROSSINK_TESSERAE

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>
#include <vector>

#include "WifiCredentialStore.h"

namespace {

// Matches the connectivity check the OTA and OPDS paths use: associated is not
// enough, the station also needs a lease before a request can go out.
bool hasActiveStationWifiConnection() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

// Poll cadence while waiting for the association to complete. Long enough that
// the wait is mostly idle, short enough to not overshoot the timeout much.
constexpr uint32_t CONNECT_POLL_MS = 100;

bool waitForConnection(const uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
      // WL_CONNECTED can land a beat before DHCP; the IP is what callers need.
      if (WiFi.localIP() != IPAddress(0, 0, 0, 0)) return true;
    } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      return false;
    }
    delay(CONNECT_POLL_MS);
  }
  return false;
}

bool tryCredential(const std::string& ssid, const std::string& password, const uint32_t timeoutMs) {
  LOG_DBG("TSR", "Trying saved network %s", ssid.c_str());
  if (password.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }

  if (waitForConnection(timeoutMs)) {
    LOG_INF("TSR", "Connected to %s", ssid.c_str());
    WIFI_STORE.setLastConnectedSsid(ssid);
    return true;
  }

  WiFi.disconnect(false);
  delay(30);
  return false;
}

}  // namespace

bool WifiAutoConnect::connect(const uint32_t perNetworkTimeoutMs) {
#ifdef SIMULATOR
  // No radio to bring up: TesseraeClient's simulator path serves the frame
  // from disk, so report success and let the paint path run.
  (void)perNetworkTimeoutMs;
  return true;
#else
  if (hasActiveStationWifiConnection()) return true;

  // WifiSelectionActivity is the only other thing that loads this store, so on
  // a boot where the user never opened the WiFi screen it is still empty and
  // every saved network would look missing.
  WIFI_STORE.loadFromFile();

  const auto& credentials = WIFI_STORE.getCredentials();
  if (credentials.empty()) {
    LOG_INF("TSR", "No saved WiFi networks; skipping Tesserae fetch");
    return false;
  }

  WiFi.mode(WIFI_STA);

  // The network we used last is overwhelmingly likely to be in range, so try
  // it before burning a timeout on each of the others.
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    if (const WifiCredential* preferred = WIFI_STORE.findCredential(lastSsid)) {
      if (tryCredential(preferred->ssid, preferred->password, perNetworkTimeoutMs)) return true;
    }
  }

  for (const auto& credential : credentials) {
    if (credential.ssid == lastSsid) continue;  // already attempted above
    if (tryCredential(credential.ssid, credential.password, perNetworkTimeoutMs)) return true;
  }

  LOG_INF("TSR", "No saved network reachable");
  WiFi.mode(WIFI_OFF);
  return false;
#endif
}

void WifiAutoConnect::disconnect() {
#ifdef SIMULATOR
  return;
#else
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(true);
  delay(30);
  WiFi.mode(WIFI_OFF);
  delay(30);
#endif
}

#endif  // CROSSINK_TESSERAE
