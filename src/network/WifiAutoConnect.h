#pragma once

#ifdef CROSSINK_TESSERAE

#include <cstdint>

/**
 * Blocking, headless station connect using the credentials already saved by
 * WifiSelectionActivity.
 *
 * SleepActivity cannot drive the interactive WiFi picker: ActivityManager
 * ::goToSleep() runs a single loop() iteration and expects the sleep screen to
 * be painted by the time it returns, so there is no room for an activity that
 * waits on input. This helper is the non-interactive equivalent, and is only
 * safe on a path that is already committed to sleeping.
 */
namespace WifiAutoConnect {

// Try the last-connected network first, then every other saved credential.
// Returns true once the station has an IP. A connection already up is
// reported as success without touching the radio.
bool connect(uint32_t perNetworkTimeoutMs = 8000);

// Tear the station down. HalPowerManager::startDeepSleep() also does this, but
// the paint should not share the rail with an active radio.
void disconnect();

}  // namespace WifiAutoConnect

#endif  // CROSSINK_TESSERAE
