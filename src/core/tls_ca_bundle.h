#pragma once

#include <WiFiClientSecure.h>

// Mozilla root CA bundle that ESP-IDF links into libmbedtls
// (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL).
extern const uint8_t kTlsCaBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kTlsCaBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace tlsca {

inline void attach(WiFiClientSecure &client) {
  client.setCACertBundle(kTlsCaBundleStart,
                         static_cast<size_t>(kTlsCaBundleEnd - kTlsCaBundleStart));
}

}  // namespace tlsca
