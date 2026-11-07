// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/multi_page_protocol.h"
#include "testing/gtest/include/gtest/gtest.h"
namespace krypton_ui {
namespace {
KrShellUI CompleteProtocol() {
  KrShellUI api{.struct_size = sizeof(KrShellUI)};
  api.multi_page_capabilities = [](void*) -> uint32_t {
    return KR_MULTI_PAGE_COMPLETE;
  };
  api.upsert_split_group = [](void*, const KrSplitGroup*) { return KR_OK; };
  api.remove_split_group = [](void*, KrSplitGroupId, uint64_t) {
    return KR_OK;
  };
  api.set_glance = [](void*, const KrGlanceState*) { return KR_OK; };
  api.layout_split = [](void*, const KrSplitLayoutRequest*, KrSplitGeometry*) {
    return KR_OK;
  };
  api.reduce_split = [](void*, const KrSplitGroup*, const KrSplitEdit*,
                        KrSplitGroup*) { return KR_OK; };
  api.reduce_glance = [](void*, const KrGlanceState*, const KrGlanceEdit*,
                         KrGlanceState*) { return KR_OK; };
  api.show_split_chooser = [](void*) { return KR_OK; };
  api.make_split = [](void*, const KrSplitCreateRequest*, KrSplitGroup*) {
    return KR_OK;
  };
  api.plan_split_copies = [](void*, const KrSplitCopyRequest*,
                             KrSplitCopyPlan*) { return KR_OK; };
  api.get_multi_page_metrics = [](void*, KrMultiPageMetrics*) { return KR_OK; };
  api.layout_glance = [](void*, KrRect, KrGlanceGeometry*) { return KR_OK; };
  return api;
}
TEST(MultiPageProtocol, RequiresEverySlotAndBothFeatureCapabilities) {
  auto api = CompleteProtocol();
  EXPECT_TRUE(HasCompleteMultiPageProtocol(api));
  api.layout_glance = nullptr;
  EXPECT_FALSE(HasCompleteMultiPageProtocol(api));
  api = CompleteProtocol();
  api.make_split = nullptr;
  EXPECT_FALSE(HasCompleteMultiPageProtocol(api));
  api = CompleteProtocol();
  api.multi_page_capabilities = [](void*) -> uint32_t {
    return KR_MULTI_PAGE_COMPLETE & ~KR_MULTI_PAGE_GLANCE;
  };
  EXPECT_FALSE(HasCompleteMultiPageProtocol(api));
  api.multi_page_capabilities = [](void*) -> uint32_t {
    return KR_MULTI_PAGE_COMPLETE | (1u << 31);
  };
  EXPECT_TRUE(HasCompleteMultiPageProtocol(api));
}
TEST(MultiPageProtocol, OlderAndPartialOutputTablesCannotAdvertiseSupport) {
  auto api = CompleteProtocol();
  api.struct_size = offsetof(KrShellUI, multi_page_capabilities);
  EXPECT_FALSE(HasCompleteMultiPageProtocol(api));
  api.struct_size =
      offsetof(KrShellUI, layout_glance) + sizeof(api.layout_glance) - 1;
  EXPECT_FALSE(HasCompleteMultiPageProtocol(api));
}

TEST(MultiPageProtocol, CardPresentationIsOptionalForLegacyModules) {
  auto api = CompleteProtocol();
  api.struct_size = offsetof(KrShellUI, layout_glance_card);
  EXPECT_TRUE(HasCompleteMultiPageProtocol(api));
  EXPECT_FALSE(KR_HAS_SLOT(&api, KrShellUI, layout_glance_card));
}
TEST(MultiPageProtocol, SnapLayoutsRequireCapabilityAndCompleteAppendedSlot) {
  auto api = CompleteProtocol();
  EXPECT_FALSE(HasSnapLayoutProtocol(api));
  api.multi_page_capabilities = [](void*) -> uint32_t {
    return KR_MULTI_PAGE_COMPLETE | KR_MULTI_PAGE_SNAP_LAYOUTS;
  };
  EXPECT_FALSE(HasSnapLayoutProtocol(api));
  api.show_split_chooser_for_layout = [](void*, KrSplitLayout) {
    return KR_OK;
  };
  EXPECT_TRUE(HasSnapLayoutProtocol(api));
  api.struct_size = offsetof(KrShellUI, show_split_chooser_for_layout) +
                    sizeof(api.show_split_chooser_for_layout) - 1;
  EXPECT_FALSE(HasSnapLayoutProtocol(api));
  EXPECT_TRUE(HasCompleteMultiPageProtocol(api));
  api.struct_size = sizeof(api);
  api.layout_split = nullptr;
  EXPECT_FALSE(HasSnapLayoutProtocol(api));
}
}  // namespace
}  // namespace krypton_ui
