// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef COMPONENTS_KRYPTON_OMNIBOX_GOOGLE_SUGGEST_COMPAT_H_
#define COMPONENTS_KRYPTON_OMNIBOX_GOOGLE_SUGGEST_COMPAT_H_
#include <cstddef>

class GURL;
namespace krypton::omnibox {
inline constexpr std::size_t kSearchProviderMaxMatches = 10;
bool IsGoogleSearchProvider(const GURL& search_url);
// Upgrade only the legacy Google desktop endpoint. Caller retains Chromium's
// consent, OTR and request-source checks. Never adds a page URL, API key,
// session id, variations header, or changes a saved engine definition.
GURL UpgradeGoogleSuggestRequest(const GURL& search_url,
                                 const GURL& request_url,
                                 std::size_t cursor_position);
}  // namespace krypton::omnibox
#endif
