#include "i18n.h"

namespace {

const char *textEn(UiTextKey key) {
  switch (key) {
    case UiTextKey::OkSelectBackExit:
      return "OK Select  BACK Exit";
    case UiTextKey::OkBackExit:
      return "OK/BACK Exit";
    case UiTextKey::OkBackClose:
      return "OK/BACK Close";
    case UiTextKey::BackExit:
      return "BACK Exit";
    case UiTextKey::BackCancel:
      return "BACK Cancel";
    case UiTextKey::Select:
      return "Select";
    case UiTextKey::Launcher:
      return "Launcher";
    case UiTextKey::Settings:
      return "Setting";
    case UiTextKey::FileExplorer:
      return "File Explorer";
    case UiTextKey::AppMarket:
      return "APPMarket";
    case UiTextKey::Rf:
      return "RF";
    case UiTextKey::Nfc:
      return "NFC";
    case UiTextKey::Rfid:
      return "RFID";
    case UiTextKey::Nrf24:
      return "NRF24";
    case UiTextKey::Js:
      return "JS Console";
    case UiTextKey::OpenClaw:
      return "OpenClaw";
    case UiTextKey::Language:
      return "Language";
    case UiTextKey::English:
      return "English";
    case UiTextKey::Korean:
      return "Korean";
    case UiTextKey::LanguageAndFont:
      return "Language & Font";
    case UiTextKey::FontPacks:
      return "Font Packs";
    case UiTextKey::KoreanFontPack:
      return "Korean Font Pack";
    case UiTextKey::Install:
      return "Install";
    case UiTextKey::Uninstall:
      return "Uninstall";
    case UiTextKey::Installed:
      return "Installed";
    case UiTextKey::NotInstalled:
      return "Not Installed";
    case UiTextKey::FontInstalled:
      return "Korean font installed";
    case UiTextKey::FontUninstalled:
      return "Korean font uninstalled";
    case UiTextKey::FontRequiredForKorean:
      return "Install Korean font first";
    case UiTextKey::Saved:
      return "Saved";
    case UiTextKey::UnsavedChanges:
      return "Unsaved changes";
    default:
      return "";
  }
}

const char *textKo(UiTextKey key) {
  switch (key) {
    case UiTextKey::OkSelectBackExit:
      return "OK 선택  BACK 종료";
    case UiTextKey::OkBackExit:
      return "OK/BACK 종료";
    case UiTextKey::OkBackClose:
      return "OK/BACK 닫기";
    case UiTextKey::BackExit:
      return "BACK 종료";
    case UiTextKey::BackCancel:
      return "BACK 취소";
    case UiTextKey::Select:
      return "선택";
    case UiTextKey::Launcher:
      return "런처";
    case UiTextKey::Settings:
      return "설정";
    case UiTextKey::FileExplorer:
      return "파일 탐색기";
    case UiTextKey::AppMarket:
      return "앱마켓";
    case UiTextKey::Rf:
      return "RF";
    case UiTextKey::Nfc:
      return "NFC";
    case UiTextKey::Rfid:
      return "RFID";
    case UiTextKey::Nrf24:
      return "NRF24";
    case UiTextKey::Js:
      return "JS 콘솔";
    case UiTextKey::OpenClaw:
      return "OpenClaw";
    case UiTextKey::Language:
      return "언어";
    case UiTextKey::English:
      return "영어";
    case UiTextKey::Korean:
      return "한국어";
    case UiTextKey::LanguageAndFont:
      return "언어 및 글꼴";
    case UiTextKey::FontPacks:
      return "글꼴 팩";
    case UiTextKey::KoreanFontPack:
      return "한국어 글꼴 팩";
    case UiTextKey::Install:
      return "설치";
    case UiTextKey::Uninstall:
      return "제거";
    case UiTextKey::Installed:
      return "설치됨";
    case UiTextKey::NotInstalled:
      return "미설치";
    case UiTextKey::FontInstalled:
      return "한국어 글꼴 설치됨";
    case UiTextKey::FontUninstalled:
      return "한국어 글꼴 제거됨";
    case UiTextKey::FontRequiredForKorean:
      return "한국어 글꼴을 먼저 설치하세요";
    case UiTextKey::Saved:
      return "저장됨";
    case UiTextKey::UnsavedChanges:
      return "저장되지 않은 변경사항";
    default:
      return "";
  }
}

}  // namespace

UiLanguage uiLanguageFromConfigCode(const String &code) {
  String normalized = code;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "ko" || normalized == "korean" || normalized == "kr") {
    return UiLanguage::Korean;
  }
  return UiLanguage::English;
}

const char *uiLanguageCode(UiLanguage lang) {
  return lang == UiLanguage::Korean ? "ko" : "en";
}

const char *uiLanguageLabel(UiLanguage lang) {
  return lang == UiLanguage::Korean ? "한국어" : "English";
}

const char *uiText(UiLanguage lang, UiTextKey key) {
  return lang == UiLanguage::Korean ? textKo(key) : textEn(key);
}
