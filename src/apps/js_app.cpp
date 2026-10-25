#include "js_app.h"

#include <SD.h>

#include <string.h>

#include <vector>

#include "../core/js/js_engine.h"
#include "../core/sd_storage.h"
#include "../ui/ui_runtime.h"

namespace {

constexpr const char *kScriptsDir = "/scripts";
constexpr size_t kMaxLogLines = 60;
constexpr uint32_t kScriptTimeoutMs = 30000;

const char kSampleScript[] =
    "// ZX-OS JS framework sample.\n"
    "// Run this from the JS app to see ir/nfc/cc1101/gpio in action.\n"
    "print('board:', sys.board());\n"
    "print('heap:', JSON.stringify(sys.heapFree()));\n"
    "\n"
    "print('cc1101:', JSON.stringify(cc1101.info()));\n"
    "\n"
    "print('sending NEC(addr=0, cmd=0x45) on IR...');\n"
    "ir.send('NEC', 0, 0x45);\n"
    "\n"
    "print('scanning for an NFC tag (3s)...');\n"
    "var tag = nfc.scan(3000);\n"
    "print('nfc:', tag ? tag.uidHex : 'none found');\n";

bool listScripts(std::vector<String> &names, String &errorOut) {
  names.clear();
  if (!sdstorage::ensureMounted(&errorOut)) {
    return false;
  }
  if (!SD.exists(kScriptsDir)) {
    SD.mkdir(kScriptsDir);
    File sample = SD.open(String(kScriptsDir) + "/sample.js", FILE_WRITE);
    if (sample) {
      sample.write(reinterpret_cast<const uint8_t *>(kSampleScript),
                   sizeof(kSampleScript) - 1);
      sample.close();
    }
  }
  File dir = SD.open(kScriptsDir, FILE_READ);
  if (!dir || !dir.isDirectory()) {
    errorOut = "cannot open " + String(kScriptsDir);
    return false;
  }
  for (File entry = dir.openNextFile(); entry;
      entry = dir.openNextFile()) {
    if (!entry.isDirectory()) {
      const char *entryName = entry.name();
      const char *base = strrchr(entryName, '/');
      String name = base ? base + 1 : entryName;
      if (name.endsWith(".js")) {
        names.push_back(name);
      }
    }
    entry.close();
  }
  dir.close();
  return true;
}

void appendClipped(std::vector<String> &lines, const String &text) {
  if (lines.size() >= kMaxLogLines) {
    return;
  }
  lines.push_back(text.length() > 60 ? text.substring(0, 60) + "..." : text);
}

void runScriptFile(AppContext &ctx,
                   const String &path,
                   const std::function<void()> &backgroundTick) {
  std::vector<String> log;

  zxjs::Host host;
  host.print = [&log](const String &line) { appendClipped(log, line); };
  host.yield = backgroundTick;
  host.cancelRequested = [&ctx]() {
    const UiEvent ev = ctx.uiRuntime->pollInput();
    return ev.back;
  };
  host.ui = ctx.uiRuntime;

  ctx.uiRuntime->showProgressOverlay("JS", "Running " + path);
  const zxjs::Result result =
      zxjs::runFile(path, host, kScriptTimeoutMs);
  ctx.uiRuntime->hideProgressOverlay();

  std::vector<String> lines = log;
  if (result.ok) {
    lines.push_back("-- OK --");
    if (result.value.length() && result.value != "undefined") {
      appendClipped(lines, "= " + result.value);
    }
  } else if (result.cancelled) {
    lines.push_back("-- stopped (BACK) --");
  } else if (result.timedOut) {
    lines.push_back("-- timeout (30s) --");
  } else {
    lines.push_back("-- ERROR --");
    appendClipped(lines, result.error);
  }

  ctx.uiRuntime->showInfo("JS: " + path, lines, backgroundTick, "OK/BACK Exit");
}

void runScriptsMenu(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  int selected = 0;
  while (true) {
    std::vector<String> names;
    String err;
    if (!listScripts(names, err)) {
      ctx.uiRuntime->showToast("JS", err, 1700, backgroundTick);
      return;
    }
    if (names.empty()) {
      ctx.uiRuntime->showToast("JS", "No .js files in /scripts", 1700,
                               backgroundTick);
      return;
    }

    std::vector<String> menu = names;
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("JS Scripts", menu, selected,
                                               backgroundTick,
                                               "OK Run  BACK Exit", "/scripts");
    if (choice < 0 || choice == static_cast<int>(names.size())) {
      return;
    }
    selected = choice;
    runScriptFile(ctx, String(kScriptsDir) + "/" + names[choice], backgroundTick);
  }
}

}  // namespace

void runJsApp(AppContext &ctx, const std::function<void()> &backgroundTick) {
  if (zxjs::isBusy()) {
    ctx.uiRuntime->showToast("JS", "A script is already running", 1500,
                             backgroundTick);
    return;
  }
  runScriptsMenu(ctx, backgroundTick);
}
