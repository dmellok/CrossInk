#include "TesseraeSettingsActivity.h"

#ifdef CROSSINK_TESSERAE

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "TesseraeStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/TesseraeClient.h"
#include "network/WifiAutoConnect.h"

namespace {
// Server URL, enable toggle, fallback screen, status, test now.
// A paired device also gets "Forget pairing" (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 5;

constexpr int ITEM_URL = 0;
constexpr int ITEM_ENABLED = 1;
constexpr int ITEM_FALLBACK = 2;
constexpr int ITEM_STATUS = 3;
constexpr int ITEM_TEST = 4;
constexpr int ITEM_UNPAIR = 5;

// Sleep screens offered as the fallback. QUICK_RESUME and TESSERAE_SLEEP are
// excluded: the first is a different sleep path entirely, the second would
// recurse.
constexpr uint8_t FALLBACK_MODES[] = {
    static_cast<uint8_t>(CrossPointSettings::DARK),
    static_cast<uint8_t>(CrossPointSettings::LIGHT),
    static_cast<uint8_t>(CrossPointSettings::CUSTOM),
    static_cast<uint8_t>(CrossPointSettings::COVER),
    static_cast<uint8_t>(CrossPointSettings::COVER_CUSTOM),
    static_cast<uint8_t>(CrossPointSettings::READING_STATS_SLEEP),
    static_cast<uint8_t>(CrossPointSettings::MINIMAL_SLEEP),
    static_cast<uint8_t>(CrossPointSettings::MINIMAL_STATS_SLEEP),
    static_cast<uint8_t>(CrossPointSettings::DASHBOARD_SLEEP),
    static_cast<uint8_t>(CrossPointSettings::BLANK),
};
constexpr size_t FALLBACK_MODE_COUNT = sizeof(FALLBACK_MODES) / sizeof(FALLBACK_MODES[0]);

StrId fallbackModeLabel(const uint8_t mode) {
  switch (mode) {
    case CrossPointSettings::LIGHT:
      return StrId::STR_LIGHT;
    case CrossPointSettings::CUSTOM:
      return StrId::STR_CUSTOM;
    case CrossPointSettings::COVER:
      return StrId::STR_COVER;
    case CrossPointSettings::COVER_CUSTOM:
      return StrId::STR_COVER_CUSTOM;
    case CrossPointSettings::READING_STATS_SLEEP:
      return StrId::STR_READING_STATS;
    case CrossPointSettings::MINIMAL_SLEEP:
      return StrId::STR_THEME_MINIMAL;
    case CrossPointSettings::MINIMAL_STATS_SLEEP:
      return StrId::STR_THEME_MINIMAL_STATS;
    case CrossPointSettings::DASHBOARD_SLEEP:
      return StrId::STR_THEME_DASHBOARD;
    case CrossPointSettings::BLANK:
      return StrId::STR_NONE_OPT;
    case CrossPointSettings::DARK:
    default:
      return StrId::STR_DARK;
  }
}
}  // namespace

int TesseraeSettingsActivity::getMenuItemCount() const {
  return TESSERAE_STORE.isPaired() ? BASE_ITEMS + 1 : BASE_ITEMS;
}

void TesseraeSettingsActivity::onEnter() {
  Activity::onEnter();
  TESSERAE_STORE.loadFromFile();
  selectedIndex = 0;
  testPending = false;
  testMessage.clear();
  requestUpdate();
}

