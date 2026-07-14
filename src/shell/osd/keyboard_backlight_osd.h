#pragma once

class KeyboardBacklightService;
class OsdOverlay;

class KeyboardBacklightOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void onBrightnessChanged(const KeyboardBacklightService& service);
  void showValue(int brightness, int maxBrightness);

private:
  OsdOverlay* m_overlay = nullptr;
  int m_lastBrightness = -1;
  int m_lastMaxBrightness = 0;
};
