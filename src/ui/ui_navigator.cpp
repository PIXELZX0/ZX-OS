#include "ui_navigator.h"

#include <vector>

#include "../apps/app_market_app.h"
#include "../apps/file_explorer_app.h"
#include "../apps/nfc_app.h"
#include "../apps/rfid_app.h"
#include "../apps/settings_app.h"
#include "i18n.h"
#include "ui_runtime.h"

void UiNavigator::runLauncher(AppContext &ctx,
                              const std::function<void()> &backgroundTick) {
  if (!ctx.uiRuntime) {
    return;
  }

  const UiLanguage lang = ctx.uiRuntime->language();

  std::vector<String> items;
  items.push_back(uiText(lang, UiTextKey::AppMarket));
  items.push_back(uiText(lang, UiTextKey::Settings));
  items.push_back(uiText(lang, UiTextKey::FileExplorer));
  items.push_back(uiText(lang, UiTextKey::Nfc));
  items.push_back(uiText(lang, UiTextKey::Rfid));

  ctx.uiRuntime->setStatusLine("");

  const int choice = ctx.uiRuntime->launcherLoop(uiText(lang, UiTextKey::Launcher),
                                                 items,
                                                 selected_,
                                                 backgroundTick);
  if (choice < 0) {
    return;
  }

  selected_ = choice;
  if (choice == 0) {
    runAppMarketApp(ctx, backgroundTick);
  } else if (choice == 1) {
    runSettingsApp(ctx, backgroundTick);
  } else if (choice == 2) {
    runFileExplorerApp(ctx, backgroundTick);
  } else if (choice == 3) {
    runNfcApp(ctx, backgroundTick);
  } else if (choice == 4) {
    runRfidApp(ctx, backgroundTick);
  }
}
