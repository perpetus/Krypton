// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "components/krypton_omnibox/google_suggest_compat.h"

#include <string>
#include <string_view>

#include "base/containers/fixed_flat_set.h"
#include "base/strings/string_number_conversions.h"
#include "components/google/core/common/google_tld_list.h"
#include "net/base/url_util.h"
#include "url/gurl.h"

namespace krypton::omnibox {
bool IsGoogleSearchProvider(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIs("https") ||
      url.EffectiveIntPort() != 443)
    return false;
  // Helium stubs google_util's URL classifiers globally. Use Chromium's
  // public TLD allowlist only for this opted-in suggestion path.
  constexpr auto suffixes =
      base::MakeFixedFlatSet<std::string_view>({GOOGLE_TLD_LIST});
  constexpr std::string_view label = "google.";
  std::string_view host = url.host();
  if (host.ends_with('.'))
    host.remove_suffix(1);
  const auto position = host.find(label);
  return position != std::string_view::npos &&
         (position == 0 || host[position - 1] == '.') &&
         suffixes.contains(host.substr(position + label.size()));
}
GURL UpgradeGoogleSuggestRequest(const GURL& search_url,
                                 const GURL& request_url,
                                 std::size_t cursor_position) {
  std::string client;
  if (!IsGoogleSearchProvider(search_url) ||
      !IsGoogleSearchProvider(request_url) ||
      request_url.path() != "/complete/search" ||
      !net::GetValueForKeyInQuery(request_url, "client", &client) ||
      client != "chrome")
    return request_url;
  // Match Chromium's desktop SEARCHBOX replacements for suggestClient and
  // suggestRid. Keep the remaining user-defined URL parameters intact.
  GURL url =
      net::AppendOrReplaceQueryParameter(request_url, "client", "chrome-omni");
  url = net::AppendOrReplaceQueryParameter(url, "gs_ri", "chrome-ext-ansg");
  url = net::AppendOrReplaceQueryParameter(url, "xssi", "t");
  if (cursor_position != std::u16string::npos)
    url = net::AppendOrReplaceQueryParameter(
        url, "cp", base::NumberToString(cursor_position));
  return url;
}
}  // namespace krypton::omnibox
