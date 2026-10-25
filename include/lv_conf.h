#ifndef LV_CONF_H
#define LV_CONF_H

/* Core */
#define LV_COLOR_DEPTH 16
#define LV_USE_LOG 0
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130

/* Classic ESP32 (CYD, no PSRAM) cannot fit LVGL's 64 KB built-in pool in
 * static DRAM; allocate LVGL objects from the regular heap instead. */
#if defined(BOARD_CYD_2432S028)
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#endif

/* Fonts */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_UNSCII_16 0
#define LV_FONT_SOURCE_HAN_SANS_SC_14_CJK 0
#define LV_USE_FONT_COMPRESSED 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Keep text UTF-8 for bilingual strings */
#define LV_TXT_ENC LV_TXT_ENC_UTF8

/* Widgets used by this firmware */
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_LIST 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_MSGBOX 1
#define LV_USE_BAR 1
#define LV_USE_SPINNER 1
#define LV_USE_BUTTONMATRIX 1

/* Disable heavy widgets we don't use */
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 1
#define LV_USE_ARCLABEL 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGE 1
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_MENU 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/* Themes */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 40
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0

/* Layouts */
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

/* File system/image decoders for SD media preview */
#define LV_USE_FS_ARDUINO_SD 1
#define LV_FS_ARDUINO_SD_LETTER 'S'
#define LV_FS_ARDUINO_SD_PATH "/sd"

#define LV_USE_LODEPNG 1
#define LV_USE_TJPGD 1
#define LV_USE_BMP 1

/* Examples / demos */
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#endif /* LV_CONF_H */
