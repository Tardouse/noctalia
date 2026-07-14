#include "shell/osd/keyboard_backlight_osd.h"

#include "shell/osd/osd_overlay.h"
#include "system/keyboard_backlight_service.h"

#include <string>

namespace {

  const char* kbdBacklightIcon(int brightness) {
    if (brightness <= 0) {
      return "keyboard-off";
    }
    return "keyboard";
  }

  OsdContent makeKbdBacklightContent(int brightness, int maxBrightness) {
    const float progress =
        maxBrightness > 0 ? static_cast<float>(brightness) / static_cast<float>(maxBrightness) : 0.0f;
    return OsdContent{
        .kind = OsdKind::KeyboardBacklight,
        .icon = kbdBacklightIcon(brightness),
        .value = std::to_string(maxBrightness > 0 ? (brightness * 100) / maxBrightness : 0) + "%",
        .progress = std::clamp(progress, 0.0f, 1.0f),
    };
  }

} // namespace

void KeyboardBacklightOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }

void KeyboardBacklightOsd::onBrightnessChanged(const KeyboardBacklightService& service) {
  if (!service.available()) {
    return;
  }
  const int brightness = service.brightness();
  const int maxBrightness = service.maxBrightness();

  // Only show when value actually changed
  if (brightness == m_lastBrightness && maxBrightness == m_lastMaxBrightness) {
    return;
  }

  m_lastBrightness = brightness;
  m_lastMaxBrightness = maxBrightness;

  if (m_overlay != nullptr) {
    m_overlay->show(makeKbdBacklightContent(brightness, maxBrightness));
  }
}

void KeyboardBacklightOsd::showValue(int brightness, int maxBrightness) {
  m_lastBrightness = brightness;
  m_lastMaxBrightness = maxBrightness;
  if (m_overlay != nullptr) {
    m_overlay->show(makeKbdBacklightContent(brightness, maxBrightness));
  }
}
