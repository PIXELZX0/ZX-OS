#include <Arduino.h>

#include "../nfc_reader.h"
#include "js_internal.h"

// nfc.* — PN532 (ISO14443A / MIFARE Classic / Ultralight-NTAG).
namespace zxjs {

namespace {

void pushUid(duk_context *ctx, const std::vector<uint8_t> &uid) {
  pushBytes(ctx, uid.data(), uid.size());
}

bool getUidArg(duk_context *ctx, duk_idx_t idx, std::vector<uint8_t> &uid,
              String &err) {
  return getBytes(ctx, idx, uid, 10, err);
}

duk_ret_t nfcScan(duk_context *ctx) {
  const int timeoutMs = duk_opt_int(ctx, 0, 3000);
  std::vector<uint8_t> uid;
  String err;
  const bool found = nfcreader::scan(
      timeoutMs > 0 ? timeoutMs : 3000, uid, err,
      [ctx]() { return !yieldHost(); });
  if (yieldHost()) {
    return throwStop(ctx);
  }
  if (!found) {
    if (err.length()) {
      return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
    }
    duk_push_null(ctx);
    return 1;
  }
  duk_push_object(ctx);
  pushUid(ctx, uid);
  duk_put_prop_string(ctx, -2, "uid");
  duk_push_string(ctx, bytesToHexString(uid.data(), uid.size(), ':').c_str());
  duk_put_prop_string(ctx, -2, "uidHex");
  return 1;
}

duk_ret_t nfcInfo(duk_context *ctx) {
  duk_push_object(ctx);
  duk_push_boolean(ctx, nfcreader::compiledIn());
  duk_put_prop_string(ctx, -2, "compiled");
  duk_push_boolean(ctx, nfcreader::isReady());
  duk_put_prop_string(ctx, -2, "ready");
  uint8_t ic = 0, ver = 0, rev = 0;
  if (nfcreader::firmware(ic, ver, rev)) {
    duk_push_object(ctx);
    duk_push_uint(ctx, ic);
    duk_put_prop_string(ctx, -2, "ic");
    duk_push_uint(ctx, ver);
    duk_put_prop_string(ctx, -2, "major");
    duk_push_uint(ctx, rev);
    duk_put_prop_string(ctx, -2, "minor");
    duk_put_prop_string(ctx, -2, "firmware");
  }
  return 1;
}

duk_ret_t mifareAuth(duk_context *ctx) {
  std::vector<uint8_t> uid;
  String err;
  if (!getUidArg(ctx, 0, uid, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  const uint32_t block = duk_require_uint(ctx, 1);
  std::vector<uint8_t> key;
  if (!getBytes(ctx, 2, key, 6, err) || key.size() != 6) {
    return duk_error(ctx, DUK_ERR_ERROR, "key must be 6 bytes");
  }
  const bool useKeyB = duk_opt_boolean(ctx, 3, 0) != 0;

  if (!nfcreader::mifareAuth(uid, block, useKeyB, key.data(), err)) {
    duk_push_boolean(ctx, 0);
    return 1;
  }
  duk_push_boolean(ctx, 1);
  return 1;
}

duk_ret_t mifareRead(duk_context *ctx) {
  const uint32_t block = duk_require_uint(ctx, 0);
  uint8_t data[16];
  String err;
  if (!nfcreader::mifareRead(static_cast<uint8_t>(block), data, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  pushBytes(ctx, data, sizeof(data));
  return 1;
}

duk_ret_t mifareWrite(duk_context *ctx) {
  const uint32_t block = duk_require_uint(ctx, 0);
  std::vector<uint8_t> data;
  String err;
  if (!getBytes(ctx, 1, data, 16, err) || data.size() != 16) {
    return duk_error(ctx, DUK_ERR_ERROR, "data must be 16 bytes");
  }
  if (!nfcreader::mifareWrite(static_cast<uint8_t>(block), data.data(), err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t pageRead(duk_context *ctx) {
  const uint32_t page = duk_require_uint(ctx, 0);
  uint8_t data[4];
  String err;
  if (!nfcreader::pageRead(static_cast<uint8_t>(page), data, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  pushBytes(ctx, data, sizeof(data));
  return 1;
}

duk_ret_t pageWrite(duk_context *ctx) {
  const uint32_t page = duk_require_uint(ctx, 0);
  std::vector<uint8_t> data;
  String err;
  if (!getBytes(ctx, 1, data, 4, err) || data.size() != 4) {
    return duk_error(ctx, DUK_ERR_ERROR, "data must be 4 bytes");
  }
  if (!nfcreader::pageWrite(static_cast<uint8_t>(page), data.data(), err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  return 0;
}

duk_ret_t transceive(duk_context *ctx) {
  std::vector<uint8_t> payload;
  String err;
  if (!getBytes(ctx, 0, payload, 250, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  std::vector<uint8_t> response;
  if (!nfcreader::transceive(payload.data(), payload.size(), response, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }
  pushBytes(ctx, response.data(), response.size());
  return 1;
}

}  // namespace

void registerNfcModule(duk_context *ctx) {
  duk_push_global_object(ctx);
  duk_push_object(ctx);
  putFunction(ctx, "scan", nfcScan, 1);
  putFunction(ctx, "info", nfcInfo, 0);
  putFunction(ctx, "transceive", transceive, 1);

  duk_push_object(ctx);
  putFunction(ctx, "auth", mifareAuth, 4);
  putFunction(ctx, "read", mifareRead, 1);
  putFunction(ctx, "write", mifareWrite, 2);
  duk_put_prop_string(ctx, -2, "mifare");

  duk_push_object(ctx);
  putFunction(ctx, "readPage", pageRead, 1);
  putFunction(ctx, "writePage", pageWrite, 2);
  duk_put_prop_string(ctx, -2, "ntag");

  duk_put_prop_string(ctx, -2, "nfc");
  duk_pop(ctx);
}

}  // namespace zxjs
