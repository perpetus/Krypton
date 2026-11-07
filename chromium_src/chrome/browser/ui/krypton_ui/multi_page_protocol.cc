// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/multi_page_protocol.h"
namespace krypton_ui {
bool HasCompleteMultiPageProtocol(const KrShellUI& api) {
  return KR_HAS_SLOT(&api, KrShellUI, layout_glance) &&
         api.multi_page_capabilities && api.upsert_split_group &&
         api.remove_split_group && api.set_glance && api.layout_split &&
         api.reduce_split && api.reduce_glance && api.show_split_chooser &&
         api.make_split && api.plan_split_copies &&
         api.get_multi_page_metrics && api.layout_glance &&
         (api.multi_page_capabilities(api.self) & KR_MULTI_PAGE_COMPLETE) ==
             KR_MULTI_PAGE_COMPLETE;
}
bool HasSnapLayoutProtocol(const KrShellUI& api) {
  return HasCompleteMultiPageProtocol(api) &&
         KR_HAS_SLOT(&api, KrShellUI, show_split_chooser_for_layout) &&
         api.show_split_chooser_for_layout &&
         (api.multi_page_capabilities(api.self) & KR_MULTI_PAGE_SNAP_LAYOUTS);
}
}  // namespace krypton_ui
