#include "js_engine.h"

#include <SD.h>
#include <esp_heap_caps.h>

#include <string.h>

#include "../sd_storage.h"
#include "js_internal.h"

namespace zxjs {

namespace {

RunState *gState = nullptr;
bool gBusy = false;

constexpr size_t kAllocHeader = 16;  // keeps 16-byte alignment, stores size
constexpr size_t kPsramHeapLimit = 3UL * 1024UL * 1024UL;
constexpr size_t kInternalHeapLimit = 96UL * 1024UL;
constexpr size_t kMaxScriptFileBytes = 256UL * 1024UL;
constexpr uint32_t kCancelPollIntervalMs = 60;

void *rawAlloc(size_t total) {
  void *p = nullptr;
  if (psramFound()) {
    p = heap_caps_aligned_alloc(16, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!p) {
    p = heap_caps_aligned_alloc(16, total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return p;
}

void *zxAlloc(void *, duk_size_t size) {
  if (size == 0) {
    return nullptr;
  }
  RunState *s = gState;
  if (s && s->memUsed + size > s->memLimit) {
    return nullptr;  // surfaces as a catchable "alloc failed" error
  }
  uint8_t *base = static_cast<uint8_t *>(rawAlloc(size + kAllocHeader));
  if (!base) {
    return nullptr;
  }
  *reinterpret_cast<size_t *>(base) = size;
  if (s) {
    s->memUsed += size;
  }
  return base + kAllocHeader;
}

void zxFree(void *, void *ptr) {
  if (!ptr) {
    return;
  }
  uint8_t *base = static_cast<uint8_t *>(ptr) - kAllocHeader;
  const size_t size = *reinterpret_cast<size_t *>(base);
  if (gState) {
    gState->memUsed -= size;
  }
  heap_caps_free(base);
}

void *zxRealloc(void *udata, void *ptr, duk_size_t newSize) {
  if (!ptr) {
    return zxAlloc(udata, newSize);
  }
  if (newSize == 0) {
    zxFree(udata, ptr);
    return nullptr;
  }
  const size_t oldSize =
      *reinterpret_cast<size_t *>(static_cast<uint8_t *>(ptr) - kAllocHeader);
  void *fresh = zxAlloc(udata, newSize);
  if (!fresh) {
    return nullptr;  // old block stays valid, as Duktape expects
  }
  memcpy(fresh, ptr, oldSize < newSize ? oldSize : newSize);
  zxFree(udata, ptr);
  return fresh;
}

void zxFatal(void *, const char *msg) {
  Serial.printf("[js] FATAL: %s\n", msg ? msg : "(null)");
  Serial.flush();
  delay(100);
  ESP.restart();
  while (true) {
  }
}

// JS side of the framework: small conveniences layered on the native
// bindings. Runs once per script, before user code.
const char kPrelude[] = R"JS(
(function (g) {
  var hexd = '0123456789abcdef';
  g.toHex = function (b, sep) {
    sep = (sep === undefined) ? '' : String(sep);
    var out = [];
    for (var i = 0; i < b.length; i++) {
      var v = b[i] & 255;
      out.push(hexd.charAt(v >> 4) + hexd.charAt(v & 15));
    }
    return out.join(sep);
  };
  g.fromHex = function (s) {
    s = String(s).replace(/[^0-9a-fA-F]/g, '');
    if (s.length % 2) throw new Error('odd number of hex digits');
    var o = new Uint8Array(s.length / 2);
    for (var i = 0; i < o.length; i++) o[i] = parseInt(s.substr(i * 2, 2), 16);
    return o;
  };
  function fmt(a) {
    if (typeof a === 'string') return a;
    if (a === undefined) return 'undefined';
    if (a === null) return 'null';
    if (a instanceof Uint8Array) return 'hex:' + g.toHex(a);
    if (a instanceof Error) return String(a);
    if (typeof a === 'object') {
      try { return Duktape.enc('jx', a); } catch (e) { return String(a); }
    }
    return String(a);
  }
  g.print = function () {
    var p = [];
    for (var i = 0; i < arguments.length; i++) p.push(fmt(arguments[i]));
    __print(p.join(' '));
  };
  g.console = { log: g.print, info: g.print, warn: g.print, error: g.print, debug: g.print };
  g.sleep = g.delay;

  if (g.cc1101) {
    g.cc1101.dump = function () {
      var o = [];
      for (var a = 0; a <= 0x2e; a++) o.push(g.cc1101.readReg(a));
      return o;
    };
  }

  if (g.nfc && g.nfc.mifare) {
    // Dictionary attack helper: a failed MIFARE auth deselects the tag, so
    // the tag is re-scanned before each key is tried.
    g.nfc.mifare.findKey = function (block, keys, useKeyB) {
      for (var i = 0; i < keys.length; i++) {
        var tag = g.nfc.scan(1500);
        if (!tag) return null;
        var k = (typeof keys[i] === 'string') ? g.fromHex(keys[i]) : keys[i];
        if (g.nfc.mifare.auth(tag.uid, block, k, !!useKeyB)) {
          return { key: k, keyHex: g.toHex(k), uid: tag.uid };
        }
      }
      return null;
    };
  }
})(this);
)JS";

duk_ret_t nativePrint(duk_context *ctx) {
  const char *text = duk_to_string(ctx, 0);
  if (gState && gState->host.print) {
    gState->host.print(String(text));
  } else {
    Serial.println(text);
  }
  return 0;
}

duk_ret_t nativeDelay(duk_context *ctx) {
  const double ms = duk_to_number(ctx, 0);
  const uint32_t startMs = millis();
  const uint32_t waitMs = (ms > 0) ? static_cast<uint32_t>(ms) : 0;
  while (millis() - startMs < waitMs) {
    if (yieldHost()) {
      return throwStop(ctx);
    }
    delay(1);
  }
  if (yieldHost()) {
    return throwStop(ctx);
  }
  return 0;
}

duk_ret_t nativeMillis(duk_context *ctx) {
  duk_push_number(ctx, static_cast<double>(millis()));
  return 1;
}

duk_ret_t nativeRandom(duk_context *ctx) {
  duk_push_number(ctx, static_cast<double>(esp_random()) / 4294967296.0);
  return 1;
}

void installGlobals(duk_context *ctx) {
  duk_push_global_object(ctx);

  putFunction(ctx, "__print", nativePrint, 1);
  putFunction(ctx, "delay", nativeDelay, 1);
  putFunction(ctx, "millis", nativeMillis, 0);
  putFunction(ctx, "random", nativeRandom, 0);

  duk_pop(ctx);  // global object

  registerSysModule(ctx);
  registerGpioModule(ctx);
  registerFsModule(ctx);
  if (gState->host.ui) {
    registerUiModule(ctx);
  }
  registerIrModule(ctx);
  registerNfcModule(ctx);
  registerCc1101Module(ctx);
}

String describeError(duk_context *ctx) {
  String text;
  if (duk_is_error(ctx, -1)) {
    duk_get_prop_string(ctx, -1, "stack");
    if (duk_is_string(ctx, -1)) {
      text = duk_safe_to_string(ctx, -1);
    }
    duk_pop(ctx);
    if (text.length() == 0) {
      text = duk_safe_to_string(ctx, -1);
    }
  } else {
    text = duk_safe_to_string(ctx, -1);
  }
  // Keep only the first few lines so a toast/log line stays readable.
  int lines = 0;
  int cut = -1;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] == '\n') {
      if (++lines >= 6) {
        cut = static_cast<int>(i);
        break;
      }
    }
  }
  if (cut >= 0) {
    text = text.substring(0, cut);
  }
  return text;
}

}  // namespace

