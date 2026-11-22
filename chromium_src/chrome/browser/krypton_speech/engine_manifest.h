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
    {"msvcp140.dll", 652136,
     "59d325227321d52b9a523ea401ce6787a5d59f78229a0ff3a6f124e5d3445685"},
    {"msvcp140_1.dll", 35696,
     "1c6522f4d005539109aa040f063659704bfda94a224142f50942573cfbcf7ec9"},
    {"vcruntime140.dll", 176488,
     "eb1ee979bf02324712346e5bdbfc59c785388bc50d18cc2688aae4b0734915a0"},
    {"vcruntime140_1.dll", 50544,
     "a14b4a42a2c6df04d6d53a8340816023d953406f7f3d94996c7cab30c414ff63"},
}};
}  // namespace krypton::speech
