#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

#include "i18n.h"

struct UiEvent {
  int delta = 0;
  bool ok = false;
  bool back = false;
  bool okLong = false;
  uint8_t okCount = 0;
  uint8_t backCount = 0;
  uint8_t okLongCount = 0;
};

enum class MessengerAction : uint8_t {
  Back = 0,
  Text = 1,
  Voice = 2,
  File = 3,
  TextLong = 4,
  Refresh = 5,
};

class UiRuntime {
 public:
  UiRuntime();

  void begin();
  void tick();
  UiEvent pollInput();
  void resetInputState();
  void setOkBackBlocked(bool blocked);

  void setStatusLine(const String &line);

  void setLanguage(UiLanguage language);
  UiLanguage language() const;

  // Loads (or unloads) the Korean font pack from SD. Returns whether the
  // font is now active; on failure the UI keeps using the built-in font.
  bool setKoreanFontInstalled(bool installed, String *error = nullptr);
  bool isKoreanFontInstalled() const;

  void setTimezone(const String &tz);
  String timezone() const;
  bool syncTimezoneFromIp(String *resolvedTz = nullptr, String *error = nullptr);
  void setDisplayBrightnessPercent(uint8_t percent);
  uint8_t displayBrightnessPercent() const;

  int launcherLoop(const String &title,
                   const std::vector<String> &items,
                   int selectedIndex,
                   const std::function<void()> &backgroundTick);

  int menuLoop(const String &title,
               const std::vector<String> &items,
               int selectedIndex,
               const std::function<void()> &backgroundTick,
               const String &footer = "OK Select  BACK Exit",
               const String &subtitle = "");

  MessengerAction messengerHomeLoop(const std::vector<String> &previewLines,
                                    int selectedIndex,
                                    const std::function<void()> &backgroundTick);

  void showInfo(const String &title,
                const std::vector<String> &lines,
                const std::function<void()> &backgroundTick,
                const String &footer = "BACK: Exit");

  bool confirm(const String &title,
               const String &message,
               const std::function<void()> &backgroundTick,
               const String &confirmLabel = "Yes",
               const String &cancelLabel = "No");

  bool textInput(const String &title,
                 String &inOutValue,
                 bool mask,
                 const std::function<void()> &backgroundTick);

  bool numberWheelInput(const String &title,
                        int minValue,
                        int maxValue,
                        int step,
                        int &inOutValue,
                        const std::function<void()> &backgroundTick,
                        const String &suffix = "",
                        const std::function<void(int)> &onValueChanged = std::function<void(int)>());

  void showProgressOverlay(const String &title,
                           const String &message,
                           int percent = -1);
  void hideProgressOverlay();

  void showToast(const String &title,
                 const String &message,
                 unsigned long showMs,
                 const std::function<void()> &backgroundTick);

  void showBootSplash(const String &subtitle,
                      unsigned long durationMs,
                      const std::function<void()> &backgroundTick);

 private:
  class Impl;
  Impl *impl_;
};
