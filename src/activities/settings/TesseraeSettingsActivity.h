#pragma once

#ifdef CROSSINK_TESSERAE

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Configuration screen for the Tesserae dashboard sleep screen.
 *
 * Server URL, an enable toggle, the fallback sleep screen, a pairing-status
 * line and a "test now" action that performs a real fetch so the user finds
 * out here rather than at the next sleep. Modelled on OpdsSettingsActivity.
 */
class TesseraeSettingsActivity final : public Activity {
 public:
  explicit TesseraeSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TesseraeSettings", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;
  bool testPending = false;   // a test was requested; run it after the next paint
  bool previewActive = false;  // framebuffer holds a fetched frame awaiting a keypress
  std::string testMessage;    // outcome of the last test, empty when never run

  int getMenuItemCount() const;
  void handleSelection();
  void runConnectionTest();
  std::string pairingStatusText() const;
  std::string fallbackScreenText() const;
};

#endif  // CROSSINK_TESSERAE
