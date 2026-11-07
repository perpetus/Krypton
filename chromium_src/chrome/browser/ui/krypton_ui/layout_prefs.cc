// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/layout_prefs.h"
#include "chrome/browser/ui/krypton_ui/essential_prefs.h"
#include "chrome/browser/ui/krypton_ui/workspace_prefs.h"

#include <cmath>
#include <cstdint>

#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "krypton_ui/abi.h"

namespace krypton_ui {

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry) {
  RegisterWorkspacePrefs(registry);
  RegisterEssentialPrefs(registry);
  RegisterAppearanceProfilePrefs(registry);
  registry->RegisterBooleanPref(kLibraryMediaEnabledPref, false);
  registry->RegisterIntegerPref(kLayoutFlagsPref, 0);
  // Zero leaves the width to KryptonUI's default.
  registry->RegisterDoublePref(kSidebarWidthPref, 0);
}

KrLayoutPrefs ReadLayoutPrefs(const PrefService& prefs) {
  return {sizeof(KrLayoutPrefs),
          static_cast<uint32_t>(prefs.GetInteger(kLayoutFlagsPref)),
          static_cast<float>(prefs.GetDouble(kSidebarWidthPref))};
}

void WriteLayoutPrefs(PrefService& prefs, const KrLayoutPrefs& layout) {
  prefs.SetInteger(kLayoutFlagsPref, static_cast<int>(layout.flags));
  if (std::isfinite(layout.sidebar_width) && layout.sidebar_width >= 0) {
    prefs.SetDouble(kSidebarWidthPref, layout.sidebar_width);
  }
}

}  // namespace krypton_ui
