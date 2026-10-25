#include <Arduino.h>
#include <SD.h>

#include <string.h>

#include "../sd_storage.h"
#include "js_internal.h"

// fs.* exposes the SD card to scripts, scoped to its root (no "..").
namespace zxjs {

namespace {

constexpr size_t kMaxReadBytes = 128UL * 1024UL;
constexpr size_t kMaxListEntries = 500;

bool checkedPath(duk_context *ctx, duk_idx_t idx, String &path) {
  path = duk_require_string(ctx, idx);
  if (!sdstorage::isSafePath(path)) {
    duk_error(ctx, DUK_ERR_ERROR, "invalid path (must be absolute, no '..')");
    return false;
  }
  String mountErr;
  if (!sdstorage::ensureMounted(&mountErr)) {
    duk_error(ctx, DUK_ERR_ERROR, "%s",
              mountErr.length() ? mountErr.c_str() : "SD card not available");
    return false;
  }
  return true;
}

duk_ret_t fsReadText(duk_context *ctx) {
  String path;
  if (!checkedPath(ctx, 0, path)) {
    return 0;
  }
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    return duk_error(ctx, DUK_ERR_ERROR, "cannot open %s", path.c_str());
  }
  const size_t size = f.size();
  if (size > kMaxReadBytes) {
    f.close();
    return duk_error(ctx, DUK_ERR_ERROR, "file too large (max %u bytes)",
                     (unsigned)kMaxReadBytes);
  }
  String text;
  text.reserve(size + 1);
  uint8_t buf[512];
  size_t remaining = size;
  while (remaining > 0) {
    const size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
    const size_t got = f.read(buf, chunk);
    if (got == 0) {
      break;
    }
    text.concat(reinterpret_cast<const char *>(buf), got);
    remaining -= got;
  }
  f.close();
  duk_push_string(ctx, text.c_str());
  return 1;
}

duk_ret_t fsReadBytes(duk_context *ctx) {
  String path;
  if (!checkedPath(ctx, 0, path)) {
    return 0;
  }
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    return duk_error(ctx, DUK_ERR_ERROR, "cannot open %s", path.c_str());
  }
  const size_t size = f.size();
  if (size > kMaxReadBytes) {
    f.close();
    return duk_error(ctx, DUK_ERR_ERROR, "file too large (max %u bytes)",
                     (unsigned)kMaxReadBytes);
  }
  void *buf = duk_push_fixed_buffer(ctx, size);
  const size_t got = size > 0 ? f.read(static_cast<uint8_t *>(buf), size) : 0;
  f.close();
  duk_push_buffer_object(ctx, -1, 0, got, DUK_BUFOBJ_UINT8ARRAY);
  duk_swap_top(ctx, -2);
  duk_pop(ctx);
  return 1;
}

duk_ret_t fsWrite(duk_context *ctx) {
  String path;
  if (!checkedPath(ctx, 0, path)) {
    return 0;
  }
  const bool append = duk_opt_boolean(ctx, 2, 0) != 0;

  std::vector<uint8_t> bytes;
  String err;
  if (duk_is_string(ctx, 1)) {
    const char *s = duk_get_string(ctx, 1);
    bytes.assign(reinterpret_cast<const uint8_t *>(s),
                reinterpret_cast<const uint8_t *>(s) + strlen(s));
  } else if (!getBytes(ctx, 1, bytes, kMaxReadBytes, err)) {
    return duk_error(ctx, DUK_ERR_ERROR, "%s", err.c_str());
  }

  File f = SD.open(path, append ? FILE_APPEND : FILE_WRITE);
  if (!f) {
    return duk_error(ctx, DUK_ERR_ERROR, "cannot open %s for write",
                     path.c_str());
  }
  const size_t written =
      bytes.empty() ? 0 : f.write(bytes.data(), bytes.size());
  f.close();
  duk_push_uint(ctx, static_cast<duk_uint_t>(written));
  return 1;
}

duk_ret_t fsRemove(duk_context *ctx) {
  String path;
  if (!checkedPath(ctx, 0, path)) {
    return 0;
  }
  duk_push_boolean(ctx, SD.remove(path) ? 1 : 0);
  return 1;
}

duk_ret_t fsMkdir(duk_context *ctx) {
  String path;
  if (!checkedPath(ctx, 0, path)) {
    return 0;
  }
  duk_push_boolean(ctx, SD.mkdir(path) ? 1 : 0);
  return 1;
}

duk_ret_t fsExists(duk_context *ctx) {
  String path;
  if (!checkedPath(ctx, 0, path)) {
    return 0;
  }
  duk_push_boolean(ctx, SD.exists(path) ? 1 : 0);
  return 1;
}

duk_ret_t fsList(duk_context *ctx) {
  String path;
  if (!checkedPath(ctx, 0, path)) {
    return 0;
  }
  File dir = SD.open(path, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    return duk_error(ctx, DUK_ERR_ERROR, "not a directory: %s", path.c_str());
  }
  duk_push_array(ctx);
  duk_uarridx_t i = 0;
  for (File entry = dir.openNextFile();
      entry && i < kMaxListEntries;
      entry = dir.openNextFile(), ++i) {
    duk_push_object(ctx);
    const char *entryName = entry.name();
    const char *base = strrchr(entryName, '/');
    duk_push_string(ctx, base ? base + 1 : entryName);
    duk_put_prop_string(ctx, -2, "name");
    duk_push_boolean(ctx, entry.isDirectory());
    duk_put_prop_string(ctx, -2, "dir");
    duk_push_number(ctx, static_cast<double>(entry.size()));
    duk_put_prop_string(ctx, -2, "size");
    entry.close();
    duk_put_prop_index(ctx, -2, i);
  }
  dir.close();
  return 1;
}

}  // namespace

void registerFsModule(duk_context *ctx) {
  duk_push_global_object(ctx);
  duk_push_object(ctx);
  putFunction(ctx, "readText", fsReadText, 1);
  putFunction(ctx, "readBytes", fsReadBytes, 1);
  putFunction(ctx, "write", fsWrite, 3);
  putFunction(ctx, "remove", fsRemove, 1);
  putFunction(ctx, "mkdir", fsMkdir, 1);
  putFunction(ctx, "exists", fsExists, 1);
  putFunction(ctx, "list", fsList, 1);
  duk_put_prop_string(ctx, -2, "fs");
  duk_pop(ctx);
}

}  // namespace zxjs
