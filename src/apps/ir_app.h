#pragma once

#include <functional>

#include "app_context.h"

void runIrApp(AppContext &ctx, const std::function<void()> &backgroundTick);