bool isBusy() { return gBusy; }

bool pollStop() {
  RunState *s = gState;
  if (!s || s->stop) {
    return s ? s->stop : false;
  }
  const uint32_t now = millis();
  if (now - s->lastPollMs < kCancelPollIntervalMs && !s->exited) {
    return false;
  }
  s->lastPollMs = now;

  if (s->deadlineMs != 0 && now >= s->deadlineMs) {
    s->timedOut = true;
    s->stop = true;
    return true;
  }
  if (s->host.cancelRequested && s->host.cancelRequested()) {
    s->cancelled = true;
    s->stop = true;
    return true;
  }
  return false;
}

bool yieldHost() {
  RunState *s = gState;
  if (s && s->host.yield) {
    s->host.yield();
  }
  return pollStop();
}

RunState *state() { return gState; }

duk_ret_t throwStop(duk_context *ctx) {
  RunState *s = gState;
  const bool timedOut = s && s->timedOut;
  return duk_error(ctx, DUK_ERR_RANGE_ERROR, timedOut ? "execution timeout"
                                                      : "script stopped");
}

static int zxjsTimeoutCheck(void *udata) {
  (void)udata;
  return pollStop() ? 1 : 0;
}

Result runSource(const String &source,
                 const String &name,
                 const Host &host,
                 uint32_t timeoutMs) {
  Result result;
  if (gBusy) {
    result.error = "another script is already running";
    return result;
  }
  gBusy = true;

  RunState localState;
  localState.host = host;
  localState.lastPollMs = millis();
  localState.deadlineMs = (timeoutMs == 0) ? 0 : millis() + timeoutMs;
  // PSRAM is scarce on boards without it; cap scripts to a safe slice of
  // whichever heap actually backs the allocator.
  localState.memLimit = psramFound() ? kPsramHeapLimit : kInternalHeapLimit;
  gState = &localState;

  duk_context *ctx = duk_create_heap(zxAlloc, zxRealloc, zxFree, nullptr, zxFatal);
  if (!ctx) {
    gState = nullptr;
    gBusy = false;
    result.error = "failed to create JS heap";
    return result;
  }

  installGlobals(ctx);

  bool ok = (duk_peval_string(ctx, kPrelude) == 0);
  if (ok) {
    duk_pop(ctx);  // discard the prelude's own return value
    const char *fname = name.length() ? name.c_str() : "script.js";
    duk_push_string(ctx, source.c_str());
    duk_push_string(ctx, fname);
    ok = (duk_pcompile(ctx, DUK_COMPILE_EVAL) == 0);
    if (ok) {
      ok = (duk_pcall(ctx, 0) == 0);
    }
  }

  if (ok) {
    result.ok = true;
    result.value = duk_safe_to_string(ctx, -1);
  } else {
    result.ok = false;
    result.error = describeError(ctx);
  }
  duk_pop(ctx);

  result.cancelled = localState.cancelled;
  result.timedOut = localState.timedOut;

  duk_destroy_heap(ctx);
  gState = nullptr;
  gBusy = false;
  return result;
}

Result runFile(const String &path, const Host &host, uint32_t timeoutMs) {
  Result result;
  String mountErr;
  if (!sdstorage::ensureMounted(&mountErr)) {
    result.error = mountErr.length() ? mountErr : "SD card not available";
    return result;
  }
  if (!sdstorage::isSafePath(path)) {
    result.error = "invalid path";
    return result;
  }

  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    result.error = "cannot open " + path;
    return result;
  }
  const size_t size = file.size();
  if (size == 0 || size > kMaxScriptFileBytes) {
    file.close();
    result.error = "script file too large or empty";
    return result;
  }

  String source;
  source.reserve(size + 1);
  uint8_t buf[512];
  size_t remaining = size;
  while (remaining > 0) {
    const size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
    const size_t got = file.read(buf, chunk);
    if (got == 0) {
      break;
    }
    source.concat(reinterpret_cast<const char *>(buf), got);
    remaining -= got;
  }
  file.close();

  return runSource(source, path, host, timeoutMs);
}

}  // namespace zxjs
