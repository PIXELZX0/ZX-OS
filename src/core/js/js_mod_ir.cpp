#include <Arduino.h>

#include "../ir_protocols.h"
#include "../ir_radio.h"
#include "js_internal.h"

// ir.* — infrared send/receive/encode/decode.
namespace zxjs {

namespace {

bool parseProtocolArg(duk_context *ctx, duk_idx_t idx, irproto::Protocol &out) {
  const char *name = duk_require_string(ctx, idx);
  if (!irproto::parseName(name, out)) {
    duk_error(ctx, DUK_ERR_ERROR,
              "unknown IR protocol '%s' (NEC, NECext, Samsung32, SIRC, "
              "SIRC15, SIRC20, RC5)",
              name);
    return false;
  }
  return true;
}

duk_ret_t irSend(duk_context *ctx) {
  irproto::Protocol proto;
  if (!parseProtocolArg(ctx, 0, proto)) {
    return 0;
  }
  const uint32_t address = duk_require_uint(ctx, 1);
  const uint32_t command = duk_require_uint(ctx, 2);
  const int repeat = duk_opt_int(ctx, 3, 1);

  String err;
  if (!irradio::send(proto, address, command, repeat, 0, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t irSendRaw(duk_context *ctx) {
  std::vector<uint16_t> timings;
  String err;
  if (!getTimings(ctx, 0, timings, 4000, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  const int carrierHz = duk_opt_int(ctx, 1, 38000);
  if (!irradio::sendRaw(timings.data(), timings.size(),
                        static_cast<uint32_t>(carrierHz), err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t irReceive(duk_context *ctx) {
  const int timeoutMs = duk_opt_int(ctx, 0, 5000);
  std::vector<uint16_t> timings;
  String err;
  const bool got = irradio::receiveRaw(
      timeoutMs > 0 ? timeoutMs : 5000, timings, err,
      [ctx]() { return !yieldHost(); });
  if (yieldHost()) {
    return throwStop(ctx);
  }
  if (!got) {
    if (err.length()) {
      return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
    }
    duk_push_null(ctx);
    return 1;
  }

  irproto::Frame frame;
  const bool decoded =
      irproto::decode(timings.data(), timings.size(), frame);

  duk_push_object(ctx);
  pushTimings(ctx, timings);
  duk_put_prop_string(ctx, -2, "raw");
  if (decoded) {
    duk_push_string(ctx, irproto::name(frame.protocol));
    duk_put_prop_string(ctx, -2, "protocol");
    duk_push_uint(ctx, frame.address);
    duk_put_prop_string(ctx, -2, "address");
    duk_push_uint(ctx, frame.command);
    duk_put_prop_string(ctx, -2, "command");
    duk_push_boolean(ctx, frame.toggle);
    duk_put_prop_string(ctx, -2, "toggle");
  } else {
    duk_push_null(ctx);
    duk_put_prop_string(ctx, -2, "protocol");
  }
  return 1;
}

duk_ret_t irDecode(duk_context *ctx) {
  std::vector<uint16_t> timings;
  String err;
  if (!getTimings(ctx, 0, timings, 4000, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  irproto::Frame frame;
  if (!irproto::decode(timings.data(), timings.size(), frame)) {
    duk_push_null(ctx);
    return 1;
  }
  duk_push_object(ctx);
  duk_push_string(ctx, irproto::name(frame.protocol));
  duk_put_prop_string(ctx, -2, "protocol");
  duk_push_uint(ctx, frame.address);
  duk_put_prop_string(ctx, -2, "address");
  duk_push_uint(ctx, frame.command);
  duk_put_prop_string(ctx, -2, "command");
  duk_push_boolean(ctx, frame.toggle);
  duk_put_prop_string(ctx, -2, "toggle");
  return 1;
}

duk_ret_t irEncode(duk_context *ctx) {
  irproto::Protocol proto;
  if (!parseProtocolArg(ctx, 0, proto)) {
    return 0;
  }
  const uint32_t address = duk_require_uint(ctx, 1);
  const uint32_t command = duk_require_uint(ctx, 2);

  std::vector<uint16_t> timings;
  const char *err = nullptr;
  if (!irproto::encode(proto, address, command, 0, timings, &err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err ? err : "encode failed");
  }
  pushTimings(ctx, timings);
  return 1;
}

}  // namespace

void registerIrModule(duk_context *ctx) {
  duk_push_global_object(ctx);
  duk_push_object(ctx);
  putFunction(ctx, "send", irSend, 4);
  putFunction(ctx, "sendRaw", irSendRaw, 2);
  putFunction(ctx, "receive", irReceive, 1);
  putFunction(ctx, "decode", irDecode, 1);
  putFunction(ctx, "encode", irEncode, 3);
  duk_put_prop_string(ctx, -2, "ir");
  duk_pop(ctx);
}

}  // namespace zxjs
