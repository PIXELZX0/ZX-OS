#include <Arduino.h>

#include "../../ui/ui_runtime.h"
#include "js_internal.h"

// ui.* — lets a script show toasts/dialogs on the device's own screen.
// Only registered when the engine was started with a UiRuntime (js_app.cpp).
namespace zxjs {

namespace {

UiRuntime *ui() { return state() ? state()->host.ui : nullptr; }

std::function<void()> backgroundTick() {
  return []() { yieldHost(); };
}

duk_ret_t uiToast(duk_context *ctx) {
  const char *title = duk_require_string(ctx, 0);
  const char *message = duk_require_string(ctx, 1);
  const int showMs = duk_opt_int(ctx, 2, 1200);
  UiRuntime *u = ui();
  if (u) {
    u->showToast(title, message, showMs > 0 ? showMs : 1200, backgroundTick());
  }
  if (yieldHost()) {
    return throwStop(ctx);
  }
  return 0;
}

duk_ret_t uiConfirm(duk_context *ctx) {
  const char *title = duk_require_string(ctx, 0);
  const char *message = duk_require_string(ctx, 1);
  UiRuntime *u = ui();
  bool result = false;
  if (u) {
    result = u->confirm(title, message, backgroundTick());
  }
  if (yieldHost()) {
    return throwStop(ctx);
  }
  duk_push_boolean(ctx, result);
  return 1;
}

duk_ret_t uiPrompt(duk_context *ctx) {
  const char *title = duk_require_string(ctx, 0);
  String value = duk_opt_string(ctx, 1, "");
  UiRuntime *u = ui();
  bool ok = false;
  if (u) {
    ok = u->textInput(title, value, false, backgroundTick());
  }
  if (yieldHost()) {
    return throwStop(ctx);
  }
  if (!ok) {
    duk_push_null(ctx);
    return 1;
  }
  duk_push_string(ctx, value.c_str());
  return 1;
}

duk_ret_t uiProgress(duk_context *ctx) {
  const char *title = duk_require_string(ctx, 0);
  const char *message = duk_require_string(ctx, 1);
  const int percent = duk_opt_int(ctx, 2, -1);
  UiRuntime *u = ui();
  if (u) {
    u->showProgressOverlay(title, message, percent);
  }
  if (yieldHost()) {
    return throwStop(ctx);
  }
  return 0;
}

duk_ret_t uiProgressHide(duk_context *ctx) {
  UiRuntime *u = ui();
  if (u) {
    u->hideProgressOverlay();
  }
  return 0;
}

duk_ret_t uiStatus(duk_context *ctx) {
  const char *line = duk_require_string(ctx, 0);
  UiRuntime *u = ui();
  if (u) {
    u->setStatusLine(line);
  }
  return 0;
}

}  // namespace

void registerUiModule(duk_context *ctx) {
  duk_push_global_object(ctx);
  duk_push_object(ctx);
  putFunction(ctx, "toast", uiToast, 3);
  putFunction(ctx, "confirm", uiConfirm, 2);
  putFunction(ctx, "prompt", uiPrompt, 2);
  putFunction(ctx, "progress", uiProgress, 3);
  putFunction(ctx, "hideProgress", uiProgressHide, 0);
  putFunction(ctx, "status", uiStatus, 1);
  duk_put_prop_string(ctx, -2, "ui");
  duk_pop(ctx);
}

}  // namespace zxjs
