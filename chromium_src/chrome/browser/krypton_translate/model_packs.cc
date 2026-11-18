// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/krypton_translate/model_packs.h"

#include <algorithm>

namespace krypton::translation {
namespace {
#include "chrome/browser/krypton_translate/model_manifest.inc"
}  // namespace

std::span<const ModelPack> ModelPacks() {
  return kPacks;
}

const ModelPack* FindPack(PackKey key) {
  const auto found = std::ranges::find(kPacks, key, &ModelPack::key);
  return found == std::end(kPacks) ? nullptr : &*found;
}

std::string CanonicalLanguage(std::string_view code) {
  if (code == "he")
    return "iw";
  if (code == "nb")
    return "no";
  if (code == "zh-Hant" || code == "zh-TW" || code == "zh-HK")
    return "zh-Hant";
  if (code == "zh-Hans" || code == "zh-CN")
    return "zh";
  if (const auto dash = code.find('-'); dash != code.npos)
    code = code.substr(0, dash);
  return std::string(code);
}

std::optional<std::set<PackKey>> RequiredPacks(std::string_view source,
                                               std::string_view target) {
  const std::string from = CanonicalLanguage(source);
  const std::string to = CanonicalLanguage(target);
  if (from == to)
    return std::nullopt;
  std::set<PackKey> result;
  for (const auto& language : {from, to}) {
    if (language == "en")
      continue;
    const auto found =
        std::ranges::find(kPacks, language, &ModelPack::language);
    if (found == std::end(kPacks))
      return std::nullopt;
    result.insert(found->key);
  }
  return result;
}
}  // namespace krypton::translation
