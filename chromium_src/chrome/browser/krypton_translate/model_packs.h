// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_KRYPTON_TRANSLATE_MODEL_PACKS_H_
#define CHROME_BROWSER_KRYPTON_TRANSLATE_MODEL_PACKS_H_

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

#include "components/on_device_translation/public/language_pack.h"

namespace krypton::translation {
using PackKey = on_device_translation::LanguagePackKey;
struct ModelFile {
  std::string_view path;
  std::string_view url;
  std::string_view sha256;
  std::string_view decoded_sha256;
  uint64_t size;
  uint64_t decoded_size;
};
struct ModelPack {
  PackKey key;
  std::string_view language;
  std::string_view revision;
  std::span<const ModelFile> files;
  uint64_t download_bytes;
};
std::span<const ModelPack> ModelPacks();
const ModelPack* FindPack(PackKey key);
std::string CanonicalLanguage(std::string_view code);
std::optional<std::set<PackKey>> RequiredPacks(std::string_view source,
                                               std::string_view target);
}  // namespace krypton::translation

#endif  // CHROME_BROWSER_KRYPTON_TRANSLATE_MODEL_PACKS_H_
