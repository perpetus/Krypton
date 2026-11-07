// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/multi_page_features.h"
namespace krypton_ui {
// Glance and splits of up to four pages are enabled for normal launches.
// --disable-features=KryptonMultiPage restores Chromium's two-page split and
// Alt-click download behavior.
BASE_FEATURE(kKryptonMultiPage, base::FEATURE_ENABLED_BY_DEFAULT);
}  // namespace krypton_ui
