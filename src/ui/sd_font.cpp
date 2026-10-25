#include "sd_font.h"

#include <SD.h>

#include <new>
#include <vector>

namespace sdfont {
namespace {

constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderBytes = 36;
constexpr size_t kCmapBytes = 8;
constexpr size_t kGlyphBytes = 10;
constexpr size_t kMaxGlyphBitmapBytes = 64;
#if CONFIG_SPIRAM || BOARD_HAS_PSRAM
constexpr size_t kCacheSlots = 512;
#else
constexpr size_t kCacheSlots = 128;
#endif

struct Cmap {
  uint32_t rangeStart;
  uint16_t rangeLength;
  uint16_t glyphIdStart;
};

// dsc[0] is the reserved glyph 0 and dsc[1] the cached glyph with its
// bitmap_index rebased to 0, so LVGL's own fmt_txt decoder can render it.
struct Slot {
  uint16_t gid;
  lv_font_fmt_txt_glyph_dsc_t dsc[2];
  uint8_t bitmap[kMaxGlyphBitmapBytes];
};

struct State {
  bool active = false;
  String path;
  File file;
  uint8_t bpp = 0;
  uint8_t bitmapFormat = 0;
  uint32_t glyphCount = 0;
  uint32_t glyphOffset = 0;
  uint32_t bitmapOffset = 0;
  uint32_t bitmapSize = 0;
  std::vector<Cmap> cmaps;
  Slot *slots = nullptr;
  lv_font_t font;
  lv_font_t decodeFont;
  lv_font_fmt_txt_dsc_t decodeDsc;
};

State *gState = nullptr;

uint16_t readU16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t readU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool readAt(State &state, uint32_t offset, uint8_t *out, size_t len) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!state.file) {
      state.file = SD.open(state.path.c_str(), FILE_READ);
    }
    if (state.file && state.file.seek(offset) &&
        state.file.read(out, len) == len) {
      return true;
    }
    // The handle can go stale if the card was remounted; reopen once.
    state.file.close();
  }
  return false;
}

uint32_t glyphIdFor(const State &state, uint32_t letter) {
  for (const Cmap &cmap : state.cmaps) {
    const uint32_t rcp = letter - cmap.rangeStart;
    if (rcp < cmap.rangeLength) {
      return cmap.glyphIdStart + rcp;
    }
  }
  return 0;
}

Slot *loadGlyph(State &state, uint32_t gid) {
  if (gid == 0 || gid >= state.glyphCount) {
    return nullptr;
  }
  Slot &slot = state.slots[gid % kCacheSlots];
  if (slot.gid == gid) {
    return &slot;
  }
  slot.gid = 0;

  uint8_t raw[kGlyphBytes * 2];
  const bool hasNext = gid + 1 < state.glyphCount;
  if (!readAt(state, state.glyphOffset + gid * kGlyphBytes, raw,
              hasNext ? sizeof(raw) : kGlyphBytes)) {
    return nullptr;
  }
  const uint32_t bitmapIndex = readU32(raw);
  const uint32_t bitmapEnd = hasNext ? readU32(raw + kGlyphBytes) : state.bitmapSize;
  if (bitmapEnd < bitmapIndex || bitmapEnd - bitmapIndex > kMaxGlyphBitmapBytes) {
    return nullptr;
  }
  const size_t bitmapLen = bitmapEnd - bitmapIndex;
  if (bitmapLen > 0 &&
      !readAt(state, state.bitmapOffset + bitmapIndex, slot.bitmap, bitmapLen)) {
    return nullptr;
  }

  lv_font_fmt_txt_glyph_dsc_t &dsc = slot.dsc[1];
  dsc.bitmap_index = 0;
  dsc.adv_w = readU16(raw + 4);
  dsc.box_w = raw[6];
  dsc.box_h = raw[7];
  dsc.ofs_x = static_cast<int8_t>(raw[8]);
  dsc.ofs_y = static_cast<int8_t>(raw[9]);
  slot.gid = static_cast<uint16_t>(gid);
  return &slot;
}

// Mirrors lv_font_get_glyph_dsc_fmt_txt() for a font without kerning.
bool getGlyphDsc(const lv_font_t *font,
                 lv_font_glyph_dsc_t *dscOut,
                 uint32_t letter,
                 uint32_t letterNext) {
  (void)font;
  (void)letterNext;
  if (!gState || !gState->active) {
    return false;
  }
  const bool isTab = letter == '\t';
  if (isTab) {
    letter = ' ';
  }

  const uint32_t gid = glyphIdFor(*gState, letter);
  const Slot *slot = loadGlyph(*gState, gid);
  if (!slot) {
    return false;
  }
  const lv_font_fmt_txt_glyph_dsc_t &glyph = slot->dsc[1];

  uint32_t advW = glyph.adv_w;
  if (isTab) {
    advW *= 2;
  }
  dscOut->adv_w = (advW + (1 << 3)) >> 4;
  dscOut->box_w = isTab ? glyph.box_w * 2 : glyph.box_w;
  dscOut->box_h = glyph.box_h;
  dscOut->ofs_x = glyph.ofs_x;
  dscOut->ofs_y = glyph.ofs_y;
  dscOut->stride = 0;
  dscOut->format = static_cast<lv_font_glyph_format_t>(gState->bpp);
  dscOut->is_placeholder = false;
  dscOut->gid.index = gid;
  return true;
}

