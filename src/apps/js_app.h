#pragma once

#include <functional>

#include "app_context.h"

void runJsApp(AppContext &ctx, const std::function<void()> &backgroundTick);
