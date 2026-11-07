// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "components/update_client/update_query_params.h"

#include <string>

#include "components/version_info/version_info.h"
#include "net/base/url_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace krypton_ui {
namespace {

TEST(KryptonWebstore, DirectDownloadsIncludeTheActualBrowserVersion) {
  const GURL request("https://clients2.google.com/service/update2/crx?" +
                     update_client::UpdateQueryParams::Get(
                         update_client::UpdateQueryParams::CRX_DUMMY));
  std::string value;
  ASSERT_TRUE(net::GetValueForKeyInQuery(request, "prodversion", &value));
  EXPECT_EQ(value, version_info::GetVersionNumber());
  ASSERT_TRUE(net::GetValueForKeyInQuery(request, "os", &value));
  EXPECT_EQ(value, "win");
  ASSERT_TRUE(net::GetValueForKeyInQuery(request, "arch", &value));
  EXPECT_EQ(value, "x64");
  ASSERT_TRUE(net::GetValueForKeyInQuery(request, "prod", &value));
  EXPECT_EQ(value, "chromecrx");
  ASSERT_TRUE(net::GetValueForKeyInQuery(request, "acceptformat", &value));
  EXPECT_EQ(value, "crx3,puff");
  EXPECT_FALSE(net::GetValueForKeyInQuery(request, "brand", &value));
  EXPECT_FALSE(net::GetValueForKeyInQuery(request, "ap", &value));
  EXPECT_FALSE(net::GetValueForKeyInQuery(request, "ping", &value));
}

} // namespace
} // namespace krypton_ui
