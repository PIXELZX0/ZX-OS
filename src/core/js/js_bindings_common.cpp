#include "js_internal.h"

#include <ctype.h>
#include <stdlib.h>

namespace zxjs {

namespace {
constexpr char kHex[] = "0123456789ABCDEF";
}

String bytesToHexString(const uint8_t *data, size_t size, char sep) {
  String out;
  out.reserve(size * (sep ? 3 : 2));
  for (size_t i = 0; i < size; ++i) {
    if (sep && i > 0) {
      out += sep;
    }
    out += kHex[(data[i] >> 4) & 0x0F];
    out += kHex[data[i] & 0x0F];
  }
  return out;
}

bool getBytes(duk_context *ctx,
              duk_idx_t idx,
              std::vector<uint8_t> &out,
              size_t maxLen,
              String &err) {
  out.clear();
  idx = duk_normalize_index(ctx, idx);

  if (duk_is_string(ctx, idx)) {
    String s = duk_get_string(ctx, idx);
    String hex;
    hex.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
      const char c = s[i];
      if (isxdigit(static_cast<unsigned char>(c))) {
        hex += c;
      }
    }
    if (hex.length() % 2 != 0) {
      err = "hex string must have an even number of digits";
      return false;
    }
    if (hex.length() / 2 > maxLen) {
      err = "payload too long";
      return false;
    }
    out.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
      out.push_back(static_cast<uint8_t>(
          strtoul(hex.substring(i, i + 2).c_str(), nullptr, 16)));
    }
    return true;
  }

  if (duk_is_buffer_data(ctx, idx)) {
    duk_size_t len = 0;
    void *data = duk_get_buffer_data(ctx, idx, &len);
    if (len > maxLen) {
      err = "payload too long";
      return false;
    }
    out.assign(static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + len);
    return true;
  }

  if (duk_is_array(ctx, idx)) {
    const duk_size_t len = duk_get_length(ctx, idx);
    if (len > maxLen) {
      err = "payload too long";
      return false;
    }
    out.reserve(len);
    for (duk_uarridx_t i = 0; i < len; ++i) {
      duk_get_prop_index(ctx, idx, i);
      const duk_int_t v = duk_to_int(ctx, -1);
      duk_pop(ctx);
      if (v < 0 || v > 255) {
        err = "array values must be 0..255";
        return false;
      }
      out.push_back(static_cast<uint8_t>(v));
    }
    return true;
  }

  err = "expected a byte array, Uint8Array or hex string";
  return false;
}

void pushBytes(duk_context *ctx, const uint8_t *data, size_t size) {
  void *buf = duk_push_fixed_buffer(ctx, size);
  if (size > 0) {
    memcpy(buf, data, size);
  }
  duk_push_buffer_object(ctx, -1, 0, size, DUK_BUFOBJ_UINT8ARRAY);
  duk_swap_top(ctx, -2);
  duk_pop(ctx);  // drop the plain buffer, keep the Uint8Array
}

bool getTimings(duk_context *ctx,
                duk_idx_t idx,
                std::vector<uint16_t> &out,
                size_t maxLen,
                String &err) {
  out.clear();
  idx = duk_normalize_index(ctx, idx);
  if (!duk_is_array(ctx, idx) && !duk_is_buffer_data(ctx, idx)) {
    err = "expected an array of pulse durations";
    return false;
  }
  const duk_size_t len = duk_get_length(ctx, idx);
  if (len > maxLen) {
    err = "too many timings";
    return false;
  }
  out.reserve(len);
  for (duk_uarridx_t i = 0; i < len; ++i) {
    duk_get_prop_index(ctx, idx, i);
    const duk_int_t v = duk_to_int(ctx, -1);
    duk_pop(ctx);
    if (v < 0 || v > 65535) {
      err = "timing values must be 0..65535";
      return false;
    }
    out.push_back(static_cast<uint16_t>(v));
  }
  return true;
}

void pushTimings(duk_context *ctx, const std::vector<uint16_t> &timings) {
  duk_push_array(ctx);
  for (size_t i = 0; i < timings.size(); ++i) {
    duk_push_uint(ctx, timings[i]);
    duk_put_prop_index(ctx, -2, static_cast<duk_uarridx_t>(i));
  }
}

bool getNumberProp(duk_context *ctx,
                   duk_idx_t objIdx,
                   const char *key,
                   double &out) {
  objIdx = duk_normalize_index(ctx, objIdx);
  if (!duk_is_object(ctx, objIdx) || !duk_has_prop_string(ctx, objIdx, key)) {
    return false;
  }
  duk_get_prop_string(ctx, objIdx, key);
  out = duk_to_number(ctx, -1);
  duk_pop(ctx);
  return true;
}

bool getBoolProp(duk_context *ctx,
                 duk_idx_t objIdx,
                 const char *key,
                 bool &out) {
  objIdx = duk_normalize_index(ctx, objIdx);
  if (!duk_is_object(ctx, objIdx) || !duk_has_prop_string(ctx, objIdx, key)) {
    return false;
  }
  duk_get_prop_string(ctx, objIdx, key);
  out = duk_to_boolean(ctx, -1) != 0;
  duk_pop(ctx);
  return true;
}

void putFunction(duk_context *ctx,
                 const char *name,
                 duk_c_function fn,
                 duk_idx_t nargs) {
  duk_push_c_function(ctx, fn, nargs);
  duk_put_prop_string(ctx, -2, name);
}

}  // namespace zxjs
