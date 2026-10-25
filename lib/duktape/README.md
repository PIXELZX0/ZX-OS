# Duktape 2.7.0 (vendored)

Unmodified upstream `duktape.c` / `duktape.h` from the official 2.7.0 release
(MIT, see `LICENSE.txt`). Only `src/duk_config.h` is patched, because the
upstream `configure.py` needs Python 2:

1. `DUK_USE_INTERRUPT_COUNTER` is enabled and `DUK_USE_EXEC_TIMEOUT_CHECK`
   calls `zxjs_timeout_check()` (implemented in `src/core/js/js_engine.cpp`),
   so a runaway script (`while(true){}`) can be aborted.
2. An explicit ESP32 platform branch (newlib, little endian, no `strptime`).
