#include "system/keyboard_backlight_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "util/clamp.h"
#include "util/string_utils.h"

#include <charconv>
#include <cmath>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string_view>

namespace {

  constexpr Logger kLog("keyboard-backlight");

  const sdbus::ServiceName kUpowerBusName{"org.freedesktop.UPower"};
  const sdbus::ObjectPath kKbdBacklightPath{"/org/freedesktop/UPower/KbdBacklight"};
  constexpr auto kKbdBacklightInterface = "org.freedesktop.UPower.KbdBacklight";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  std::optional<std::string> rejectArgs(std::string_view command, const std::string& args) {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return "error: " + std::string(command) + " takes no arguments\n";
  }

  std::optional<int> parseInt(const std::string& s) {
    int value = 0;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (result.ec == std::errc() && result.ptr == s.data() + s.size()) {
      return value;
    }
    return std::nullopt;
  }

} // namespace

KeyboardBacklightService::KeyboardBacklightService(SystemBus& bus) : m_bus(bus) {
  try {
    m_proxy = sdbus::createProxy(m_bus.connection(), kUpowerBusName, kKbdBacklightPath);
    m_available = true;

    refreshBrightness();

    m_proxy->uponSignal("BrightnessChanged").onInterface(kKbdBacklightInterface).call([this](int32_t value) {
      m_brightness = util::clampOrdered<int>(value, 0, m_maxBrightness);
      onBrightnessChangedInternal();
    });

    kLog.info("keyboard backlight service active (up to {})", m_maxBrightness);
  } catch (const sdbus::Error& e) {
    kLog.warn("keyboard backlight service disabled: {}", e.what());
    m_proxy.reset();
    m_available = false;
  }
}

KeyboardBacklightService::~KeyboardBacklightService() = default;

void KeyboardBacklightService::refreshBrightness() {
  if (!m_available || m_proxy == nullptr) {
    return;
  }
  try {
    m_proxy->callMethod("GetMaxBrightness").onInterface(kKbdBacklightInterface).storeResultsTo(m_maxBrightness);
    m_proxy->callMethod("GetBrightness").onInterface(kKbdBacklightInterface).storeResultsTo(m_brightness);
    m_brightness = util::clampOrdered(m_brightness, 0, m_maxBrightness);
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to read keyboard backlight: {}", e.what());
    m_available = false;
  }
}

void KeyboardBacklightService::onBrightnessChangedInternal() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void KeyboardBacklightService::setBrightness(int value) {
  if (!m_available || m_proxy == nullptr) {
    return;
  }
  const int clamped = util::clampOrdered(value, 0, m_maxBrightness);
  try {
    m_proxy->callMethod("SetBrightness").onInterface(kKbdBacklightInterface).withArguments(clamped);
    m_brightness = clamped;
  } catch (const sdbus::Error& e) {
    kLog.warn("failed to set keyboard backlight: {}", e.what());
  }
}

void KeyboardBacklightService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void KeyboardBacklightService::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "keyboard-backlight-set",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: keyboard-backlight-set requires <value>\n";
        }
        const auto parsed = parseInt(parts[0]);
        if (!parsed.has_value() || *parsed < 0 || *parsed > 100) {
          return "error: invalid keyboard backlight value (use 0-100 for percentage)\n";
        }
        const int raw = static_cast<int>(std::round(*parsed / 100.0 * m_maxBrightness));
        setBrightness(raw);
        return "ok\n";
      },
      "keyboard-backlight-set <value>", "Set keyboard backlight level (0-100 percentage)"
  );

  ipc.registerHandler(
      "keyboard-backlight-up",
      [this](const std::string& args) -> std::string {
        if (auto err = rejectArgs("keyboard-backlight-up", args); err.has_value()) {
          return *err;
        }
        if (!m_available) {
          return "error: keyboard backlight unavailable\n";
        }
        setBrightness(m_brightness + 1);
        return "ok\n";
      },
      "keyboard-backlight-up", "Increase keyboard backlight by one level"
  );

  ipc.registerHandler(
      "keyboard-backlight-down",
      [this](const std::string& args) -> std::string {
        if (auto err = rejectArgs("keyboard-backlight-down", args); err.has_value()) {
          return *err;
        }
        if (!m_available) {
          return "error: keyboard backlight unavailable\n";
        }
        setBrightness(m_brightness - 1);
        return "ok\n";
      },
      "keyboard-backlight-down", "Decrease keyboard backlight by one level"
  );

  ipc.registerHandler(
      "keyboard-backlight-toggle",
      [this](const std::string& args) -> std::string {
        if (auto err = rejectArgs("keyboard-backlight-toggle", args); err.has_value()) {
          return *err;
        }
        if (!m_available) {
          return "error: keyboard backlight unavailable\n";
        }
        setBrightness(m_brightness > 0 ? 0 : m_maxBrightness);
        return "ok\n";
      },
      "keyboard-backlight-toggle", "Toggle keyboard backlight on/off"
  );
}
