// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/site_customization.h"
#include "components/pref_registry/pref_registry_syncable.h"

namespace krypton_ui {
void RegisterSiteCustomizationPrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterDictionaryPref(kSiteCustomizationPref);
}
}  // namespace krypton_ui
