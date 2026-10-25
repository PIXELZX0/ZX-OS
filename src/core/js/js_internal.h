#pragma once

// Shared helpers for the JS module bindings. Not part of the public API.
//
// Duktape reports errors with longjmp, which skips C++ destructors. Every
// native binding therefore follows the same rule: do the work (Strings,
// vectors, ...) inside an inner scope, copy a failure message into an ErrBuf,
// leave the scope, and only then call throwErr().

#include <Arduino.h>

#include <vector>

extern "C" {
#include "duktape.h"
}

#include "js_engine.h"

namespace zxjs {

struct RunState {
  Host host;
  uint32_t deadlineMs = 0;  // 0 = no wall-clock limit
  uint32_t lastPollMs = 0;
  bool stop = false;
  bool cancelled = false;
  bool timedOut = false;
  bool exited = false;
  size_t memUsed = 0;
  size_t memLimit = 0;
};

RunState *state();

// Evaluates deadline / cancel button. Once true it stays true.
bool pollStop();
// Lets the firmware run its background tick, then calls pollStop().
bool yieldHost();

struct ErrBuf {
  char msg[160];
  ErrBuf() { msg[0] = 0; }
  void set(const String &text) { strlcpy(msg, text.c_str(), sizeof(msg)); }
  void set(const char *text) { strlcpy(msg, text, sizeof(msg)); }
  bool failed() const { return msg[0] != 0; }
};

inline duk_ret_t throwErr(duk_context *ctx, const ErrBuf &eb) {
  return duk_error(ctx, DUK_ERR_ERROR, "%s", eb.msg);
}

// Raises the "script stopped" error for the current stop reason.
duk_ret_t throwStop(duk_context *ctx);

// Accepts Uint8Array / ArrayBuffer views / Duktape buffers, plain arrays of
// 0..255 numbers, or a hex string (separators allowed).
bool getBytes(duk_context *ctx,
              duk_idx_t idx,
              std::vector<uint8_t> &out,
              size_t maxLen,
              String &err);
void pushBytes(duk_context *ctx, const uint8_t *data, size_t size);

// Arrays (or typed arrays) of 0..65535 numbers.
bool getTimings(duk_context *ctx,
                duk_idx_t idx,
                std::vector<uint16_t> &out,
                size_t maxLen,
                String &err);
void pushTimings(duk_context *ctx, const std::vector<uint16_t> &timings);

String bytesToHexString(const uint8_t *data, size_t size, char sep);

// Reads obj[key] as a number when present. Returns false when absent.
bool getNumberProp(duk_context *ctx,
                   duk_idx_t objIdx,
                   const char *key,
                   double &out);
bool getBoolProp(duk_context *ctx,
                 duk_idx_t objIdx,
                 const char *key,
                 bool &out);

// Registers a native function as property `name` of the object on the stack top.
void putFunction(duk_context *ctx,
                 const char *name,
                 duk_c_function fn,
                 duk_idx_t nargs);

void registerSysModule(duk_context *ctx);
void registerGpioModule(duk_context *ctx);
void registerFsModule(duk_context *ctx);
void registerUiModule(duk_context *ctx);
void registerIrModule(duk_context *ctx);
void registerNfcModule(duk_context *ctx);
void registerCc1101Module(duk_context *ctx);

}  // namespace zxjs
