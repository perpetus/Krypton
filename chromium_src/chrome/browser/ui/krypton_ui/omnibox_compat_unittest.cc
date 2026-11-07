// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "components/krypton_omnibox/google_suggest_compat.h"

#include <string>

#include "net/base/url_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace krypton::omnibox {
namespace {
TEST(KryptonGoogleSuggestions,
     UpgradesTheUserReportedEndpointWithoutContextData) {
  const GURL engine("https://www.google.com/search?q=hello");
  const auto result = UpgradeGoogleSuggestRequest(
      engine,
      GURL("https://suggestqueries.google.com/complete/"
           "search?client=chrome&q=hello&hl=en"),
      5);
  std::string value;
  ASSERT_TRUE(net::GetValueForKeyInQuery(result, "client", &value));
  EXPECT_EQ(value, "chrome-omni");
  ASSERT_TRUE(net::GetValueForKeyInQuery(result, "gs_ri", &value));
  EXPECT_EQ(value, "chrome-ext-ansg");
  ASSERT_TRUE(net::GetValueForKeyInQuery(result, "cp", &value));
  EXPECT_EQ(value, "5");
  ASSERT_TRUE(net::GetValueForKeyInQuery(result, "q", &value));
  EXPECT_EQ(value, "hello");
  ASSERT_TRUE(net::GetValueForKeyInQuery(result, "hl", &value));
  EXPECT_EQ(value, "en");
  EXPECT_FALSE(net::GetValueForKeyInQuery(result, "sugkey", &value));
  EXPECT_FALSE(net::GetValueForKeyInQuery(result, "url", &value));
  EXPECT_FALSE(net::GetValueForKeyInQuery(result, "psi", &value));
}
TEST(KryptonGoogleSuggestions,
     LeavesCustomEnginesClientsAndInsecureEndpointsAlone) {
  for (const auto* endpoint :
       {"https://www.google.com/complete/search?client=firefox&q=hello",
        "https://www.google.com/complete/search?client=chrome-omni&q=hello",
        "https://www.google.com/other?client=chrome&q=hello",
        "http://www.google.com/complete/search?client=chrome&q=hello",
        "https://www.google.com:8443/complete/search?client=chrome&q=hello",
        "https://google.com.example/complete/search?client=chrome&q=hello"}) {
    const GURL url(endpoint);
    EXPECT_EQ(UpgradeGoogleSuggestRequest(
                  GURL("https://www.google.com/search?q=hello"), url, 5),
              url);
  }
  const GURL url(
      "https://www.google.com/complete/search?client=chrome&q=hello");
  EXPECT_EQ(UpgradeGoogleSuggestRequest(
                GURL("https://example.test/search?q=hello"), url, 5),
            url);
}
TEST(KryptonGoogleSuggestions,
     PreservesTheEndpointAndOmitsUnknownCursorPosition) {
  const auto result = UpgradeGoogleSuggestRequest(
      GURL("https://www.google.co.in/search?q=hello"),
      GURL("https://suggestqueries.google.com/complete/"
           "search?client=chrome&q=hello"),
      std::u16string::npos);
  EXPECT_EQ(result.host(), "suggestqueries.google.com");
  std::string value;
  EXPECT_FALSE(net::GetValueForKeyInQuery(result, "cp", &value));
}
}  // namespace
}  // namespace krypton::omnibox