const void *getGlyphBitmap(lv_font_glyph_dsc_t *gDsc, lv_draw_buf_t *drawBuf) {
  if (!gState || !gState->active) {
    return nullptr;
  }
  // The slot may have been evicted since getGlyphDsc(); loadGlyph refills it.
  Slot *slot = loadGlyph(*gState, gDsc->gid.index);
  if (!slot) {
    return nullptr;
  }

  gState->decodeDsc.glyph_bitmap = slot->bitmap;
  gState->decodeDsc.glyph_dsc = slot->dsc;
  lv_font_glyph_dsc_t local = *gDsc;
  local.resolved_font = &gState->decodeFont;
  local.gid.index = 1;
  return lv_font_get_bitmap_fmt_txt(&local, drawBuf);
}

bool fail(String *error, const char *message) {
  if (error) {
    *error = message;
  }
  return false;
}

bool loadHeader(State &state, const lv_font_t *fallback, String *error) {
  uint8_t header[kHeaderBytes];
  if (!readAt(state, 0, header, sizeof(header))) {
    return fail(error, "Font file read failed");
  }
  if (memcmp(header, "ZXF1", 4) != 0 || header[4] != kVersion) {
    return fail(error, "Not a ZXF1 font");
  }

  state.bpp = header[5];
  state.bitmapFormat = header[6];
  const int16_t lineHeight = static_cast<int16_t>(readU16(header + 8));
  const int16_t baseLine = static_cast<int16_t>(readU16(header + 10));
  const int8_t underlinePosition = static_cast<int8_t>(header[12]);
  const int8_t underlineThickness = static_cast<int8_t>(header[13]);
  const uint16_t cmapCount = readU16(header + 14);
  state.glyphCount = readU32(header + 16);
  state.glyphOffset = readU32(header + 20);
  state.bitmapOffset = readU32(header + 24);
  state.bitmapSize = readU32(header + 28);
  const uint16_t maxGlyphBytes = readU16(header + 32);

  if ((state.bpp != 1 && state.bpp != 2 && state.bpp != 4 && state.bpp != 8) ||
      state.bitmapFormat > LV_FONT_FMT_TXT_COMPRESSED_NO_PREFILTER ||
      state.glyphCount < 2 || state.glyphCount > UINT16_MAX ||
      maxGlyphBytes > kMaxGlyphBitmapBytes ||
      state.glyphOffset != kHeaderBytes + cmapCount * kCmapBytes ||
      state.bitmapOffset != state.glyphOffset + state.glyphCount * kGlyphBytes ||
      state.file.size() < state.bitmapOffset + state.bitmapSize) {
    return fail(error, "Font file is corrupt");
  }

  std::vector<uint8_t> cmapRaw(cmapCount * kCmapBytes);
  if (!readAt(state, kHeaderBytes, cmapRaw.data(), cmapRaw.size())) {
    return fail(error, "Font file read failed");
  }
  state.cmaps.reserve(cmapCount);
  for (size_t i = 0; i < cmapCount; ++i) {
    const uint8_t *p = cmapRaw.data() + i * kCmapBytes;
    state.cmaps.push_back({readU32(p), readU16(p + 4), readU16(p + 6)});
  }

  memset(&state.font, 0, sizeof(state.font));
  state.font.get_glyph_dsc = getGlyphDsc;
  state.font.get_glyph_bitmap = getGlyphBitmap;
  state.font.line_height = lineHeight;
  state.font.base_line = baseLine;
  state.font.subpx = LV_FONT_SUBPX_NONE;
  state.font.underline_position = underlinePosition;
  state.font.underline_thickness = underlineThickness;
  state.font.fallback = fallback;

  memset(&state.decodeDsc, 0, sizeof(state.decodeDsc));
  state.decodeDsc.bpp = state.bpp;
  state.decodeDsc.bitmap_format = state.bitmapFormat;
  memset(&state.decodeFont, 0, sizeof(state.decodeFont));
  state.decodeFont.dsc = &state.decodeDsc;
  return true;
}

}  // namespace

// The State (and so the lv_font_t handed to LVGL) is allocated once and never
// freed: widgets may still reference the font after it is reopened or closed,
// and an inactive font simply resolves every glyph through its fallback.
const lv_font_t *open(const char *path, const lv_font_t *fallback, String *error) {
  if (!gState) {
    State *state = new (std::nothrow) State();
    Slot *slots = new (std::nothrow) Slot[kCacheSlots];
    if (!state || !slots) {
      delete state;
      delete[] slots;
      fail(error, "Out of memory");
      return nullptr;
    }
    state->slots = slots;
    gState = state;
  }

  close();
  memset(gState->slots, 0, sizeof(Slot) * kCacheSlots);
  gState->cmaps.clear();
  gState->path = path;
  if (!SD.exists(path)) {
    fail(error, "Font pack not found on SD");
    return nullptr;
  }
  if (!loadHeader(*gState, fallback, error)) {
    gState->file.close();
    return nullptr;
  }
  gState->active = true;
  return &gState->font;
}

void close() {
  if (!gState) {
    return;
  }
  gState->active = false;
  gState->file.close();
}

}  // namespace sdfont