void TesseraeSettingsActivity::loop() {
  if (testPending) {
    // Paint the "contacting" message before blocking on the radio.
    requestUpdateAndWait();
    runConnectionTest();
    return;
  }

  if (previewActive) {
    // Any button dismisses the preview and restores the settings list.
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      previewActive = false;
      mappedInput.suppressNextBackRelease();
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int menuItems = getMenuItemCount();
  buttonNavigator.onNext([this, menuItems] {
    selectedIndex = (selectedIndex + 1) % menuItems;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, menuItems] {
    selectedIndex = (selectedIndex + menuItems - 1) % menuItems;
    requestUpdate();
  });
}

std::string TesseraeSettingsActivity::pairingStatusText() const {
  if (TESSERAE_STORE.isPaired()) return std::string(tr(STR_TESSERAE_PAIRED));
  if (TESSERAE_STORE.getServerUrl().empty()) return std::string(tr(STR_NOT_SET));
  return std::string(tr(STR_TESSERAE_NOT_PAIRED));
}

std::string TesseraeSettingsActivity::fallbackScreenText() const {
  return std::string(I18N.get(fallbackModeLabel(TESSERAE_STORE.getFallbackSleepScreen())));
}

// Runs the same sequence a sleep entry would, so a green result here means the
// dashboard will paint. Stops short of painting the frame: the user is looking
// at a settings screen, not a sleep screen.
void TesseraeSettingsActivity::runConnectionTest() {
  testPending = false;

  if (TESSERAE_STORE.getServerUrl().empty()) {
    testMessage = tr(STR_NOT_SET);
    requestUpdate();
    return;
  }

  const bool alreadyConnected = WifiAutoConnect::connect();
  if (!alreadyConnected) {
    testMessage = tr(STR_TESSERAE_NO_WIFI);
    requestUpdate();
    return;
  }

  if (!TESSERAE_STORE.isPaired()) {
    const TesseraeClient::Result discovery = TesseraeClient::discover();
    if (discovery == TesseraeClient::Result::AwaitingApproval) {
      testMessage = tr(STR_TESSERAE_AWAITING);
      WifiAutoConnect::disconnect();
      requestUpdate();
      return;
    }
    if (discovery != TesseraeClient::Result::Ok) {
      testMessage = tr(STR_TESSERAE_TEST_FAILED);
      WifiAutoConnect::disconnect();
      requestUpdate();
      return;
    }
  }

  TesseraeClient::FrameInfo frame;
  const TesseraeClient::Result result = TesseraeClient::fetchFrameInfo(frame, true);

  if (result != TesseraeClient::Result::Ok) {
    WifiAutoConnect::disconnect();
    LOG_INF("TSR", "Connection test failed: %s", TesseraeClient::resultToString(result));
    testMessage = tr(STR_TESSERAE_TEST_FAILED);
    requestUpdate();
    return;
  }

  // Paint the real frame rather than just reporting success: it is the only way
  // to confirm the panel geometry and orientation without waiting for a sleep.
  // Downloading straight into the framebuffer costs no extra RAM; the settings
  // list is redrawn from scratch when the preview is dismissed.
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  const size_t bufferSize = renderer.getBufferSize();
  if (frameBuffer == nullptr || bufferSize != TESSERAE_FRAME_BYTES) {
    WifiAutoConnect::disconnect();
    LOG_ERR("TSR", "Framebuffer is %zu bytes, Tesserae frame is %zu", bufferSize, TESSERAE_FRAME_BYTES);
    testMessage = tr(STR_TESSERAE_TEST_FAILED);
    requestUpdate();
    return;
  }

  const TesseraeClient::Result download =
      TesseraeClient::downloadFrame(frame.url, frameBuffer, bufferSize, TESSERAE_FRAME_BYTES);
  WifiAutoConnect::disconnect();

  if (download != TesseraeClient::Result::Ok) {
    LOG_ERR("TSR", "Test frame download failed: %s", TesseraeClient::resultToString(download));
    testMessage = tr(STR_TESSERAE_TEST_FAILED);
    requestUpdate();
    return;
  }

  LOG_INF("TSR", "Test frame %s fetched; previewing", frame.renderId.c_str());
  testMessage = tr(STR_TESSERAE_TEST_OK);
  previewActive = true;
  requestUpdate();
}

void TesseraeSettingsActivity::handleSelection() {
  switch (static_cast<int>(selectedIndex)) {
    case ITEM_URL: {
      const std::string prefill =
          TESSERAE_STORE.getServerUrl().empty() ? "http://" : TESSERAE_STORE.getServerUrl();
      auto handler = [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto& kb = std::get<KeyboardResult>(result.data);
        const std::string entered = (kb.text == "http://" || kb.text == "https://") ? "" : kb.text;
        // setServerUrl() drops any existing pairing: the token belongs to the
        // server that issued it.
        TESSERAE_STORE.setServerUrl(entered);
        testMessage.clear();
        requestUpdate();
      };
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput,
                                                                     tr(STR_TESSERAE_SERVER_URL), prefill, 127,
                                                                     InputType::Url),
                             handler);
      break;
    }
    case ITEM_ENABLED:
      TESSERAE_STORE.setEnabled(!TESSERAE_STORE.isEnabled());
      requestUpdate();
      break;
    case ITEM_FALLBACK: {
      const uint8_t current = TESSERAE_STORE.getFallbackSleepScreen();
      size_t index = 0;
      for (size_t i = 0; i < FALLBACK_MODE_COUNT; i++) {
        if (FALLBACK_MODES[i] == current) {
          index = i;
          break;
        }
      }
      TESSERAE_STORE.setFallbackSleepScreen(FALLBACK_MODES[(index + 1) % FALLBACK_MODE_COUNT]);
      requestUpdate();
      break;
    }
    case ITEM_STATUS:
      break;  // read-only
    case ITEM_TEST:
      testPending = true;
      testMessage = tr(STR_TESSERAE_TESTING);
      requestUpdate();
      break;
    case ITEM_UNPAIR:
      if (TESSERAE_STORE.isPaired()) {
        TESSERAE_STORE.clearPairing();
        testMessage.clear();
        if (selectedIndex >= static_cast<size_t>(getMenuItemCount())) {
          selectedIndex = BASE_ITEMS - 1;
        }
        requestUpdate();
      }
      break;
    default:
      break;
  }
}

void TesseraeSettingsActivity::render(RenderLock&&) {
  if (previewActive) {
    // The framebuffer already holds the fetched frame; clearing or drawing over
    // it would destroy exactly what we want the user to look at. Same single
    // HALF refresh the sleep screen uses, so the preview matches what a sleep
    // will actually produce.
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_TESSERAE_DASHBOARD));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int menuItems = getMenuItemCount();

  static const StrId fieldNames[] = {StrId::STR_TESSERAE_SERVER_URL, StrId::STR_TESSERAE_ENABLE,
                                     StrId::STR_TESSERAE_FALLBACK, StrId::STR_TESSERAE_STATUS,
                                     StrId::STR_TESSERAE_TEST_NOW};

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, menuItems, static_cast<int>(selectedIndex),
      [](int index) {
        if (index < BASE_ITEMS) return std::string(I18N.get(fieldNames[index]));
        return std::string(tr(STR_TESSERAE_UNPAIR));
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        switch (index) {
          case ITEM_URL:
            return TESSERAE_STORE.getServerUrl().empty() ? std::string(tr(STR_NOT_SET))
                                                         : TESSERAE_STORE.getServerUrl();
          case ITEM_ENABLED:
            return TESSERAE_STORE.isEnabled() ? std::string(tr(STR_ENABLED)) : std::string(tr(STR_DISABLED));
          case ITEM_FALLBACK:
            return fallbackScreenText();
          case ITEM_STATUS:
            return pairingStatusText();
          case ITEM_TEST:
            return testMessage;
          default:
            return std::string("");
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (testPending) {
    GUI.drawPopup(renderer, tr(STR_TESSERAE_TESTING));
  }

  renderer.displayBuffer();
}

#endif  // CROSSINK_TESSERAE
