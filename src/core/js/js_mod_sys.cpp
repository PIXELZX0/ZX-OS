#include <Arduino.h>

#include "../board_pins.h"
#include "../cc1101_radio.h"
#include "js_internal.h"

namespace zxjs {

namespace {

duk_ret_t sysBoard(duk_context *ctx) {
  duk_push_string(ctx, HAL_BOARD_NAME);
  return 1;
}

duk_ret_t sysHeapFree(duk_context *ctx) {
  duk_push_object(ctx);
  duk_push_number(ctx, static_cast<double>(ESP.getFreeHeap()));
  duk_put_prop_string(ctx, -2, "internal");
  duk_push_number(ctx, static_cast<double>(psramFound() ? ESP.getFreePsram() : 0));
  duk_put_prop_string(ctx, -2, "psram");
  RunState *s = state();
  duk_push_number(ctx, static_cast<double>(s ? s->memUsed : 0));
  duk_put_prop_string(ctx, -2, "jsUsed");
  duk_push_number(ctx, static_cast<double>(s ? s->memLimit : 0));
  duk_put_prop_string(ctx, -2, "jsLimit");
  return 1;
}

duk_ret_t sysUptimeMs(duk_context *ctx) {
  duk_push_number(ctx, static_cast<double>(millis()));
  return 1;
}

}  // namespace

void registerSysModule(duk_context *ctx) {
  duk_push_global_object(ctx);
  duk_push_object(ctx);
  putFunction(ctx, "board", sysBoard, 0);
  putFunction(ctx, "heapFree", sysHeapFree, 0);
  putFunction(ctx, "uptimeMs", sysUptimeMs, 0);
  duk_put_prop_string(ctx, -2, "sys");
  duk_pop(ctx);
}

}  // namespace zxjs
