#pragma once

#include <functional>
#include <memory>

class IpcService;
class SystemBus;

namespace sdbus {
  class IProxy;
}

class KeyboardBacklightService {
public:
  using ChangeCallback = std::function<void()>;

  explicit KeyboardBacklightService(SystemBus& bus);
  ~KeyboardBacklightService();

  KeyboardBacklightService(const KeyboardBacklightService&) = delete;
  KeyboardBacklightService& operator=(const KeyboardBacklightService&) = delete;

  [[nodiscard]] int brightness() const noexcept { return m_brightness; }
  [[nodiscard]] int maxBrightness() const noexcept { return m_maxBrightness; }
  [[nodiscard]] bool available() const noexcept { return m_available; }

  void setBrightness(int value);
  void registerIpc(IpcService& ipc);
  void setChangeCallback(ChangeCallback callback);

private:
  void refreshBrightness();
  void onBrightnessChangedInternal();

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  int m_brightness = 0;
  int m_maxBrightness = 0;
  bool m_available = false;
  ChangeCallback m_changeCallback;
};
