#include "activities/network/TesseraeViewerActivity.h"

#ifdef CROSSINK_TESSERAE

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "TesseraeFrame.h"
#include "TesseraeStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/TesseraeClient.h"
#include "network/WifiAutoConnect.h"

void TesseraeViewerActivity::onEnter() {
  Activity::onEnter();
  TESSERAE_STORE.loadFromFile();

  if (!TESSERAE_STORE.getServerUrl().empty() && TesseraeFrame::cacheIsUsable()) {
    // Paint what we already have before touching the radio. On a device where
    // the dashboard is usually minutes old at worst, this is the common case
    // and it costs nothing.
    state = State::Painted;
  } else {
    // Nothing cached, so go and get one rather than showing an empty screen.
    fetchPending = true;
    showMessage(tr(STR_TESSERAE_TESTING));
  }
  requestUpdate();
}

void TesseraeViewerActivity::onExit() {
  Activity::onExit();
  if (tookWifiUp) {
    WifiAutoConnect::disconnect();
    tookWifiUp = false;
  }
}

void TesseraeViewerActivity::showMessage(const char* text) {
  state = State::Message;
  message = text != nullptr ? text : "";
}

// Connect, ask the server to re-render, download and paint. Mirrors the sleep
// path's forced-refresh behaviour: the request carries ?button=refresh, so what
// comes back reflects current widget data rather than whatever was last
// rendered.
void TesseraeViewerActivity::fetchAndPaint() {
  fetchPending = false;

  if (TESSERAE_STORE.getServerUrl().empty()) {
    showMessage(tr(STR_NOT_SET));
    requestUpdate();
    return;
  }

  if (!TesseraeFrame::panelIsSupported(renderer)) {
    LOG_ERR("TSR", "Panel geometry cannot take a Tesserae frame");
    showMessage(tr(STR_TESSERAE_TEST_FAILED));
    requestUpdate();
    return;
  }

  if (!WifiAutoConnect::connect()) {
    showMessage(tr(STR_TESSERAE_NO_WIFI));
    requestUpdate();
    return;
  }
  tookWifiUp = true;

  if (!TESSERAE_STORE.isPaired()) {
    const TesseraeClient::Result discovery = TesseraeClient::discover();
    if (discovery != TesseraeClient::Result::Ok) {
      LOG_INF("TSR", "Pairing incomplete: %s", TesseraeClient::resultToString(discovery));
      showMessage(discovery == TesseraeClient::Result::AwaitingApproval ? tr(STR_TESSERAE_AWAITING)
                                                                       : tr(STR_TESSERAE_TEST_FAILED));
      requestUpdate();
      return;
    }
  }

  TesseraeClient::FrameInfo frame;
  const TesseraeClient::Result result = TesseraeClient::fetchFrameInfo(frame, /*forceRefresh=*/true);
  if (result != TesseraeClient::Result::Ok) {
    LOG_INF("TSR", "No frame to show: %s", TesseraeClient::resultToString(result));
    showMessage(tr(STR_TESSERAE_TEST_FAILED));
    requestUpdate();
    return;
  }

  if (!TesseraeFrame::download(frame.url)) {
    showMessage(tr(STR_TESSERAE_TEST_FAILED));
    requestUpdate();
    return;
  }

  // Radio down before the slow refresh, same as the sleep path: the paint and
  // an active station should not share the rail.
  WifiAutoConnect::disconnect();
  tookWifiUp = false;

  TESSERAE_STORE.setLastRenderId(frame.renderId);
  LOG_INF("TSR", "Showing Tesserae frame %s", frame.renderId.c_str());
  state = State::Painted;
  requestUpdate();
}

void TesseraeViewerActivity::loop() {
  if (fetchPending) {
    // Paint the "contacting" message before blocking on the radio.
    requestUpdateAndWait();
    fetchAndPaint();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    fetchPending = true;
    showMessage(tr(STR_TESSERAE_TESTING));
    requestUpdate();
  }
}

void TesseraeViewerActivity::render(RenderLock&&) {
  if (state == State::Painted) {
    // The dashboard is the whole point, so nothing is drawn over it: no header,
    // no button hints. Painting goes through the same path the sleep screen
    // uses, so what is on screen here is exactly what a sleep would produce.
    // The screen stays powered because the user is still looking at it.
    if (TesseraeFrame::paint(renderer, /*turnOffScreen=*/false)) return;

    LOG_ERR("TSR", "Cached frame unreadable");
    showMessage(tr(STR_TESSERAE_TEST_FAILED));
    // Fall through and draw the message instead.
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_TESSERAE_DASHBOARD));
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, message.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TESSERAE_REFRESH), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // CROSSINK_TESSERAE
