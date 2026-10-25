#pragma once

#include <Arduino.h>

#include <functional>

class UiRuntime;

// ZX-OS JavaScript framework (Duktape, ES5.1 + typed arrays).
// One script runs at a time on the calling task; the host callbacks let the
// firmware keep Wi-Fi/UI alive and let the user abort with BACK.
namespace zxjs {

struct Host {
  // console.log / print output, one line at a time.
  std::function<void(const String &)> print;
  // Called while the script waits (delay, receive timeouts, ...).
  std::function<void()> yield;
  // Polled a few times per second; return true to abort the script.
  std::function<bool()> cancelRequested;
  // Optional: enables the ui.* module (null on headless contexts).
  UiRuntime *ui = nullptr;
};

struct Result {
  bool ok = false;
  bool cancelled = false;
  bool timedOut = false;
  String value;  // string form of the last expression (when ok)
  String error;  // message with the first stack lines (when !ok)
};

bool isBusy();

// timeoutMs == 0 disables the wall-clock limit (cancel still works).
Result runSource(const String &source,
                 const String &name,
                 const Host &host,
                 uint32_t timeoutMs);
Result runFile(const String &path, const Host &host, uint32_t timeoutMs);

}  // namespace zxjs
