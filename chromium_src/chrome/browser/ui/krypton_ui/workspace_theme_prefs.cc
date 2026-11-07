// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/workspace_theme_prefs.h"

#include <cmath>
#include <iterator>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"

namespace krypton_ui {
namespace {
// ABI v2's documented editor range. The UI owns its drawing tokens and also
// validates each confirmation through KrShellUI.set_workspace_theme.
constexpr float kDefaultOpacity = 0.4f;
constexpr float kMinimumOpacity = 0.25f;
constexpr float kMaximumOpacity = 0.8f;

KrWorkspaceTheme Default(KrSpaceId space, uint32_t scheme) {
  KrWorkspaceTheme theme{};
  theme.struct_size = sizeof(theme);
  theme.space = space;
  theme.scheme = scheme;
  theme.opacity = kDefaultOpacity;
  theme.lightness = 50;
  return theme;
}
KrWorkspaceTheme Canonical(KrWorkspaceTheme theme) {
  theme.struct_size = sizeof(theme);
  for (size_t index = theme.point_count; index < std::size(theme.points);
       ++index) {
    theme.points[index] = {};
  }
  return theme;
}
}  // namespace

void RegisterWorkspaceThemePrefs(user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterDictionaryPref(kWorkspaceThemesPref);
}
bool ValidWorkspaceTheme(const KrWorkspaceTheme& theme) {
  if (!KR_HAS_FIELD(&theme, KrWorkspaceTheme, points) || !theme.space ||
      (theme.flags & ~KR_THEME_CUSTOMIZED) || theme.scheme > KR_THEME_DARK ||
      theme.harmony > KR_THEME_TRIADIC || theme.mode > KR_THEME_GRAYSCALE ||
      theme.point_count > std::size(theme.points) ||
      !std::isfinite(theme.opacity) || theme.opacity < kMinimumOpacity ||
      theme.opacity > kMaximumOpacity || !std::isfinite(theme.texture) ||
      theme.texture < 0 || theme.texture > 1 ||
      !std::isfinite(theme.lightness) || theme.lightness < 0 ||
      theme.lightness > 100) {
    return false;
  }
  for (size_t index = 0; index < theme.point_count; ++index) {
    const auto& point = theme.points[index];
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 ||
        point.x > 1 || point.y < 0 || point.y > 1) {
      return false;
    }
  }
  return true;
}
KrWorkspaceTheme ReadWorkspaceTheme(const PrefService& prefs,
                                    KrSpaceId space,
                                    uint32_t scheme) {
  auto theme = Default(space, scheme);
  const auto* row =
      prefs.GetDict(kWorkspaceThemesPref).FindDict(base::NumberToString(space));
  if (!row || row->FindInt("version") != 1) {
    return theme;
  }
  const auto flags = row->FindInt("flags");
  const auto harmony = row->FindInt("harmony");
  const auto mode = row->FindInt("mode");
  const auto opacity = row->FindDouble("opacity");
  const auto texture = row->FindDouble("texture");
  const auto lightness = row->FindDouble("lightness");
  const auto* points = row->FindList("points");
  if (!flags || !harmony || !mode || !opacity || !texture || !lightness ||
      !points || points->size() > std::size(theme.points)) {
    return theme;
  }
  theme.flags = *flags;
  theme.harmony = *harmony;
  theme.mode = *mode;
  theme.opacity = static_cast<float>(*opacity);
  theme.texture = static_cast<float>(*texture);
  theme.lightness = static_cast<float>(*lightness);
  for (const auto& value : *points) {
    const auto* point = value.GetIfDict();
    if (!point) {
      return Default(space, scheme);
    }
    const auto x = point->FindDouble("x");
    const auto y = point->FindDouble("y");
    const auto* color = point->FindString("color");
    uint32_t argb = 0;
    if (!x || !y || !color || !base::StringToUint(*color, &argb)) {
      return Default(space, scheme);
    }
    theme.points[theme.point_count++] = {static_cast<float>(*x),
                                         static_cast<float>(*y), argb};
  }
  return ValidWorkspaceTheme(theme) ? theme : Default(space, scheme);
}
bool SaveWorkspaceTheme(PrefService& prefs, const KrWorkspaceTheme& theme) {
  if (!ValidWorkspaceTheme(theme) ||
      !ReadWorkspaces(prefs).Contains(theme.space)) {
    return false;
  }
  if (!(theme.flags & KR_THEME_CUSTOMIZED)) {
    RemoveWorkspaceTheme(prefs, theme.space);
    return true;
  }
  base::DictValue row;
  row.Set("version", 1);
  row.Set("flags", static_cast<int>(theme.flags));
  row.Set("harmony", static_cast<int>(theme.harmony));
  row.Set("mode", static_cast<int>(theme.mode));
  row.Set("opacity", static_cast<double>(theme.opacity));
  row.Set("texture", static_cast<double>(theme.texture));
  row.Set("lightness", static_cast<double>(theme.lightness));
  base::ListValue points;
  for (size_t index = 0; index < theme.point_count; ++index) {
    const auto& point = theme.points[index];
    base::DictValue value;
    value.Set("x", static_cast<double>(point.x));
    value.Set("y", static_cast<double>(point.y));
    value.Set("color", base::NumberToString(point.color));
    points.Append(std::move(value));
  }
  row.Set("points", std::move(points));
  auto themes = prefs.GetDict(kWorkspaceThemesPref).Clone();
  themes.Set(base::NumberToString(theme.space), std::move(row));
  prefs.SetDict(kWorkspaceThemesPref, std::move(themes));
  return true;
}
void RemoveWorkspaceTheme(PrefService& prefs, KrSpaceId space) {
  auto themes = prefs.GetDict(kWorkspaceThemesPref).Clone();
  if (themes.Remove(base::NumberToString(space))) {
    prefs.SetDict(kWorkspaceThemesPref, std::move(themes));
  }
}
bool WorkspaceThemeState::Apply(PrefService& prefs,
                                const KrWorkspaceTheme& theme,
                                bool commit) {
  if (!ValidWorkspaceTheme(theme) ||
      !ReadWorkspaces(prefs).Contains(theme.space)) {
    return false;
  }
  if (commit) {
    if (!SaveWorkspaceTheme(prefs, theme)) {
      return false;
    }
    preview_.reset();
  } else {
    preview_ = Canonical(theme);
  }
  return true;
}
KrWorkspaceTheme WorkspaceThemeState::Resolve(const PrefService& prefs,
                                              KrSpaceId space,
                                              uint32_t scheme) const {
  auto theme = preview_ && preview_->space == space
                   ? *preview_
                   : ReadWorkspaceTheme(prefs, space, scheme);
  theme.scheme = preview_scheme().value_or(scheme);
  return theme;
}
std::optional<uint32_t> WorkspaceThemeState::preview_scheme() const {
  return preview_ ? std::make_optional(preview_->scheme) : std::nullopt;
}
const KrWorkspaceTheme* WorkspaceThemeState::preview() const {
  return preview_ ? &*preview_ : nullptr;
}
void WorkspaceThemeState::Retain(const WorkspaceCatalog& catalog) {
  if (preview_ && !catalog.Contains(preview_->space)) {
    preview_.reset();
  }
}
}  // namespace krypton_ui
