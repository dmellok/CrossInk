#pragma once

#ifdef CROSSINK_TESSERAE

#include <string>

#include "activities/Activity.h"

/**
 * Full-screen Tesserae dashboard viewer.
 *
 * Opens on demand rather than waiting for a sleep, so the dashboard can be
 * looked at and refreshed while the reader is awake. Paints the cached frame
 * immediately when there is one, which costs no radio time at all, and only
 * goes to the network when the user asks for a refresh.
 *
 * Fetching and painting go through TesseraeFrame, the same code the sleep
 * screen uses, so the two cannot drift on wire size, cache or paint sequence.
 */
class TesseraeViewerActivity final : public Activity {
 public:
  explicit TesseraeViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TesseraeViewer", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Painted means the framebuffer holds a dashboard, so render() must not draw
  // over it. Message means we have something to tell the user instead.
  enum class State : uint8_t { Painted, Fetching, Message };

  State state = State::Message;
  std::string message;
  bool fetchPending = false;
  bool tookWifiUp = false;
  const char* pendingButton = nullptr;  // string literal, so no ownership

  // nullptr refreshes the current step; "left" / "right" step a bound
  // rotation, which the server resolves from its own button map.
  void fetchAndPaint(const char* buttonName = nullptr);
  void showMessage(const char* text);
};

#endif  // CROSSINK_TESSERAE
