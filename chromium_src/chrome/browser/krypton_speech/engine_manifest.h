// Copyright 2026 The Krypton Authors. GPL-3.0.
// Generated from the verified local engine and official runtime payload.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace krypton::speech {
inline constexpr uint64_t kEngineSize = 416256;
inline constexpr char kEngineSha256[] =
    "78f454166289dc4bff2cd37e018f0427afb0f156ad570505b5ed6e8dfdb3e4d8";
struct RuntimeDependency {
  std::string_view name;
  uint64_t size;
  std::string_view sha256;
};
inline constexpr std::array<RuntimeDependency, 5> kRuntimeDependencies = {{
    {"onnxruntime.dll", 20602168,
     "348c3d29d6995eee37e3451825eaddb7ea7400be3afda6ca2286b12bc775f042"},
    {"msvcp140.dll", 643512,
     "7c26614e1d733892c2deac7e245ce115504b1d80592dd0a01b08e3e5a55f89ca"},
    {"msvcp140_1.dll", 35768,
     "206c931bf90fdad8816de3b5e2ef80b2bcaa9406c89ecc05fe6fddffe251e982"},
    {"vcruntime140.dll", 178616,
     "d1f4225df2cd877dbf130d5668a021dce3f94118455ff5ec952061c30afc9ce7"},
    {"vcruntime140_1.dll", 50112,
     "a7146c08f89fe5b04541ab507cdb59ff7b44534d4ba3c668a426c6450a03434e"},
}};
}  // namespace krypton::speech
