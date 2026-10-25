#pragma once

#include <Arduino.h>
#include <lvgl.h>

// LVGL font backed by a ZXF1 font pack on the SD card (see
// scripts/lvgl_font_to_zxf.py). Only the character map stays in RAM; glyphs
// are read from SD on demand through a small cache, so large CJK fonts need
// neither flash space nor a full in-memory copy.
namespace sdfont {

constexpr const char *kFontDir = "/fonts";
constexpr const char *kKoreanFontPath = "/fonts/korean_ui_14.zxf";
constexpr const char *kKoreanFontReleaseAsset = "zx-font-korean-ui-14.zxf";

// Opens `path` (SD must be mounted). Returns nullptr and sets `error` on
// failure. Only one font is open at a time; reopening replaces it but keeps
// the returned pointer stable.
const lv_font_t *open(const char *path, const lv_font_t *fallback, String *error = nullptr);

// Stops reading from SD; glyphs then resolve through the fallback font.
void close();

}  // namespace sdfont
