// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.

#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

#include <windows.h>

#include <dwmapi.h>

#include "chrome/browser/ui/krypton_ui/dictation_controller.h"
#include "chrome/browser/ui/krypton_ui/frame_material.h"
#include "chrome/browser/ui/krypton_ui/live_folders.h"
#include "chrome/browser/ui/krypton_ui/multi_page_controller.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"
#include "chrome/browser/ui/krypton_ui/site_customization.h"
#include "chrome/browser/ui/krypton_ui/space_hosts.h"
#include "chrome/browser/ui/krypton_ui/space_profiles.h"
#include "chrome/browser/ui/krypton_ui/translation_menu_model.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_client.h"

// winnt.h's macro must not rewrite Abseil's StoreFence helper.
#undef StoreFence
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/krypton_agent/agent_session.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search/search.h"
#include "chrome/browser/sessions/exit_type_service.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/actions/chrome_action_properties.h"
#include "chrome/browser/ui/browser_actions.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/bubble_anchor_util.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_manager.h"
#include "chrome/browser/ui/exclusive_access/fullscreen_controller.h"
#include "chrome/browser/ui/krypton_ui/agent_panel.h"
#include "chrome/browser/ui/krypton_ui/browser_service_model.h"
#include "chrome/browser/ui/krypton_ui/browser_services.h"
#include "chrome/browser/ui/krypton_ui/command_bar_entry.h"
#include "chrome/browser/ui/krypton_ui/document_store.h"
#include "chrome/browser/ui/krypton_ui/essential_prefs.h"
#include "chrome/browser/ui/krypton_ui/layout_prefs.h"
#include "chrome/browser/ui/krypton_ui/module.h"
#include "chrome/browser/ui/krypton_ui/native_backdrop.h"
#include "chrome/browser/ui/krypton_ui/native_extensions.h"
#include "chrome/browser/ui/krypton_ui/native_glass_backdrop.h"
#include "chrome/browser/ui/krypton_ui/native_location_bar.h"
#include "chrome/browser/ui/krypton_ui/native_menus.h"
#include "chrome/browser/ui/krypton_ui/native_toolbar.h"
#include "chrome/browser/ui/krypton_ui/page_capture.h"
#include "chrome/browser/ui/krypton_ui/privacy_features_bridge.h"
#include "chrome/browser/ui/krypton_ui/shortcut_settings.h"
#include "chrome/browser/ui/krypton_ui/sidebar_services.h"
#include "chrome/browser/ui/krypton_ui/site_data_menu_model.h"
#include "chrome/browser/ui/krypton_ui/site_permissions.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/page_info/page_info_dialog.h"
#include "chrome/browser/ui/performance_controls/tab_resource_usage_collector.h"
#include "chrome/browser/ui/performance_controls/tab_resource_usage_tab_helper.h"
#include "chrome/browser/ui/tab_ui_helper.h"
#include "chrome/browser/ui/tabs/alert/tab_alert_controller.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/thumbnails/thumbnail_tab_helper.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/site_data/page_specific_site_data_dialog.h"
#include "chrome/browser/ui/views/tabs/hovercard/hover_card_anchor_target.h"
#include "chrome/browser/ui/views/tabs/hovercard/tab_hover_card_thumbnail_observer.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/generated_resources.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/omnibox/browser/location_bar_model.h"
#include "components/prefs/pref_service.h"
#include "components/security_state/core/security_state.h"
#include "components/tabs/public/tab_interface.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "components/zoom/zoom_controller.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/actions/actions.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/image_model.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/display/win/screen_win.h"
#include "ui/events/event_constants.h"
#include "ui/events/win/events_win_utils.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/native_theme/native_theme.h"
#include "ui/views/widget/widget.h"
#include "ui/views/win/hwnd_util.h"

namespace krypton_ui {
namespace {
constexpr KrActionId kZoomResetAction = uint64_t{9} << 56;
KrStr Borrow(std::string_view text) { return {text.data(), text.size()}; }
std::string_view Read(KrStr text) {
  return text.data ? std::string_view(text.data, text.size)
                   : std::string_view();
}
WindowOpenDisposition Disposition(uint32_t value) {
  switch (value) {
  case KR_OPEN_NEW_TAB:
    return WindowOpenDisposition::kNewForegroundTab;
  case KR_OPEN_NEW_BACKGROUND_TAB:
    return WindowOpenDisposition::kNewBackgroundTab;
  case KR_OPEN_NEW_WINDOW:
    return WindowOpenDisposition::kNewWindow;
  default:
    return WindowOpenDisposition::kCurrentTab;
  }
}
uint32_t Security(security_state::SecurityLevel level) {
  switch (level) {
  case security_state::SECURE:
    return KR_SECURITY_SECURE;
  case security_state::WARNING:
    return KR_SECURITY_WARNING;
  case security_state::DANGEROUS:
    return KR_SECURITY_DANGEROUS;
  default:
    return KR_SECURITY_NONE;
  }
}
} // namespace

class BrowserAdapter::Tab final : public content::WebContentsObserver {
public:
  Tab(BrowserAdapter &owner, tabs::TabInterface &tab)
      : content::WebContentsObserver(tab.GetContents()), owner_(owner),
        tab_(tab.GetWeakPtr()),
        discard_(tab.RegisterWillDiscardContents(
            base::BindRepeating(&Tab::Discard, base::Unretained(this)))) {}
  ~Tab() override { owner_->images_.Release(icon); }
  void Replaced(content::WebContents *contents) {
    SetEssentialForTab(contents, essential);
    SetTabIconForTab(contents, glyph);
    SetFolderForTab(contents, folder);
    if (!WorkspaceForTab(contents)) {
      SetWorkspaceForTab(contents, space);
    }
    Observe(contents);
  }
  base::WeakPtr<tabs::TabInterface> tab() const { return tab_; }
  std::string title;
  std::string url;
  std::string glyph;
  ui::ImageModel image;
  KrImageId icon = 0;
  uint32_t flags = 0;
  KrSpaceId space = 0;
  uint64_t essential = 0;
  KrFolderId folder = 0;
  uint64_t navigation = 0;
  double progress = 1;
  bool published = false;
  bool image_dirty = false;
  bool primary_loading = false;

private:
  void Discard(tabs::TabInterface *, content::WebContents *old,
               content::WebContents *next) {
    SetWorkspaceForTab(next, space);
    SetFolderForTab(next, folder);
    SetEssentialForTab(next, EssentialForTab(old));
    SetTabIconForTab(next, TabIconForTab(old));
    if (const auto split = SplitSessionForTab(old)) {
      SetSplitSessionForTab(next, split);
    }
    if (IsTransientGlance(old)) {
      SetTransientGlance(next, true, GlanceSessionParent(old));
    }
    Observe(next);
  }
  void Changed() {
    if (tab_) {
      owner_->PublishTab(*tab_);
    }
  }
  void TitleWasSet(content::NavigationEntry *) override { Changed(); }
  void OnAudioStateChanged(bool) override { Changed(); }
  void DidStartLoading() override {
    Changed();
    owner_->PublishNavigation();
  }
  void DidStopLoading() override {
    primary_loading = false;
    progress = 1;
    Changed();
    owner_->PublishNavigation();
    if (tab_) {
      owner_->PublishProgress(*tab_);
    }
  }
  void DidChangeVisibleSecurityState() override { owner_->PublishNavigation(); }
  void NavigationEntriesDeleted() override {
    if (tab_ && tab_->IsActivated()) {
      owner_->PublishNavigation();
    }
  }
  void DidStartNavigation(content::NavigationHandle *handle) override {
    if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument()) {
      return;
    }
    primary_loading = true;
    navigation = handle->GetNavigationId();
    progress = 0;
    owner_->PublishNavigation();
    if (tab_) {
      owner_->PublishProgress(*tab_);
    }
  }
  void ReadyToCommitNavigation(content::NavigationHandle *handle) override {
    if (owner_->site_customization_) {
      owner_->site_customization_->NavigationReady(*handle);
    }
  }
  void DidFinishNavigation(content::NavigationHandle *handle) override {
    if (tab_ && owner_->site_customization_) {
      owner_->site_customization_->NavigationFinished(*tab_, *handle);
    }
    if (!handle->IsInPrimaryMainFrame()) {
      return;
    }
    Changed();
    owner_->PublishNavigation();
    if (tab_ && tab_->IsActivated() && handle->HasCommitted() &&
        !handle->IsSameDocument()) {
      owner_->ScheduleActions();
    }
    owner_->SchedulePageBackgrounds();
    if (tab_) {
      owner_->UpdateEssential(*tab_);
    }
  }
  void LoadProgressChanged(double value) override {
    if (!primary_loading) {
      return;
    }
    progress = std::max(progress, value);
    if (tab_) {
      owner_->PublishProgress(*tab_);
    }
  }
  const raw_ref<BrowserAdapter> owner_;
  const base::WeakPtr<tabs::TabInterface> tab_;
  base::CallbackListSubscription discard_;
};

// Reuse Chromium's hover-card projection and providers without creating a
// Views bubble. Subscriptions and decoded images exist only during a hover.
class BrowserAdapter::TabPreview final : public HoverCardAnchorTarget {
public:
  explicit TabPreview(BrowserAdapter &owner)
      : HoverCardAnchorTarget(&*owner.view_), owner_(owner) {}
  ~TabPreview() override { Stop(); }

  void Start(uint32_t request, tabs::TabInterface &tab) {
    Stop();
    request_ = request;
    tab_ = tab.GetWeakPtr();
    thumbnail_changed_ = thumbnails_.AddCallback(
        base::BindRepeating(&TabPreview::OnImage, weak_factory_.GetWeakPtr()));
    data_ = std::make_unique<tabs::TabDataObserver>(&tab);
    data_changed_ = data_->RegisterTabDataChangedCallback(
        base::BindRepeating(&TabPreview::OnData, weak_factory_.GetWeakPtr()));
    Update(data_->tab_data());
    // This is an asynchronous, tab-scoped attribution query. Subsequent
    // results arrive through TabDataObserver, including changes after load.
    TabResourceUsageCollector::Get()->ImmediatelyRefreshMetrics(
        tab.GetContents());
  }

  void Stop(uint32_t request = 0) {
    if (request && request != request_) {
      return;
    }
    request_ = 0;
    weak_factory_.InvalidateWeakPtrs();
    data_changed_ = {};
    data_.reset();
    thumbnail_changed_ = {};
    thumbnails_.Observe(nullptr);
    owner_->images_.Release(std::exchange(image_, 0));
    tab_.reset();
  }

  void Removed(KrTabId tab) {
    if (tab_ && Identity(*tab_) == tab) {
      Stop();
    }
  }

  bool NeedsToShowThumbnail() const override { return true; }
  bool IsValidHoverCardTarget() const override { return tab_ != nullptr; }
  views::BubbleAnchor GetAnchor() override {
    return views::BubbleAnchor(&*owner_->view_);
  }
  views::BubbleBorder::Arrow GetAnchorPosition() const override {
    return views::BubbleBorder::TOP_LEFT;
  }

private:
  void OnData(TabChangeType, const tabs::TabData &data) { Update(data); }

  void Update(const tabs::TabData &data) {
    if (!tab_ || !request_) {
      return;
    }
    SetHoverCardDataFrom(data);
    const auto &card = std::get<TabCardData>(this->data());
    const auto &local_prefs = *g_browser_process->local_state();
    flags_ = KR_TAB_PREVIEW_LABELS_PROVIDED;
    if (card.is_tab_discarded) {
      flags_ |= KR_TAB_PREVIEW_SLEEPING;
    }
    const bool show_image =
        base::FeatureList::IsEnabled(features::kTabHoverCardImages) &&
        local_prefs.GetBoolean(prefs::kHoverCardImagesEnabled) &&
        !card.is_crashed &&
        (!card.is_tab_discarded ||
         (card.thumbnail && card.thumbnail->has_data()));
    if (!show_image) {
      flags_ |= KR_TAB_PREVIEW_NO_IMAGE;
    }
    title_ = base::UTF16ToUTF8(card.title_data.text);
    domain_ = base::UTF16ToUTF8(card.domain_data.text);
    memory_bytes_ = card.tab_resource_usage
                        ? card.tab_resource_usage->memory_usage().InBytes()
                        : 0;
    memory_text_.clear();
    if (card.tab_resource_usage && memory_bytes_ > 0) {
      flags_ |= KR_TAB_PREVIEW_MEMORY_KNOWN;
      const bool high = card.tab_resource_usage->is_high_memory_usage();
      if ((local_prefs.GetBoolean(prefs::kHoverCardMemoryUsageEnabled) ||
           high) &&
          !card.show_discard_status && !card.show_collaboration_messaging) {
        memory_text_ = base::UTF16ToUTF8(l10n_util::GetStringFUTF16(
            high ? IDS_HOVERCARD_TAB_HIGH_MEMORY_USAGE
                 : IDS_HOVERCARD_TAB_MEMORY_USAGE,
            ui::FormatBytes(card.tab_resource_usage->memory_usage())));
      }
    }
    status_.clear();
    if (card.alert_data.should_show_discard_status) {
      status_ = base::UTF16ToUTF8(
          card.alert_data.memory_savings.is_positive()
              ? l10n_util::GetStringFUTF16(
                    IDS_HOVERCARD_INACTIVE_TAB_MEMORY_SAVINGS,
                    ui::FormatBytes(card.alert_data.memory_savings))
              : l10n_util::GetStringUTF16(IDS_HOVERCARD_INACTIVE_TAB));
    } else if (card.alert_data.alert_state.has_value()) {
      status_ =
          base::UTF16ToUTF8(tabs::TabAlertController::GetTabAlertStateText(
              *card.alert_data.alert_state));
    }
    if (card.show_collaboration_messaging) {
      if (!status_.empty()) {
        status_ += "\n";
      }
      status_ += base::UTF16ToUTF8(card.collaboration_message);
    }
    auto thumbnail = show_image ? card.thumbnail : nullptr;
    const bool changed_image = thumbnail != thumbnails_.current_image();
    KrImageId previous_image = 0;
    if (changed_image || !thumbnail || !thumbnail->has_data()) {
      previous_image = std::exchange(image_, 0);
    }
    Send();
    owner_->images_.Release(previous_image);
    if (changed_image) {
      thumbnails_.Observe(std::move(thumbnail));
    }
  }

  void OnImage(TabHoverCardThumbnailObserver *, gfx::ImageSkia image) {
    if (!tab_ || !request_ || image.isNull()) {
      return;
    }
    auto acquired = owner_->AcquireImage(ui::ImageModel::FromImageSkia(image));
    if (!acquired.has_value()) {
      LOG(ERROR) << acquired.error();
      return;
    }
    const auto previous = std::exchange(image_, *acquired);
    Send();
    owner_->images_.Release(previous);
  }

  void Send() {
    const auto *api = owner_->session_.api();
    if (!tab_ || !request_ || !KR_HAS_SLOT(api, KrShellUI, set_tab_preview) ||
        !api->set_tab_preview) {
      return;
    }
    const KrTabPreview preview{
        sizeof(preview), Identity(*tab_), image_,          memory_bytes_,
        flags_,          Borrow(title_),  Borrow(domain_), Borrow(memory_text_),
        Borrow(status_)};
    std::ignore = api->set_tab_preview(api->self, request_, &preview);
  }

  const raw_ref<BrowserAdapter> owner_;
  base::WeakPtr<tabs::TabInterface> tab_;
  std::unique_ptr<tabs::TabDataObserver> data_;
  base::CallbackListSubscription data_changed_;
  TabHoverCardThumbnailObserver thumbnails_;
  base::CallbackListSubscription thumbnail_changed_;
  uint32_t request_ = 0;
  uint32_t flags_ = 0;
  uint64_t memory_bytes_ = 0;
  KrImageId image_ = 0;
  std::string title_, domain_, memory_text_, status_;
  base::WeakPtrFactory<TabPreview> weak_factory_{this};
};

struct BrowserAdapter::Action {
  base::WeakPtr<actions::ActionItem> source;
  base::CallbackListSubscription subscription;
  std::string label;
  std::string tooltip;
  ui::ImageModel image;
  KrImageId icon = 0;
  bool image_dirty = false;
};

bool BrowserAdapter::IsEligible(const BrowserWindowInterface &browser) {
  return browser.GetType() == BrowserWindowInterface::Type::TYPE_NORMAL;
}

// Bounded, one-shot snapshots. Copy compressed background data into a detached
// ThumbnailImage so observing it cannot start a renderer/video capture. Active
// content is copied once from Viz, without invalidating or painting the page.
class BrowserAdapter::SwitcherPreview final : public ThumbnailImage::Delegate {
public:
  SwitcherPreview(BrowserAdapter &owner, uint32_t request,
                  tabs::TabInterface &tab)
      : owner_(owner), tab_(tab.GetWeakPtr()), request_(request),
        url_(tab.GetContents()->GetVisibleURL()) {}
  ~SwitcherPreview() override { owner_->images_.Release(image_); }
  void Start() {
    auto *contents = tab_->GetContents();
    const auto &prefs = *g_browser_process->local_state();
    if (contents->IsCrashed() ||
        !prefs.GetBoolean(prefs::kHoverCardImagesEnabled) ||
        (ReadAppearancePrefs(owner_->WindowPrefs()).flags &
         KR_APPEARANCE_HIDE_HOVER_IMAGES)) {
      return;
    }
    if (tab_->IsActivated()) {
      auto *view = contents->GetRenderWidgetHostView();
      if (view && view->IsSurfaceAvailableForCopy()) {
        const auto source = view->GetViewBounds().size();
        const float scale = std::min(480.0f / std::max(1, source.width()),
                                     270.0f / std::max(1, source.height()));
        const gfx::Size output(
            std::max(1, static_cast<int>(source.width() * scale)),
            std::max(1, static_cast<int>(source.height() * scale)));
        view->CopyFromSurface(
            gfx::Rect(), output, base::Milliseconds(750),
            base::BindPostTaskToCurrentDefault(base::BindOnce(
                &SwitcherPreview::OnSurface, weak_factory_.GetWeakPtr())));
        return;
      }
    }
    if (const auto *helper = ThumbnailTabHelper::From(tab_.get());
        helper && helper->thumbnail()->has_data()) {
      cached_ = base::MakeRefCounted<ThumbnailImage>(
          this, helper->thumbnail()->data());
      subscription_ = cached_->Subscribe();
      subscription_->SetSizeHint(gfx::Size(480, 270));
      subscription_->SetUncompressedImageCallback(base::BindRepeating(
          &SwitcherPreview::OnImage, weak_factory_.GetWeakPtr()));
      cached_->RequestThumbnailImage();
    }
  }

private:
  // ThumbnailImage always requires a live delegate, even for a detached cached
  // snapshot. This delegate owns no capture driver and cannot wake the tab.
  // It outlives cached_ and subscription_, which are destroyed before the base.
  void ThumbnailImageBeingObservedChanged(bool) override {}
  ThumbnailImage::CaptureReadiness GetCaptureReadiness() const override {
    return ThumbnailImage::CaptureReadiness::kReadyForFinalCapture;
  }

  void OnSurface(const content::CopyFromSurfaceResult &result) {
    if (result.has_value()) {
      OnImage(gfx::ImageSkia::CreateFrom1xBitmap(result->bitmap));
    }
  }
  void OnImage(gfx::ImageSkia image) {
    if (image_ || image.isNull() || !tab_ ||
        owner_->switcher_request_ != request_ ||
        tab_->GetContents()->GetVisibleURL() != url_) {
      return;
    }
    // Bound decoded/UI-side pixels even if a cached thumbnail was much larger.
    const auto size = image.size();
    if (size.width() > 480 || size.height() > 270) {
      const float scale =
          std::min(480.0f / size.width(), 270.0f / size.height());
      image = gfx::ImageSkiaOperations::CreateResizedImage(
          image, skia::ImageOperations::RESIZE_GOOD,
          gfx::Size(std::max(1, static_cast<int>(size.width() * scale)),
                    std::max(1, static_cast<int>(size.height() * scale))));
    }
    const auto acquired =
        owner_->AcquireImage(ui::ImageModel::FromImageSkia(image));
    if (!acquired.has_value()) {
      return;
    }
    image_ = *acquired;
    if (const auto *api = owner_->session_.api(); api && api->set_tab_preview) {
      const KrTabPreview value{
          sizeof(KrTabPreview), Identity(*tab_), image_, 0, 0, {}, {}, {}, {}};
      std::ignore = api->set_tab_preview(api->self, request_, &value);
    }
  }
  const raw_ref<BrowserAdapter> owner_;
  base::WeakPtr<tabs::TabInterface> tab_;
  const uint32_t request_;
  const GURL url_;
  KrImageId image_ = 0;
  scoped_refptr<ThumbnailImage> cached_;
  std::unique_ptr<ThumbnailImage::Subscription> subscription_;
  base::WeakPtrFactory<SwitcherPreview> weak_factory_{this};
};

BrowserAdapter::BrowserAdapter(BrowserView &view)
    : view_(view), browser_(*view.browser()),
      session_(Module::Get(),
               base::SingleThreadTaskRunner::GetCurrentDefault()),
      anchors_(view), images_(session_) {
  // Before BrowserWidget asks for its native widget: a Space host for another
  // profile becomes a child of its frame instead of a top-level window.
  InitializeSpaceHost();
  auto backdrop = std::make_unique<NativeBackdrop>();
  backdrop_ = backdrop.get();
  view.SetBackground(std::move(backdrop));
  auto changed = base::BindRepeating(&BrowserAdapter::ScheduleActions,
                                     weak_factory_.GetWeakPtr());
  auto &commands = *chrome::BrowserCommandController::From(&*browser_);
  auto publish_image = base::BindRepeating(
      [](base::WeakPtr<BrowserAdapter> owner, const ui::ImageModel &image)
          -> base::expected<KrImageId, std::string> {
        if (!owner) {
          return base::unexpected("The browser window is closed.");
        }
        return owner->AcquireImage(image);
      },
      weak_factory_.GetWeakPtr());
  auto release_image =
      base::BindRepeating(&NativeImages::Release, images_.GetWeakPtr());
  menus_ = std::make_unique<NativeMenus>(
      *browser_, session_, publish_image, release_image,
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner,
             KrSpaceId target) -> std::unique_ptr<ui::MenuModel> {
            return owner ? owner->CreateWorkspaceMenu(target) : nullptr;
          },
          weak_factory_.GetWeakPtr()),
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner,
             KrActionId target) -> std::unique_ptr<ui::MenuModel> {
            return owner && owner->extensions_
                       ? owner->extensions_->CreateMenu(target)
                       : nullptr;
          },
          weak_factory_.GetWeakPtr()),
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner, uint32_t kind, uint64_t zone,
             tabs::TabInterface *target) -> std::unique_ptr<ui::MenuModel> {
            return owner ? owner->CreateContextMenu(kind, target, zone)
                         : nullptr;
          },
          weak_factory_.GetWeakPtr()),
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner, tabs::TabInterface &tab) {
            if (owner) {
              owner->SetEssential(tab, !EssentialForTab(tab.GetContents()));
            }
          },
          weak_factory_.GetWeakPtr()));
  menus_->SetExternalMenuFactory(base::BindRepeating(
      [](base::WeakPtr<BrowserAdapter> owner, uint32_t kind,
         uint64_t target) -> std::unique_ptr<ui::MenuModel> {
        return owner && owner->sidebar_
                   ? owner->sidebar_->CreateMenu(kind, target)
                   : nullptr;
      },
      weak_factory_.GetWeakPtr()));
  location_ = std::make_unique<NativeLocationBar>(
      *browser_, commands, *LocationBarModel::From(&*browser_), session_,
      anchors_, view, publish_image, release_image, changed);
  location_->SetCommandProvider(base::BindRepeating(
      [](base::WeakPtr<BrowserAdapter> owner, std::string_view query) {
        return owner ? owner->CommandBarEntries(query)
                     : std::vector<CommandBarEntry>();
      },
      weak_factory_.GetWeakPtr()));
  toolbar_ = view.AddChildView(std::make_unique<NativeToolbar>(
      *browser_, commands, view, session_, anchors_, std::move(publish_image),
      std::move(release_image), std::move(changed)));
}

BrowserAdapter::~BrowserAdapter() {
  dictation_.reset();
  Close();
  location_.reset();
  view_->RemoveChildViewT(toolbar_.ExtractAsDangling());
}

void BrowserAdapter::InitializeServices() {
  CHECK(!services_initialized_);
  services_initialized_ = true;
  toolbar_->Initialize();
  location_->Initialize();
  extensions_ = std::make_unique<NativeExtensions>(
      *browser_, *toolbar_, session_, anchors_,
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner, const ui::ImageModel &image)
              -> base::expected<KrImageId, std::string> {
            return owner ? owner->AcquireImage(image)
                         : base::unexpected("The browser window is closed.");
          },
          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&NativeImages::Release, images_.GetWeakPtr()),
      base::BindRepeating(&BrowserAdapter::ScheduleActions,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner) {
            if (owner) {
              owner->menus_->CloseAll();
              owner->toolbar_->CloseMenu();
            }
          },
          weak_factory_.GetWeakPtr()));
  toolbar_->SetExtensionsContainer(extensions_.get());
}

base::expected<void, std::string> BrowserAdapter::Attach() {
  CHECK(services_initialized_);
  if (closed_) {
    return base::unexpected("The browser window is already closing.");
  }
  auto result = Module::Get().Initialize();
  if (!result.has_value()) {
    return result;
  }
  auto *theme = ui::NativeTheme::GetInstanceForNativeUi();
  const KrTheme initial{sizeof(KrTheme),
                        theme->preferred_color_scheme() ==
                            ui::NativeTheme::PreferredColorScheme::kDark,
                        browser_->GetProfile()->IsOffTheRecord(),
                        theme->forced_colors() !=
                            ui::ColorProviderKey::ForcedColors::kNone,
                        view_->GetColorProvider()->GetColor(ui::kColorAccent)};
  // A hosted shell shares its frame's HWND and starts hidden.
  result = session_.Initialize(views::HWNDForView(&*view_), MakeBrowser(),
                               initial, hosted_);
  if (!result.has_value()) {
    return result;
  }
  // The layout precedes the tabs, so the first render already uses it.
  multi_page_capable_ = base::FeatureList::IsEnabled(kKryptonMultiPage) &&
                        MultiPageController::HasCompleteModule(session_.api());
  extensions_->Attach();
  if (auto *access = ExclusiveAccessManager::From(&*browser_)) {
    fullscreen_changed_ =
        access->fullscreen_controller()->RegisterOnFullscreenStateChanged(
            base::BindRepeating(&BrowserAdapter::PublishWindowState,
                                weak_factory_.GetWeakPtr()));
  }
  // Window-level settings and the Space catalog belong to the frame's profile.
  layout_prefs_.Init(&WindowPrefs());
  const auto publish_layout = base::BindRepeating(
      &BrowserAdapter::PublishLayout, base::Unretained(this));
  layout_prefs_.Add(kLayoutFlagsPref, publish_layout);
  layout_prefs_.Add(kSidebarWidthPref, publish_layout);
  for (const char *name :
       {kColorSchemePref, kMenuMaterialPref, kAppearanceFlagsPref,
        kIconPackPref, kGlassModePref, kAgentChatNewTabOverridePref}) {
    layout_prefs_.Add(name, publish_layout);
  }
  if (auto *registry = krypton::privacy::GetRegistry()) {
    agent_privacy_changed_ = registry->Observe(base::BindRepeating(
        [](base::WeakPtr<BrowserAdapter> owner, krypton::privacy::Id id) {
          if (!owner || owner->closed_ ||
              id != krypton::privacy::Id::kAiAgent) {
            return;
          }
          if (!krypton_agent::IsProductionAgentEnabled()) {
            owner->WindowPrefs().ClearPref(kAgentChatNewTabOverridePref);
          }
          owner->PublishLayout();
          if (owner->agent_panel_) {
            owner->agent_panel_->PrivacyChanged();
          }
        },
        weak_factory_.GetWeakPtr()));
  }
  agent_panel_->SetDark(initial.dark);
  PublishLayout();
  workspaces_ = ReadWorkspaces(WindowPrefs());
  if (hosted_ && frame_) {
    active_space_ = frame_->active_space_;
  } else {
    active_space_ = LastWorkspace(WindowPrefs(), workspaces_);
    // Startup never opens another profile: a remembered Space of another
    // profile presents again only when the user switches to it.
    if (!HostsSpace(active_space_)) {
      active_space_ = kDefaultWorkspace;
    }
  }
  for (const char *name : {kWorkspacesPref, kWorkspaceProfilesPref}) {
    layout_prefs_.Add(name,
                      base::BindRepeating(&BrowserAdapter::OnWorkspacesChanged,
                                          base::Unretained(this)));
  }
  PublishWorkspaces();
  if (CanFolders()) {
    folders_ = ReadFolders(WindowPrefs());
    layout_prefs_.Add(kFoldersPref,
                      base::BindRepeating(&BrowserAdapter::OnFoldersChanged,
                                          base::Unretained(this)));
    PublishFolders();
  }
  layout_prefs_.Add(kWorkspaceThemesPref,
                    base::BindRepeating(&BrowserAdapter::PublishWorkspaceThemes,
                                        base::Unretained(this)));
  PublishWorkspaceThemes();
  PublishTheme();
  auto *model = browser_->GetTabStripModel();
  image_scale_ = display::Screen::Get()
                     ->GetDisplayNearestWindow(view_->GetNativeWindow())
                     .device_scale_factor();
  model->AddObserver(this);
  for (int index = 0; index < model->count(); ++index) {
    Insert(*model->GetTabAtIndex(index), base::checked_cast<size_t>(index));
  }
  // Essentials belong to each tab's own profile.
  profile_prefs_.Init(browser_->GetProfile()->GetPrefs());
  profile_prefs_.Add(kEssentialsPref,
                     base::BindRepeating(&BrowserAdapter::OnEssentialsChanged,
                                         weak_factory_.GetWeakPtr()));
  essential_restore_ =
      SessionRestore::RegisterOnSessionRestoredCallback(base::BindRepeating(
          [](base::WeakPtr<BrowserAdapter> owner, Profile *profile, int) {
            if (owner && owner->browser_->GetProfile() == profile) {
              std::ignore = owner->session_.Defer(
                  base::BindOnce(&BrowserAdapter::OnEssentialsChanged, owner));
            }
          },
          weak_factory_.GetWeakPtr()));
  if (auto *exit_type =
          ExitTypeService::GetInstanceForProfile(browser_->GetProfile())) {
    // Crash recovery starts later, after the user accepts the crash prompt.
    // Resume catalog reconciliation after either restore or dismissal, when
    // Chromium also resumes saving the current session.
    exit_type->AddCrashAckCallback(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner) {
          if (owner) {
            std::ignore = owner->session_.Defer(
                base::BindOnce(&BrowserAdapter::OnEssentialsChanged, owner));
          }
        },
        weak_factory_.GetWeakPtr()));
  }
  // Attach runs while Browser is still constructing its window, before startup
  // tabs are inserted. Recreating essentials navigates, which needs both.
  std::ignore = session_.Defer(base::BindOnce(
      &BrowserAdapter::OnEssentialsChanged, weak_factory_.GetWeakPtr()));
  PublishSelection();
  PublishNavigation();
  ScheduleActions();
  auto state = session_.UpdateWindowState(WindowFlags());
  if (!state.has_value()) {
    return base::unexpected(std::move(state.error()));
  }
  view_->InvalidateLayout();
  // A hosted shell is hidden until presented; the frame owns first run.
  if (!hosted_) {
    std::ignore = session_.Defer(
        base::BindOnce(&BrowserServices::MaybeShowFirstRun,
                       browser_services_->weak_factory_.GetWeakPtr()));
  }
  sidebar_->Initialize();
  site_customization_ = std::make_unique<SiteCustomization>(*this);
  site_customization_->Initialize();
  if (CanFolders()) {
    live_folders_ = std::make_unique<LiveFolders>(*this);
  }
  AttachSpaceHost();
  return {};
}

void BrowserAdapter::Close() {
  if (closed_) {
    return;
  }
  if (const auto *draft = workspace_themes_.preview()) {
    // Closing a browser dismisses its editor too. Save before invalidating
    // deferred callbacks and destroying the UI that owns the draft surface.
    const auto value = *draft;
    ApplyWorkspaceTheme(value, true);
  }
  closed_ = true;
  shortcuts_suspended_ = false;
  shortcut_catalog_requested_ = false;
  agent_panel_.reset();
  CloseSpaceHost();
  live_folders_.reset();
  page_corner_overlays_.reset();
  TabStripModelObserver::StopObservingAll(this);
  layout_prefs_.RemoveAll();
  essential_restore_ = {};
  fullscreen_changed_ = {};
  agent_privacy_changed_ = {};
  weak_factory_.InvalidateWeakPtrs();
  if (extensions_) {
    extensions_->Close();
  }
  toolbar_->SetExtensionsContainer(nullptr);
  toolbar_->CloseMenu();
  menus_->CloseAll();
  site_customization_.reset();
  browser_services_.reset();
  site_permissions_.reset();
  screenshot_.reset();
  sidebar_.reset();
  location_->Close();
  switcher_previews_.clear();
  switcher_request_ = 0;
  tab_preview_.reset();
  glass_.reset();
  multi_page_.reset();
  tabs_.clear();
  for (const auto &[id, action] : actions_) {
    images_.Release(action->icon);
  }
  actions_.clear();
  // Cancels non-nestable browser commands and transient presentations before
  // destroying XAML. The process-lifetime module remains loaded.
  session_.Close();
  images_.Clear();
}

void BrowserAdapter::DropPreviews() {
  switcher_previews_.clear();
  switcher_request_ = 0;
  tab_preview_.reset();
}

KrTabId BrowserAdapter::Identity(const tabs::TabInterface &tab) {
  return base::checked_cast<KrTabId>(tab.GetHandle().raw_value());
}

tabs::TabInterface *BrowserAdapter::FindTab(KrTabId id) const {
  const auto found = tabs_.find(id);
  if (found == tabs_.end()) {
    return nullptr;
  }
  auto *tab = found->second->tab().get();
  return tab && tab->GetBrowserWindowInterface() == &*browser_ ? tab : nullptr;
}

void BrowserAdapter::Insert(tabs::TabInterface &tab, size_t index) {
  // Custom activation can publish this identity before the insertion observer.
  if (tabs_.contains(Identity(tab))) {
    return;
  }
  CHECK(tabs_.emplace(Identity(tab), std::make_unique<Tab>(*this, tab)).second);
  EnsureWorkspace(tab);
  if (new_folder_) {
    SetFolderForTab(tab.GetContents(), new_folder_);
  }
  if (const auto *folder = folders_.Find(FolderForTab(tab.GetContents()));
      !folder || folder->space != WorkspaceForTab(tab.GetContents())) {
    SetFolderForTab(tab.GetContents(), 0);
  }
  if (new_essential_) {
    AssignEssential(tab, new_essential_);
  }
  const KrTab initial{sizeof(KrTab), Identity(tab),
                      WorkspaceForTab(tab.GetContents())};
  const auto *api = session_.api();
  api->tab_inserted(api->self, index, &initial);
  PublishTab(tab);
  if (base::FeatureList::IsEnabled(kKryptonMultiPage) &&
      SplitSessionForTab(tab.GetContents()) && MultiPageEnabled()) {
    if (!multi_page_) {
      multi_page_ = std::make_unique<MultiPageController>(*this);
    }
    multi_page_->ScheduleRestore();
  }
}

void BrowserAdapter::PublishTab(tabs::TabInterface &tab) {
  const auto *api = session_.api();
  const auto found = tabs_.find(Identity(tab));
  if (!api || found == tabs_.end()) {
    return;
  }
  auto &previous = *found->second;
  auto *contents = tab.GetContents();
  const auto space = WorkspaceForTab(contents);
  const auto url = contents->GetVisibleURL().spec();
  const auto title = url == krypton_agent::kAgentNewTabUrl
                         ? "Agent chat"
                         : base::UTF16ToUTF8(tab.GetTitle());
  uint32_t flags = tab.IsPinned() ? KR_TAB_PINNED : 0;
  previous.essential = EssentialForTab(contents);
  if (previous.essential) {
    flags |= KR_TAB_FAVORITE;
  }
  if (contents->IsLoading()) {
    flags |= KR_TAB_LOADING;
  }
  if (contents->IsCurrentlyAudible()) {
    flags |= KR_TAB_AUDIBLE;
  }
  if (contents->IsAudioMuted()) {
    flags |= KR_TAB_MUTED;
  }
  if (auto *helper = TabUIHelper::From(&tab);
      (helper && helper->IsDiscarded()) ||
      contents->GetController().NeedsReload()) {
    flags |= KR_TAB_SLEEPING;
  }
  if (contents->IsCrashed()) {
    flags |= KR_TAB_CRASHED;
  }
  if (tab.IsBlocked()) {
    flags |= KR_TAB_ATTENTION;
  }
  if (krypton_agent::IsAgentWorkingOnTab(tab)) {
    flags |= KR_TAB_AGENT_WORKING;
  }
  ui::ImageModel image;
  if (auto *driver = favicon::ContentFaviconDriver::FromWebContents(contents)) {
    image = ui::ImageModel::FromImage(driver->GetFavicon());
  }
  if (site_customization_) {
    image = site_customization_->Icon(tab, image);
  }
  previous.glyph = TabIconForTab(contents);
  bool changed_image = previous.image_dirty || image != previous.image;
  KrImageId next_icon = previous.icon;
  if (changed_image) {
    const auto acquired = AcquireImage(image);
    if (acquired.has_value()) {
      next_icon = *acquired;
    } else {
      LOG(ERROR) << acquired.error();
      changed_image = false;
    }
  }
  if (!previous.published || title != previous.title || url != previous.url ||
      flags != previous.flags || space != previous.space || changed_image ||
      previous.folder != FolderForTab(contents)) {
    const KrTab value{sizeof(KrTab), Identity(tab),         space,
                      flags,         Borrow(title),         Borrow(url),
                      next_icon,     FolderForTab(contents)};
    api->tab_updated(api->self, &value);
    if (changed_image) {
      images_.Release(previous.icon);
      previous.icon = next_icon;
      previous.image = std::move(image);
      previous.image_dirty = false;
    }
    previous.title = title;
    previous.url = url;
    previous.flags = flags;
    previous.space = space;
    previous.folder = FolderForTab(contents);
    previous.published = true;
  }
}

void BrowserAdapter::OnTabStripModelChanged(
    TabStripModel *, const TabStripModelChange &change,
    const TabStripSelectionChange &selection) {
  if (selection.active_tab_changed() && dictation_) {
    dictation_->Stop();
  }
  const auto *api = session_.api();
  if (!api) {
    return;
  }
  switch (change.type()) {
  case TabStripModelChange::kInserted:
    for (const auto &value : change.GetInsert()->contents) {
      Insert(*value.tab, base::checked_cast<size_t>(value.index));
    }
    break;
  case TabStripModelChange::kRemoved:
    for (const auto &value : change.GetRemove()->contents) {
      const auto id = Identity(*value.tab);
      if (multi_page_) {
        multi_page_->Removed(id);
      }
      // A detached tab can already have lost its old window association, so
      // the controller's identity lookup may no longer reach it. Clear its
      // saved membership through the removal record before another window
      // can adopt it and mistake a window-local group id for its own group.
      if (multi_page_ && SplitSessionForTab(value.tab->GetContents())) {
        SetSplitSessionForTab(value.tab->GetContents(), std::nullopt);
      }
      if (tab_preview_) {
        tab_preview_->Removed(id);
      }
      api->tab_removed(api->self, id);
      anchors_.Untrack({AnchorKind::kTab, id});
      tabs_.erase(id);
    }
    break;
  case TabStripModelChange::kMoved: {
    const auto &value = *change.GetMove();
    api->tab_moved(api->self, Identity(*value.tab),
                   base::checked_cast<size_t>(value.to_index));
    break;
  }
  case TabStripModelChange::kReplaced: {
    const auto &value = *change.GetReplace();
    tabs_.at(Identity(*value.tab))->Replaced(value.new_contents);
    if (multi_page_) {
      multi_page_->Replaced(Identity(*value.tab));
    }
    EnsureWorkspace(*value.tab);
    PublishTab(*value.tab);
    break;
  }
  case TabStripModelChange::kSelectionOnly:
    break;
  }
  if (selection.active_tab_changed() ||
      change.type() == TabStripModelChange::kReplaced) {
    PublishSelection();
    PublishNavigation();
    if (agent_panel_) {
      agent_panel_->UpdateActiveTab();
    }
    ScheduleActions();
    SchedulePageBackgrounds();
  }
}

void BrowserAdapter::SchedulePageBackgrounds() {
  if (page_backgrounds_scheduled_) {
    return;
  }
  page_backgrounds_scheduled_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&BrowserAdapter::UpdatePageBackgrounds,
                                weak_factory_.GetWeakPtr()));
}

void BrowserAdapter::UpdatePageBackgrounds() {
  page_backgrounds_scheduled_ = false;
  auto *pages = view_->multi_contents_view();
  if (!pages) {
    return;
  }
  for (ContentsContainerView *container : pages->contents_container_views()) {
    ContentsWebView *web_view =
        container ? container->contents_view() : nullptr;
    if (!web_view) {
      continue;
    }
    // A new tab has no committed entry yet; its pending chrome://newtab is
    // visible. Leaving the page waits for the next page to commit.
    bool new_tab_page = false;
    if (auto *contents = web_view->web_contents()) {
      const GURL &committed = contents->GetLastCommittedURL();
      const GURL &url =
          committed.is_empty() ? contents->GetVisibleURL() : committed;
      new_tab_page =
          search::IsNTPURL(url) || url == GURL(chrome::kChromeUINewTabURL);
    }
    web_view->SetBackgroundVisible(!new_tab_page);
  }
}

void BrowserAdapter::OnTabChangedAt(tabs::TabInterface *tab, TabChangeType) {
  PublishTab(*tab);
  if (tab->IsActivated()) {
    PublishNavigation();
  }
}
void BrowserAdapter::OnTabPinnedStateChanged(tabs::TabInterface *tab, int) {
  RecordPinnedHome(*tab);
  if (!tab->IsPinned() && FolderForTab(tab->GetContents())) {
    AssignFolder(*tab, 0);
  }
  PublishTab(*tab);
}

void BrowserAdapter::PublishSelection() {
  const auto *api = session_.api();
  if (!api) {
    return;
  }
  auto *tab = browser_->GetActiveTabInterface();
  if (multi_page_ && tab) {
    (void)multi_page_->PrepareSelection(tab->GetContents());
  }
  const auto next = tab ? Identity(*tab) : 0;
  if (tab) {
    const auto space = WorkspaceForTab(tab->GetContents());
    // Only the presented profile's selection decides the window's Space.
    if (!EssentialForTab(tab->GetContents()) && space != active_space_ &&
        presented_) {
      std::ignore = FollowSpace(space);
    }
  }
  if (active_tab_ != next) {
    active_tab_ = next;
    api->set_active_tab(api->self, next);
    if (tab) {
      location_->Update(tab->GetContents());
    }
  }
  if (tab) {
    PublishProgress(*tab);
  }
}

void BrowserAdapter::PublishNavigation() {
  const auto *api = session_.api();
  if (!api) {
    return;
  }
  auto *tab = browser_->GetActiveTabInterface();
  auto *contents = tab ? tab->GetContents() : nullptr;
  UpdateDocumentPageTheme(contents);
  auto &model = *LocationBarModel::From(&*browser_);
  auto display = base::UTF16ToUTF8(model.GetURLForDisplay());
  auto full = base::UTF16ToUTF8(model.GetFormattedFullURL());
  const auto security = Security(model.GetSecurityLevel());
  const bool back = contents && contents->GetController().CanGoBack();
  const bool forward = contents && contents->GetController().CanGoForward();
  const auto loading_tab = tabs_.find(active_tab_);
  const bool loading = contents && loading_tab != tabs_.end() &&
                       loading_tab->second->primary_loading;
  if (navigation_published_ && display == display_url_ && full == full_url_ &&
      security == security_ && back == back_ && forward == forward_ &&
      loading == loading_) {
    return;
  }
  display_url_ = std::move(display);
  full_url_ = std::move(full);
  security_ = security;
  back_ = back;
  forward_ = forward;
  loading_ = loading;
  navigation_published_ = true;
  const KrNavState state{sizeof(KrNavState),
                         back,
                         forward,
                         loading,
                         security,
                         Borrow(display_url_),
                         Borrow(full_url_)};
  api->set_nav_state(api->self, &state);
}

void BrowserAdapter::PublishProgress(tabs::TabInterface &tab) {
  const auto *api = session_.api();
  const auto found = tabs_.find(Identity(tab));
  if (!api || !tab.IsActivated() || found == tabs_.end()) {
    return;
  }
  uint32_t flags = tab.GetContents()->GetVisibleURL().SchemeIsHTTPOrHTTPS()
                       ? KR_LOAD_PROGRESS_ENABLED
                       : 0;
  const auto &state = *found->second;
  if (state.primary_loading) {
    flags |= KR_LOAD_PROGRESS_LOADING;
  }
  const KrLoadProgress progress{sizeof(KrLoadProgress), Identity(tab),
                                state.navigation,
                                static_cast<float>(state.progress), flags};
  api->set_load_progress(api->self, &progress);
}

base::expected<KrImageId, std::string>
BrowserAdapter::AcquireImage(const ui::ImageModel &image) {
  return images_.Acquire(image, view_->GetColorProvider(),
                         display::Screen::Get()
                             ->GetDisplayNearestWindow(view_->GetNativeWindow())
                             .device_scale_factor());
}

void BrowserAdapter::ScheduleActions() {
  if (!ready() || actions_scheduled_ || closed_) {
    return;
  }
  actions_scheduled_ = true;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&BrowserAdapter::PublishActions,
                                weak_factory_.GetWeakPtr()));
}

void BrowserAdapter::ShowTranslationMenu() {
  if (!ready() || !presented_) {
    return;
  }
  auto *tab = browser_->GetActiveTabInterface();
  if (!tab || !CanShowTranslationPanel(tab->GetContents())) {
    return;
  }
  std::ignore = session_.Defer(base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> owner,
         base::WeakPtr<tabs::TabInterface> tab,
         content::WeakDocumentPtr document) {
        if (!owner || !owner->ready() || !owner->presented_ || !tab ||
            owner->browser_->GetActiveTabInterface() != tab.get() ||
            !CanShowTranslationPanel(tab->GetContents()) ||
            document.AsRenderFrameHostIfValid() !=
                tab->GetContents()->GetPrimaryMainFrame()) {
          return;
        }
        auto bounds =
            owner->anchors_.GetBounds({AnchorKind::kAction, kTranslateAction});
        if (!bounds) {
          bounds = owner->anchors_.GetBounds(
              {AnchorKind::kAction, kSiteToolsAction});
        }
        const auto *api = owner->session_.api();
        if (bounds && api) {
          api->request_context_menu(
              api->self, KR_MENU_ACTION, kTranslateAction,
              {bounds->x(), bounds->y(), bounds->width(), bounds->height()});
        }
      },
      weak_factory_.GetWeakPtr(), tab->GetWeakPtr(),
      tab->GetContents()->GetPrimaryMainFrame()->GetWeakDocumentPtr()));
}

void BrowserAdapter::ShowVoiceMenu() {
  if (!ready() || !presented_) {
    return;
  }
  auto *tab = browser_->GetActiveTabInterface();
  if (!tab) {
    return;
  }
  std::ignore = session_.Defer(base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> owner,
         base::WeakPtr<tabs::TabInterface> tab) {
        if (!owner || !owner->ready() || !owner->presented_ || !tab ||
            owner->browser_->GetActiveTabInterface() != tab.get()) {
          return;
        }
        auto bounds = owner->anchors_.GetBounds(
            {AnchorKind::kElement, KR_ELEMENT_ADDRESS});
        if (!bounds) {
          bounds = owner->anchors_.GetBounds(
              {AnchorKind::kAction, kSiteToolsAction});
        }
        const auto *api = owner->session_.api();
        if (bounds && api) {
          api->request_context_menu(
              api->self, KR_MENU_ACTION, kDictationAction,
              {bounds->x(), bounds->y(), bounds->width(), bounds->height()});
        }
      },
      weak_factory_.GetWeakPtr(), tab->GetWeakPtr()));
}

void BrowserAdapter::PublishActions() {
  actions_scheduled_ = false;
  const auto *api = session_.api();
  if (!api) {
    return;
  }
  action_views_.clear();
  action_ids_.assign(toolbar_->PinnedActionIds().begin(),
                     toolbar_->PinnedActionIds().end());
  for (const auto id : toolbar_->PoppedOutActionIds()) {
    if (!std::ranges::contains(action_ids_, id)) {
      action_ids_.push_back(id);
    }
  }
  for (const auto id : action_ids_) {
    // Library owns downloads and its start/progress feedback.
    if (id == kActionShowDownloads) {
      continue;
    }
    auto *item = toolbar_->GetActionItemFor(id);
    if (!item) {
      continue;
    }
    const auto key = NativeToolbar::ActionIdentity(id);
    auto &state = actions_[key];
    if (!state || state->source.get() != item) {
      if (state) {
        images_.Release(state->icon);
      }
      state = std::make_unique<Action>();
      state->source = item->GetAsWeakPtr();
      state->subscription = item->AddActionChangedCallback(base::BindRepeating(
          &BrowserAdapter::ScheduleActions, weak_factory_.GetWeakPtr()));
    }
    state->label = base::UTF16ToUTF8(item->GetText());
    state->tooltip = base::UTF16ToUTF8(item->GetTooltipText());
    if (state->image_dirty || state->image != item->GetImage()) {
      auto image = AcquireImage(item->GetImage());
      if (image.has_value()) {
        const auto previous = state->icon;
        state->icon = *image;
        state->image = item->GetImage();
        state->image_dirty = false;
        images_.Release(previous);
      }
    }
    uint32_t flags = item->GetVisible() ? KR_ACTION_VISIBLE : 0;
    if (item->GetEnabled()) {
      flags |= KR_ACTION_ENABLED;
    }
    if (item->GetChecked()) {
      flags |= KR_ACTION_ACTIVE;
    }
    action_views_.push_back({sizeof(KrAction),
                             key,
                             KR_ACTION_PINNED,
                             flags,
                             Borrow(state->label),
                             Borrow(state->tooltip),
                             state->icon,
                             {},
                             {},
                             0,
                             -1});
  }
  if (extensions_) {
    extensions_->AppendActions(action_views_);
  }
  if (copy_link_label_.empty()) {
    copy_link_label_ =
        base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_APP_MENU_COPY_LINK));
  }
  static constexpr char copy_glyph[] = "\uE8C8";
  action_views_.push_back(
      {sizeof(KrAction),
       kCopyLinkAction,
       KR_ACTION_COPY_LINK,
       static_cast<uint32_t>(
           KR_ACTION_VISIBLE |
           (chrome::CanCopyUrl(&*browser_) ? KR_ACTION_ENABLED : 0)),
       Borrow(copy_link_label_),
       Borrow(copy_link_label_),
       0,
       {copy_glyph, sizeof(copy_glyph) - 1},
       {},
       0,
       -1});
  if (!dictation_) {
    dictation_ = std::make_unique<DictationController>(
        *browser_, *view_, session_,
        base::BindRepeating(
            [](base::WeakPtr<BrowserAdapter> owner) {
              return owner && owner->ready() && owner->presented_ &&
                     owner->location_->IsFocusWithin();
            },
            weak_factory_.GetWeakPtr()),
        base::BindRepeating(&BrowserAdapter::ScheduleActions,
                            weak_factory_.GetWeakPtr()),
        base::BindRepeating(&BrowserAdapter::ShowVoiceMenu,
                            weak_factory_.GetWeakPtr()));
  }
  if (auto action = dictation_->Action()) {
    action_views_.push_back(*action);
  }
  if (!translation_actions_) {
    translation_actions_ = std::make_unique<TranslationActions>(
        *browser_->GetProfile(),
        base::BindRepeating(&BrowserAdapter::ScheduleActions,
                            weak_factory_.GetWeakPtr()));
  }
  if (auto *tab = browser_->GetActiveTabInterface()) {
    auto *contents = tab->GetContents();
    if (auto *zoom = zoom::ZoomController::FromWebContents(contents);
        zoom && !zoom->IsAtDefaultZoom() &&
        chrome::IsCommandEnabled(&*browser_, IDC_ZOOM_NORMAL)) {
      zoom_label_ = base::NumberToString(zoom->GetZoomPercent()) + "%";
      zoom_tooltip_ = "Reset page zoom (" + zoom_label_ + ")";
      KrAction reset{};
      reset.struct_size = sizeof(reset);
      reset.id = kZoomResetAction;
      reset.kind = KR_ACTION_ZOOM_RESET;
      reset.flags = KR_ACTION_VISIBLE | KR_ACTION_ENABLED;
      reset.label = Borrow(zoom_label_);
      reset.tooltip = Borrow(zoom_tooltip_);
      reset.progress = -1;
      action_views_.push_back(reset);
    }
    if (auto action = TranslationAction(contents, translation_tooltip_,
                                        translation_locale_)) {
      action_views_.push_back(*action);
    }
  }
  if (site_tools_label_.empty()) {
    site_tools_label_ =
        base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_MORE_TOOLS_MENU));
  }
  static constexpr char tools_glyph[] = "\uE713";
  const auto *tools_tab = browser_->GetActiveTabInterface();
  const bool boosted =
      tools_tab && site_customization_ &&
      site_customization_
          ->BoostForSite(tools_tab->GetContents()->GetLastCommittedURL())
          .active;
  action_views_.push_back({sizeof(KrAction),
                           kSiteToolsAction,
                           KR_ACTION_SITE_TOOLS,
                           KR_ACTION_VISIBLE | KR_ACTION_ENABLED |
                               (boosted ? KR_ACTION_ACTIVE : 0u),
                           Borrow(site_tools_label_),
                           Borrow(site_tools_label_),
                           0,
                           {tools_glyph, sizeof(tools_glyph) - 1},
                           {},
                           0,
                           -1});
  api->set_actions(api->self, action_views_.data(), action_views_.size(),
                   sizeof(KrAction));
  if (menus_) {
    menus_->RefreshTranslationMenus();
  }
  for (auto iterator = actions_.begin(); iterator != actions_.end();) {
    const auto &entry = *iterator;
    const bool removed =
        std::ranges::none_of(action_views_, [&](const KrAction &action) {
          return action.id == entry.first;
        });
    if (removed) {
      images_.Release(entry.second->icon);
    }
    if (removed) {
      iterator = actions_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

void BrowserAdapter::PublishLayout() {
  const auto *api = session_.api();
  if (!api || closed_) {
    return;
  }
  const auto layout = ReadLayoutPrefs(WindowPrefs());
  api->set_layout_prefs(api->self, &layout);
  if (KR_HAS_SLOT(api, KrShellUI, set_appearance_prefs) &&
      api->set_appearance_prefs) {
    const bool agent_enabled = browser_->GetProfile()->IsRegularProfile() &&
                               krypton_agent::IsProductionAgentEnabled();
    const auto appearance = ReadAppearancePrefs(WindowPrefs(), agent_enabled);
    api->set_appearance_prefs(api->self, &appearance);
  }
}

base::ScopedClosureRunner
BrowserAdapter::RequestPageMenu(ui::MenuModel &model,
                                const gfx::Point &screen_point) {
  if (closed_ || !session_.api()) {
    return {};
  }
  auto point = screen_point;
  views::View::ConvertPointFromScreen(&*view_, &point);
  return menus_->RequestPageMenu(model, {static_cast<float>(point.x()),
                                         static_cast<float>(point.y()), 1, 1});
}

void BrowserAdapter::RefreshPageMenu(const ui::MenuModel &model) {
  if (!closed_ && session_.api()) {
    menus_->RefreshPageMenu(model, view_->GetColorProvider());
  }
}

base::expected<void, std::string>
BrowserAdapter::ShowPageInfo(tabs::TabInterface *tab) {
  if (!tab || tab->GetBrowserWindowInterface() != &*browser_ ||
      browser_->GetActiveTabInterface() != tab) {
    return base::unexpected("There is no page to describe.");
  }
  const auto *api = session_.api();
  if (!api) {
    return base::unexpected("The native shell is closed.");
  }
  auto bounds = location_->Bounds();
  if (bounds.IsEmpty()) {
    bounds = views::View::ConvertRectFromScreen(
        &*view_, tab->GetContents()->GetContainerBounds());
    bounds.set_height(0);
  }
  api->request_context_menu(api->self, KR_MENU_SECURITY, 0,
                            {static_cast<float>(bounds.x()),
                             static_cast<float>(bounds.y()),
                             static_cast<float>(bounds.width()),
                             static_cast<float>(bounds.height())});
  return {};
}

void BrowserAdapter::ExecuteCommand(int command) {
  if (!closed_ && chrome::IsCommandEnabled(&*browser_, command)) {
    chrome::ExecuteCommand(&*browser_, command);
  }
}

base::OnceClosure BrowserAdapter::CaptureCommand(int command, bool tab_scoped) {
  if (!chrome::IsCommandEnabled(&*browser_, command)) {
    return {};
  }
  auto *tab = tab_scoped ? browser_->GetActiveTabInterface() : nullptr;
  auto target = tab ? tab->GetWeakPtr() : base::WeakPtr<tabs::TabInterface>();
  return base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> owner,
         base::WeakPtr<tabs::TabInterface> target, bool tab_scoped,
         int command) {
        if (!owner ||
            (tab_scoped &&
             (!target ||
              target.get() != owner->browser_->GetActiveTabInterface()))) {
          return;
        }
        owner->ExecuteCommand(command);
      },
      weak_factory_.GetWeakPtr(), std::move(target), tab_scoped, command);
}
void BrowserAdapter::Activate(KrTabId id) {
  if (auto *tab = FindTab(id);
      tab && tab != browser_->GetActiveTabInterface()) {
    browser_->GetTabStripModel()->ActivateTab(tab);
  }
}
void BrowserAdapter::CloseTab(KrTabId id) {
  if (auto *tab = FindTab(id)) {
    browser_->GetTabStripModel()->CloseWebContents(
        tab->GetContents(), TabCloseTypes::CLOSE_USER_GESTURE);
  }
}
void BrowserAdapter::SleepTab(KrTabId id) {
  auto *tab = FindTab(id);
  auto *tab_list = TabListInterface::From(&*browser_);
  if (!tab || !tab_list || IsTransientGlance(tab->GetContents())) {
    return;
  }

  auto *model = browser_->GetTabStripModel();
  if (tab->IsActivated()) {
    const auto handle = tab->GetHandle();
    const auto next = model->GetSelectionAfterRemovingTab(tab);
    if (next && IsWorkspaceCloseCandidate(*model->GetTabAtIndex(*next))) {
      model->ActivateTabAt(*next);
    } else {
      const base::AutoReset<KrSpaceId> workspace(&new_tab_space_,
                                                 active_space_);
      chrome::AddTabAt(&*browser_, GURL(), -1, /*foreground=*/true);
    }
    tab = handle.Get();
    if (!tab || tab->IsActivated() || model->GetIndexOfTab(tab) < 0) {
      return;
    }
  }

  tab_list->DiscardTab(tab->GetHandle());
}

void BrowserAdapter::MoveTab(KrTabId id, KrSpaceId space, size_t index) {
  // Tabs never change profile: only Spaces this browser hosts are targets.
  if (!HostsSpace(space)) {
    return;
  }
  if (multi_page_ && !multi_page_->BeforeTabMove(id, space, index)) {
    return;
  }
  auto *tab = FindTab(id);
  auto *model = browser_->GetTabStripModel();
  if (tab && index < base::checked_cast<size_t>(model->count())) {
    const bool follow = tab->IsActivated() && space != active_space_;
    AssignWorkspace(*tab, space);
    PublishTab(*tab);
    if (EssentialForTab(tab->GetContents())) {
      AlignEssentialPin(*tab, base::checked_cast<int>(index));
    }
    model->MoveWebContentsAt(model->GetIndexOfTab(tab),
                             base::checked_cast<int>(index), false);
    if (EssentialForTab(tab->GetContents())) {
      std::vector<uint64_t> order;
      for (auto *item : *model) {
        if (const auto essential = EssentialForTab(item->GetContents())) {
          order.push_back(essential);
        }
      }
      std::ignore =
          ReorderEssentials(*browser_->GetProfile()->GetPrefs(), order);
    }
    PublishSelection();
    if (follow) {
      SwitchWorkspace(space);
    }
  }
}
// Chromium keeps pinned tabs before the others, while the Essentials grid
// shows Essentials in strip order whatever their pinned state. Match a moved
// Essential's state to the tab it lands beside, so the strip can hold the
// order the grid asked for.
void BrowserAdapter::AlignEssentialPin(tabs::TabInterface &tab, int to_index) {
  auto *model = browser_->GetTabStripModel();
  const int from = model->GetIndexOfTab(&tab);
  // `to_index` counts the strip without `tab`.
  const auto other = [&](int index) -> tabs::TabInterface * {
    if (index < 0 || index >= model->count() - 1) {
      return nullptr;
    }
    return model->GetTabAtIndex(index < from ? index : index + 1);
  };
  const auto *next = other(to_index);
  const auto *previous = other(to_index - 1);
  bool pinned = tab.IsPinned();
  if (next && next->IsPinned()) {
    pinned = true;
  } else if (previous && !previous->IsPinned()) {
    pinned = false;
  }
  if (pinned != tab.IsPinned()) {
    model->SetTabPinned(from, pinned);
  }
}

void BrowserAdapter::SetTabFlags(KrTabId id, uint32_t mask, uint32_t values) {
  auto *tab = FindTab(id);
  if (!tab) {
    return;
  }
  if (mask & KR_TAB_FAVORITE) {
    SetEssential(*tab, (values & KR_TAB_FAVORITE) != 0);
  }
  if (mask & KR_TAB_MUTED) {
    tab->GetContents()->SetAudioMuted((values & KR_TAB_MUTED) != 0);
  }
  if (mask & KR_TAB_PINNED) {
    if (multi_page_ &&
        multi_page_->PinGroup(id, (values & KR_TAB_PINNED) != 0)) {
      return;
    }
    auto *model = browser_->GetTabStripModel();
    model->SetTabPinned(model->GetIndexOfTab(tab),
                        (values & KR_TAB_PINNED) != 0);
  }
}

void BrowserAdapter::TakeKeyboardFocus() {
  if (closed_) {
    return;
  }
  // KryptonUI islands are child windows that hold Win32 keyboard focus while
  // in use. Chromium only moves focus inside this window, so a focused page
  // would still leave keys with the island.
  auto *widget = view_->GetWidget();
  if (!widget || !widget->IsActive() || !widget->IsVisible()) {
    return;
  }
  const HWND window = views::HWNDForWidget(widget);
  const HWND focus = ::GetFocus();
  if (window && focus && ::IsChild(window, focus)) {
    ::SetFocus(window);
  }
}

bool BrowserAdapter::ExecuteNativeShortcut(int command) {
  auto *tab = browser_->GetActiveTabInterface();
  switch (command) {
  case kShortcutSleepTab:
    if (tab) {
      SleepTab(Identity(*tab));
    }
    return true;
  case kShortcutNewNote:
  case kShortcutNewEasel:
    if (CanCreateDocument()) {
      NewDocument(command == kShortcutNewNote ? DocumentKind::kNote
                                              : DocumentKind::kEasel);
    }
    return true;
  case kShortcutSplitGrid:
  case kShortcutSplitColumns:
  case kShortcutSplitRows:
  case kShortcutUnsplit:
    if (!MultiPageEnabled()) {
      return false;
    }
    if (tab) {
      auto *dialogs = web_modal::WebContentsModalDialogManager::FromWebContents(
          tab->GetContents());
      if (dialogs && dialogs->IsDialogActive()) {
        return false;
      }
    }
    if (!multi_page_) {
      multi_page_ = std::make_unique<MultiPageController>(*this);
    }
    return multi_page_->ExecuteLayoutShortcut(
        command == kShortcutSplitGrid   ? KR_SPLIT_GRID
        : command == kShortcutSplitRows ? KR_SPLIT_ROWS
                                        : KR_SPLIT_COLUMNS,
        command == kShortcutUnsplit);
  default:
    return false;
  }
}

void BrowserAdapter::ShortcutsChanged() {
  if (!shortcut_catalog_requested_) {
    return;
  }
  const auto *ui = session_.api();
  const auto *local_state = g_browser_process->local_state();
  if (local_state && KR_HAS_SLOT(ui, KrShellUI, set_shortcuts)) {
    PublishShortcutCatalog(*ui, *local_state,
                           *browser_->GetProfile()->GetPrefs());
  }
}

bool BrowserAdapter::HandleUnhandledKey(const MSG &message) {
  if (shortcuts_suspended_) {
    return false;
  }
  if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
    return false;
  }
  // Use Chromium's native conversion, including repeat state, AltGr and the
  // original event timestamp, just as its Views keyboard path does.
  const auto key = ui::KeyEventFromMSG({message.hwnd,
                                        message.message,
                                        message.wParam,
                                        message.lParam,
                                        message.time,
                                        {message.pt.x, message.pt.y}});
  const int flags = key.flags();
  if (MultiPageEnabled()) {
    if (auto *contents = browser_->GetTabStripModel()->GetActiveWebContents()) {
      const auto *dialogs =
          web_modal::WebContentsModalDialogManager::FromWebContents(contents);
      if (dialogs && dialogs->IsDialogActive()) {
        return false;
      }
    }
    if (message.wParam == VK_ESCAPE) {
      const auto *method = view_->GetWidget()->GetInputMethod();
      const auto *client = method ? method->GetTextInputClient() : nullptr;
      if (client && client->HasCompositionText()) {
        return false;
      }
      if (menus_->HasOpenMenu()) {
        menus_->CloseAll();
        return true;
      }
    }
    if (multi_page_ &&
        multi_page_->HandleKey(base::checked_cast<unsigned int>(message.wParam),
                               (flags & ui::EF_CONTROL_DOWN) != 0,
                               (flags & ui::EF_ALT_DOWN) != 0,
                               (flags & ui::EF_SHIFT_DOWN) != 0)) {
      return true;
    }
  }
  const ui::Accelerator accelerator(key);
  int command;
  if (!view_->FindCommandIdForAccelerator(accelerator, &command)) {
    // A known, non-repeatable shortcut remains consumed when held. Falling
    // through could run the module's simpler fallback for the same command.
    return accelerator.IsRepeat() &&
           view_->FindCommandIdForAccelerator(
               ui::Accelerator(accelerator.key_code(),
                               flags & ~ui::EF_IS_REPEAT),
               &command);
  }
  auto *tab = browser_->GetActiveTabInterface();
  auto target = tab ? tab->GetWeakPtr() : base::WeakPtr<tabs::TabInterface>();
  return session_.Defer(base::BindOnce(
      [](base::WeakPtr<BrowserAdapter> owner,
         base::WeakPtr<tabs::TabInterface> target, bool had_tab,
         ui::Accelerator accelerator, int command) {
        if (!owner ||
            (had_tab &&
             (!target ||
              target.get() != owner->browser_->GetActiveTabInterface()))) {
          return;
        }
        owner->view_->ExecuteCapturedAccelerator(accelerator, command);
      },
      weak_factory_.GetWeakPtr(), std::move(target), tab != nullptr,
      accelerator, command));
}

uint32_t BrowserAdapter::WindowFlags() const {
  const auto *widget = view_->GetWidget();
  uint32_t flags = widget->IsActive() ? KR_WINDOW_ACTIVE : 0;
  if (widget->IsMaximized()) {
    flags |= KR_WINDOW_MAXIMIZED;
  }
  if (widget->IsFullscreen()) {
    flags |= KR_WINDOW_FULLSCREEN;
  }
  if (auto *access = ExclusiveAccessManager::From(&*browser_);
      access && access->fullscreen_controller()->IsTabFullscreen()) {
    flags |= KR_WINDOW_TAB_FULLSCREEN;
  }
  return flags;
}
void BrowserAdapter::WindowCommand(uint32_t command) {
  if (hosted_) {
    // Caption commands act on the shared frame, never only on this host.
    if (frame_) {
      frame_->WindowCommand(command);
    }
    return;
  }
  auto *widget = view_->GetWidget();
  switch (command) {
  case KR_WINDOW_MINIMIZE:
    widget->Minimize();
    break;
  case KR_WINDOW_TOGGLE_MAXIMIZE:
    if (widget->IsMaximized()) {
      widget->Restore();
    } else {
      widget->Maximize();
    }
    break;
  case KR_WINDOW_CLOSE:
    widget->Close();
    break;
  }
}
void BrowserAdapter::PublishTheme() {
  const auto *api = session_.api();
  if (!api) {
    return;
  }
  toolbar_->CloseMenu();
  menus_->CloseAll();
  auto *native = ui::NativeTheme::GetInstanceForNativeUi();
  const auto scheme = ThemeScheme();
  const KrTheme theme{sizeof(KrTheme),
                      scheme == KR_THEME_DARK ||
                          (scheme == KR_THEME_SYSTEM &&
                           native->preferred_color_scheme() ==
                               ui::NativeTheme::PreferredColorScheme::kDark),
                      browser_->GetProfile()->IsOffTheRecord(),
                      native->forced_colors() !=
                          ui::ColorProviderKey::ForcedColors::kNone,
                      view_->GetColorProvider()->GetColor(ui::kColorAccent)};
  api->set_theme(api->self, &theme);
  if (agent_panel_) {
    agent_panel_->SetDark(theme.dark);
  }
  auto *model = browser_->GetTabStripModel();
  for (int index = 0; index < model->count(); ++index) {
    UpdateDocumentPageTheme(model->GetWebContentsAt(index));
  }
  PublishWorkspaceThemes();
  for (const auto &[id, action] : actions_) {
    action->image_dirty = true;
  }
  if (extensions_) {
    extensions_->RefreshImages();
  }
  ScheduleActions();
}

void BrowserAdapter::UpdateFrameMaterial() {
  auto *widget = view_->GetWidget();
  // The frame owns the HWND's DWM material; hosts share it.
  if (!widget || closed_ || hosted_) {
    return;
  }
  // Glass, or an open Agent panel whose backdrop lies behind the content.
  const auto material = backdrop_->transparent() || backdrop_->has_hole()
                            ? FrameMaterial::kTransparent
                            : FrameMaterial::kOpaque;
  const HWND window = views::HWNDForWidget(widget);
  if (material == FrameMaterial::kTransparent) {
    // DWM tints acrylic, and its inactive fallback, by the immersive mode.
    // Follow the module's resolved base rather than Chromium's color mode,
    // or a dark theme over light acrylic reads as white.
    const KrColor base = backdrop_->base();
    const BOOL dark = ((base >> 16) & 255) * 299 + ((base >> 8) & 255) * 587 +
                          (base & 255) * 114 <
                      128000;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                          sizeof(dark));
  }
  const auto current = FrameMaterialOf(widget);
  // Upstream behaviour stays untouched until the module first asks for glass.
  if (current == material || (current == FrameMaterial::kUnset &&
                              material == FrameMaterial::kOpaque)) {
    return;
  }
  widget->SetNativeWindowProperty(
      kFrameMaterialProperty,
      reinterpret_cast<void *>(static_cast<uintptr_t>(material)));
  // Acrylic samples what is behind the window; upstream's default for
  // redirection-free browser windows is Mica.
  const DWM_SYSTEMBACKDROP_TYPE type = material == FrameMaterial::kTransparent
                                           ? DWMSBT_TRANSIENTWINDOW
                                           : DWMSBT_MAINWINDOW;
  DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
  // Re-evaluates the DWM frame extension and compositor transparency.
  widget->FrameTypeChanged();
}

void BrowserAdapter::UpdateAgentSurface() {
  if (closed_ || !backdrop_) {
    return;
  }
  gfx::RectF hole;
  const HWND window = views::HWNDForView(&*view_);
  POINT origin{};
  if (agent_surface_.width > 0 && agent_surface_.height > 0 && window &&
      ::ClientToScreen(window, &origin)) {
    // Module coordinates are DIPs from the native client origin.
    const auto client =
        display::win::GetScreenWin()
            ->ScreenToDIPRect(window, gfx::Rect(origin.x, origin.y, 0, 0))
            .origin();
    const auto view = view_->GetBoundsInScreen().origin();
    hole = gfx::RectF(agent_surface_.x + client.x() - view.x(),
                      agent_surface_.y + client.y() - view.y(),
                      agent_surface_.width, agent_surface_.height);
  }
  const bool had_hole = backdrop_->has_hole();
  if (!backdrop_->SetHole(hole, agent_surface_radius_)) {
    return;
  }
  if (had_hole != backdrop_->has_hole()) {
    UpdateFrameMaterial();
  }
  if (page_corner_overlays_) {
    page_corner_overlays_->BackdropChanged();
  }
  view_->SchedulePaint();
}

void BrowserAdapter::UpdatePageCornerOverlays() {
  if (closed_) {
    return;
  }
  // Layout can move the frame relative to the module's client origin.
  UpdateAgentSurface();
  auto *contents = view_->multi_contents_view();
  if (!contents) {
    return;
  }
  const float radius = contents->GetBackgroundRadii().upper_left();
  const bool enabled =
      !backdrop_->transparent() && radius > 0 &&
      !(WindowFlags() & (KR_WINDOW_FULLSCREEN | KR_WINDOW_TAB_FULLSCREEN)) &&
      !contents->IsInSplitView() && !(multi_page_ && multi_page_->active());
  if (enabled && !page_corner_overlays_) {
    page_corner_overlays_ =
        std::make_unique<PageCornerOverlays>(*view_, *backdrop_);
  }
  if (page_corner_overlays_) {
    page_corner_overlays_->Update(*contents, enabled, radius);
  }
}

void BrowserAdapter::PublishWindowState() {
  if (browser_services_) {
    browser_services_->OnWindowState(WindowFlags());
  }
  if (!ready() || closed_) {
    return;
  }
  auto result = session_.UpdateWindowState(WindowFlags());
  if (!result.has_value()) {
    Fail(std::move(result.error()));
    return;
  }
  anchors_.RefreshGeometry();
  const auto scale = display::Screen::Get()
                         ->GetDisplayNearestWindow(view_->GetNativeWindow())
                         .device_scale_factor();
  if (image_scale_ != scale) {
    image_scale_ = scale;
    for (const auto &[id, state] : tabs_) {
      state->image_dirty = true;
      if (auto *tab = FindTab(id)) {
        PublishTab(*tab);
      }
    }
    for (const auto &[id, action] : actions_) {
      action->image_dirty = true;
    }
    if (extensions_) {
      extensions_->RefreshImages();
    }
    ScheduleActions();
  }
}
void BrowserAdapter::Fail(std::string error) {
  LOG(ERROR) << "Cannot initialize KryptonUI: " << error;
  // The failure seam in BrowserWidget reports this before closing the window.
  view_->ReportKryptonUiFailure(std::move(error));
}

KrBrowser BrowserAdapter::MakeBrowser() {
  KrBrowser host{};
  host.struct_size = sizeof(host);
  host.self = this;
  if (!agent_panel_) {
    agent_panel_ = std::make_unique<AgentPanel>(*browser_, session_);
  }
  host.agent_panel_visibility_changed = [](void *self, bool visible) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (owner.agent_panel_) {
      owner.agent_panel_->SetVisible(visible);
    }
  };
  host.set_agent_chat_bounds = [](void *self, KrRect bounds) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (owner.agent_panel_) {
      owner.agent_panel_->SetBounds(bounds);
    }
  };
  host.agent_command = [](void *self, const KrAgentCommand *command) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    return owner.agent_panel_ ? owner.agent_panel_->Command(command)
                              : KR_ERR_UNSUPPORTED;
  };
  host.agent_request_response = [](void *self, KrAgentRecordId id,
                                   uint32_t decision) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    return owner.agent_panel_ ? owner.agent_panel_->Respond(id, decision)
                              : KR_ERR_UNSUPPORTED;
  };
  host.agent_site_flags = [](void *self, KrStr site) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    return owner.agent_panel_ ? owner.agent_panel_->SiteFlags(site) : 0u;
  };
  host.set_agent_surface = [](void *self, KrRect bounds,
                              float radius) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    const bool valid = std::isfinite(bounds.x) && std::isfinite(bounds.y) &&
                       std::isfinite(bounds.width) &&
                       std::isfinite(bounds.height) && bounds.width > 0 &&
                       bounds.height > 0 && std::isfinite(radius) &&
                       radius >= 0;
    owner.agent_surface_ = valid ? bounds : KrRect{};
    owner.agent_surface_radius_ = valid ? radius : 0;
    owner.UpdateAgentSurface();
  };
  if (!sidebar_) {
    sidebar_ = std::make_unique<SidebarServices>(
        *browser_, session_, base::BindRepeating([](tabs::TabInterface &tab) {
          return Identity(tab);
        }));
  }
  host.download_command = [](void *self, KrDownloadId id,
                             uint32_t command) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, KrDownloadId id,
           uint32_t command) {
          if (owner && owner->sidebar_) {
            owner->sidebar_->DownloadCommand(id, command);
          }
        },
        owner.weak_factory_.GetWeakPtr(), id, command));
  };
  host.media_command = [](void *self, KrMediaId id, uint32_t command) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, KrMediaId id,
           uint32_t command) {
          if (owner && owner->sidebar_) {
            owner->sidebar_->MediaCommand(id, command);
          }
        },
        owner.weak_factory_.GetWeakPtr(), id, command));
  };
  host.media_seek = [](void *self, KrMediaId id, double seconds) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, KrMediaId id, double seconds) {
          if (owner && owner->sidebar_) {
            owner->sidebar_->MediaSeek(id, seconds);
          }
        },
        owner.weak_factory_.GetWeakPtr(), id, seconds));
  };
  host.library_visibility_changed = [](void *self, uint32_t section,
                                       bool visible) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, uint32_t section,
           bool visible) {
          if (owner && owner->sidebar_) {
            owner->sidebar_->LibraryVisibility(section, visible);
          }
        },
        owner.weak_factory_.GetWeakPtr(), section, visible));
  };
  host.library_command = [](void *self, uint32_t section, KrLibraryItemId id,
                            uint32_t command) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, uint32_t section,
           KrLibraryItemId id, uint32_t command) {
          if (owner && owner->sidebar_) {
            owner->sidebar_->LibraryCommand(section, id, command);
          }
        },
        owner.weak_factory_.GetWeakPtr(), section, id, command));
  };
  host.set_appearance_prefs = [](void *self,
                                 const KrAppearancePrefs *value) noexcept {
    if (!value || !KR_HAS_FIELD(value, KrAppearancePrefs, flags)) {
      return;
    }
    const KrAppearancePrefs copy{
        sizeof(KrAppearancePrefs),
        value->color_scheme,
        value->menu_material,
        value->flags,
        KR_HAS_FIELD(value, KrAppearancePrefs, icon_pack) ? value->icon_pack
                                                          : KR_ICON_PACK_FLUENT,
        KR_HAS_FIELD(value, KrAppearancePrefs, glass_mode) ? value->glass_mode
                                                           : KR_GLASS_OFF};
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, KrAppearancePrefs value) {
          if (!owner) {
            return;
          }
          const bool agent_enabled =
              owner->browser_->GetProfile()->IsRegularProfile() &&
              krypton_agent::IsProductionAgentEnabled();
          WriteAppearancePrefs(owner->WindowPrefs(), &value, agent_enabled);
          owner->PublishLayout();
        },
        owner.weak_factory_.GetWeakPtr(), copy));
  };
  if (!privacy_features_) {
    privacy_features_ = std::make_unique<PrivacyFeaturesBridge>(
        krypton::privacy::GetRegistry(),
        base::BindRepeating(
            [](base::WeakPtr<WindowSession> session) -> const KrShellUI * {
              return session ? session->api() : nullptr;
            },
            session_.GetWeakPtr()),
        base::RepeatingCallback<std::string(int)>(),
        browser_->GetProfile()->GetPrefs());
  }
  host.privacy_features_visibility_changed = [](void *self,
                                                bool visible) noexcept {
    static_cast<BrowserAdapter *>(self)->privacy_features_->SetVisible(visible);
  };
  host.set_privacy_feature = [](void *self, KrPrivacyFeatureId id,
                                bool enabled) noexcept {
    return static_cast<BrowserAdapter *>(self)->privacy_features_->SetEnabled(
        id, enabled);
  };
  host.relaunch_browser = [](void *self) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(&chrome::AttemptRestart));
  };
  host.request_tab_switcher_previews = [](void *self, uint32_t request,
                                          const KrTabId *ids,
                                          size_t count) noexcept {
    if (!request || !ids || count == 0 || count > 6) {
      return;
    }
    auto &owner = *static_cast<BrowserAdapter *>(self);
    // ABI memory is borrowed only for this call. Defer all host/model work.
    const std::vector<KrTabId> tabs(ids, ids + count);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, uint32_t request,
           std::vector<KrTabId> tabs) {
          if (!owner) {
            return;
          }
          owner->switcher_previews_.clear();
          owner->switcher_request_ = request;
          for (const auto id : tabs) {
            if (auto *tab = owner->FindTab(id)) {
              auto preview =
                  std::make_unique<SwitcherPreview>(*owner, request, *tab);
              owner->switcher_previews_.push_back(std::move(preview));
              owner->switcher_previews_.back()->Start();
            }
          }
        },
        owner.weak_factory_.GetWeakPtr(), request, tabs));
  };
  host.cancel_tab_switcher_previews = [](void *self,
                                         uint32_t request) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, uint32_t request) {
          if (owner && owner->switcher_request_ == request) {
            owner->switcher_request_ = 0;
            owner->switcher_previews_.clear();
          }
        },
        owner.weak_factory_.GetWeakPtr(), request));
  };
  host.request_tab_preview = [](void *self, uint32_t request,
                                KrTabId id) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, uint32_t request, KrTabId id) {
          if (!owner || !request) {
            return;
          }
          auto *tab = owner->FindTab(id);
          if (!tab) {
            return;
          }
          if (!owner->tab_preview_) {
            owner->tab_preview_ = std::make_unique<TabPreview>(*owner);
          }
          owner->tab_preview_->Start(request, *tab);
        },
        owner.weak_factory_.GetWeakPtr(), request, id));
  };
  host.cancel_tab_preview = [](void *self, uint32_t request) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, uint32_t request) {
          if (owner && owner->tab_preview_) {
            owner->tab_preview_->Stop(request);
          }
        },
        owner.weak_factory_.GetWeakPtr(), request));
  };
  host.switch_space = [](void *self, KrSpaceId space) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(
        base::BindOnce(&BrowserAdapter::SwitchWorkspace,
                       owner.weak_factory_.GetWeakPtr(), space));
  };
  host.edit_workspace = [](void *self, uint32_t kind,
                           const KrSpace *space) noexcept {
    if (!space || !KR_HAS_FIELD(space, KrSpace, color)) {
      return;
    }
    auto &owner = *static_cast<BrowserAdapter *>(self);
    Workspace value{space->id, std::string(Read(space->name)),
                    std::string(Read(space->glyph)), space->color};
    std::ignore = owner.session_.Defer(base::BindOnce(
        &BrowserAdapter::EditWorkspace, owner.weak_factory_.GetWeakPtr(), kind,
        std::move(value)));
  };
  host.set_space_profile = [](void *self, KrSpaceId space, KrProfileId profile,
                              KrStr new_profile) noexcept {
    const auto name = Read(new_profile);
    if (!base::IsStringUTF8(name)) {
      return KR_ERR_INVALID_ARGUMENT;
    }
    return static_cast<BrowserAdapter *>(self)->SetSpaceProfileCommand(
        space, profile, base::UTF8ToUTF16(name));
  };
  host.create_space = [](void *self, const KrSpace *space, KrProfileId profile,
                         KrStr new_profile) noexcept {
    const auto name = Read(new_profile);
    if (!space || !KR_HAS_FIELD(space, KrSpace, color) || space->id ||
        !base::IsStringUTF8(name)) {
      return KR_ERR_INVALID_ARGUMENT;
    }
    Workspace value{0, std::string(Read(space->name)),
                    std::string(Read(space->glyph)), space->color};
    return static_cast<BrowserAdapter *>(self)->CreateSpaceCommand(
        std::move(value), profile, base::UTF8ToUTF16(name));
  };
  host.activate_tab = [](void *self, KrTabId id) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        &BrowserAdapter::Activate, owner.weak_factory_.GetWeakPtr(), id));
  };
  host.close_tab = [](void *self, KrTabId id) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        &BrowserAdapter::CloseTab, owner.weak_factory_.GetWeakPtr(), id));
  };
  host.sleep_tab = [](void *self, KrTabId id) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        &BrowserAdapter::SleepTab, owner.weak_factory_.GetWeakPtr(), id));
  };
  host.new_tab = [](void *self) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore =
        owner.session_.Defer(owner.CaptureCommand(IDC_NEW_TAB, false));
  };
  host.set_window_theme = [](void *self, const KrWorkspaceTheme *,
                             bool dark) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (owner.agent_panel_) {
      owner.agent_panel_->SetDark(dark);
    }
  };
  host.open_agent_chat = [](void *self) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (!owner.browser_->GetProfile()->IsRegularProfile() ||
        !krypton_agent::IsProductionAgentEnabled()) {
      return false;
    }
    return owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, base::OnceClosure fallback) {
          if (!owner) {
            return;
          }
          if (!krypton_agent::IsProductionAgentEnabled() ||
              !chrome::AddSelectedTabWithURL(
                  &*owner->browser_, GURL(krypton_agent::kAgentNewTabUrl),
                  ui::PAGE_TRANSITION_AUTO_TOPLEVEL)) {
            std::move(fallback).Run();
          } else if (owner && owner->agent_panel_) {
            owner->agent_panel_->Show();
          }
        },
        owner.weak_factory_.GetWeakPtr(),
        owner.CaptureCommand(IDC_NEW_TAB, false)));
  };
  host.move_tab = [](void *self, KrTabId tab, KrSpaceId space,
                     size_t index) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(
        base::BindOnce(&BrowserAdapter::MoveTab,
                       owner.weak_factory_.GetWeakPtr(), tab, space, index));
  };
  host.set_tab_flags = [](void *self, KrTabId tab, uint32_t mask,
                          uint32_t values) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(
        base::BindOnce(&BrowserAdapter::SetTabFlags,
                       owner.weak_factory_.GetWeakPtr(), tab, mask, values));
  };
  host.navigate = [](void *self, KrStr text, uint32_t disposition) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (const auto section =
            NativeSettingsSection(GURL(std::string(Read(text))))) {
      std::ignore = owner.session_.Defer(base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner, uint32_t section) {
            if (owner) {
              OpenNativeSettings(owner->session_.api(), section);
            }
          },
          owner.weak_factory_.GetWeakPtr(), *section));
      return;
    }
    auto command = owner.location_->CaptureNavigation(Read(text),
                                                      Disposition(disposition));
    if (command) {
      std::ignore = owner.session_.Defer(std::move(command));
    }
  };
  host.nav_command = [](void *self, uint32_t command) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    int id;
    switch (command) {
    case KR_NAV_BACK:
      id = IDC_BACK;
      break;
    case KR_NAV_FORWARD:
      id = IDC_FORWARD;
      break;
    case KR_NAV_RELOAD:
      id = IDC_RELOAD;
      break;
    case KR_NAV_STOP:
      id = IDC_STOP;
      break;
    case KR_NAV_HOME:
      id = IDC_HOME;
      break;
    default:
      return;
    }
    std::ignore = owner.session_.Defer(owner.CaptureCommand(id, true));
  };
  host.invoke_action = [](void *self, KrActionId id, KrRect anchor) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (id == kZoomResetAction) {
      if (auto *tab = owner.browser_->GetActiveTabInterface()) {
        std::ignore = owner.session_.Defer(base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner,
               base::WeakPtr<tabs::TabInterface> tab) {
              if (owner && owner->ready() && owner->presented_ && tab &&
                  owner->browser_->GetActiveTabInterface() == tab.get()) {
                chrome::ExecuteCommand(&*owner->browser_, IDC_ZOOM_NORMAL);
              }
            },
            owner.weak_factory_.GetWeakPtr(), tab->GetWeakPtr()));
      }
      return;
    }
    if (id == kCopyLinkAction) {
      auto *tab = owner.browser_->GetActiveTabInterface();
      if (tab) {
        std::ignore = owner.session_.Defer(base::BindOnce(
            [](base::WeakPtr<BrowserAdapter> owner,
               base::WeakPtr<tabs::TabInterface> tab) {
              if (owner && tab &&
                  owner->browser_->GetActiveTabInterface() == tab.get()) {
                owner->CopyCurrentLink();
              }
            },
            owner.weak_factory_.GetWeakPtr(), tab->GetWeakPtr()));
      }
      return;
    }
    if (id == kTranslateAction) {
      auto *tab = owner.browser_->GetActiveTabInterface();
      if (!tab || !CanShowTranslationPanel(tab->GetContents())) {
        return;
      }
      std::ignore = owner.session_.Defer(base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner,
             base::WeakPtr<tabs::TabInterface> tab,
             content::WeakDocumentPtr document, KrRect anchor) {
            if (!owner || !owner->ready() || !owner->presented_ || !tab ||
                owner->browser_->GetActiveTabInterface() != tab.get() ||
                !CanShowTranslationPanel(tab->GetContents()) ||
                document.AsRenderFrameHostIfValid() !=
                    tab->GetContents()->GetPrimaryMainFrame()) {
              return;
            }
            if (const auto *api = owner->session_.api()) {
              api->request_context_menu(api->self, KR_MENU_ACTION,
                                        kTranslateAction, anchor);
            }
          },
          owner.weak_factory_.GetWeakPtr(), tab->GetWeakPtr(),
          tab->GetContents()->GetPrimaryMainFrame()->GetWeakDocumentPtr(),
          anchor));
      return;
    }
    if (id == kSiteToolsAction) {
      auto *tab = owner.browser_->GetActiveTabInterface();
      std::ignore = owner.session_.Defer(base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner,
             base::WeakPtr<tabs::TabInterface> tab, KrRect anchor) {
            if (!owner || !tab ||
                owner->browser_->GetActiveTabInterface() != tab.get()) {
              return;
            }
            if (const auto *api = owner->session_.api()) {
              api->request_context_menu(api->self, KR_MENU_SITE_DATA, 0,
                                        anchor);
            }
          },
          owner.weak_factory_.GetWeakPtr(),
          tab ? tab->GetWeakPtr() : base::WeakPtr<tabs::TabInterface>(),
          anchor));
      return;
    }
    if (NativeExtensions::Handles(id)) {
      auto *tab = owner.browser_->GetActiveTabInterface();
      std::ignore = owner.session_.Defer(base::BindOnce(
          [](base::WeakPtr<BrowserAdapter> owner,
             base::WeakPtr<tabs::TabInterface> tab, KrActionId id) {
            if (owner && owner->extensions_ && tab &&
                tab.get() == owner->browser_->GetActiveTabInterface()) {
              owner->extensions_->Invoke(id);
            }
          },
          owner.weak_factory_.GetWeakPtr(),
          tab ? tab->GetWeakPtr() : base::WeakPtr<tabs::TabInterface>(), id));
      return;
    }
    const auto found = owner.actions_.find(id);
    auto *active = owner.browser_->GetActiveTabInterface();
    if (found == owner.actions_.end() || !active) {
      return;
    }
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner,
           base::WeakPtr<tabs::TabInterface> tab,
           base::WeakPtr<actions::ActionItem> action, KrActionId id,
           KrRect anchor) {
          if (!owner || !tab || !action ||
              tab.get() != owner->browser_->GetActiveTabInterface() ||
              !action->GetVisible() || !action->GetEnabled()) {
            return;
          }
          if (NativeMenus::SubmenuCommand(
                  static_cast<actions::ActionId>(id & 0xffffffff))) {
            if (const auto *api = owner->session_.api()) {
              api->request_context_menu(api->self, KR_MENU_ACTION,
                                        (uint64_t{2} << 56) | (id & 0xffffffff),
                                        anchor);
            }
            return;
          }
          actions::ActionInvocationContext context;
          context.SetProperty(chrome::kActionInvocationSourceKey,
                              chrome::ActionInvocationSource::kViewButton);
          action->InvokeAction(std::move(context));
        },
        owner.weak_factory_.GetWeakPtr(), active->GetWeakPtr(),
        found->second->source, id, anchor));
  };
  host.query_suggestions = [](void *self, uint32_t request,
                              KrStr text) noexcept {
    static_cast<BrowserAdapter *>(self)->location_->Query(request, Read(text));
  };
  host.handle_address_key = [](void *self, uint32_t key,
                               uint32_t modifiers) noexcept {
    return static_cast<BrowserAdapter *>(self)->location_->HandleKey(key,
                                                                     modifiers);
  };
  host.cancel_suggestions = [](void *self, uint32_t request) noexcept {
    static_cast<BrowserAdapter *>(self)->location_->CancelQuery(request);
  };
  host.start_dictation = [](void *self, uint32_t request) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (owner.dictation_) {
      owner.dictation_->Start(request);
    }
  };
  host.stop_dictation = [](void *self, uint32_t request) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (owner.dictation_) {
      owner.dictation_->Stop(request);
    }
  };
  host.address_state_changed = [](void *self,
                                  const KrAddressState *state) noexcept {
    if (state) {
      static_cast<BrowserAdapter *>(self)->location_->OnNativeState(*state);
    }
  };
  host.select_suggestion_snapshot = [](void *self, uint32_t request,
                                       uint64_t generation,
                                       size_t index) noexcept {
    static_cast<BrowserAdapter *>(self)->location_->SelectSuggestion(
        request, generation, index);
  };
  host.accept_suggestion_snapshot = [](void *self, uint32_t request,
                                       uint64_t generation, size_t index,
                                       uint32_t disposition) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    auto command = owner.location_->CaptureSuggestion(
        request, generation, index, Disposition(disposition));
    if (command) {
      std::ignore = owner.session_.Defer(std::move(command));
    }
  };
  host.request_menu = [](void *self, uint32_t request, uint32_t kind,
                         uint64_t target, KrRect) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    auto *tab = kind == KR_MENU_TAB ? owner.FindTab(target)
                                    : owner.browser_->GetActiveTabInterface();
    auto target_tab =
        tab ? tab->GetWeakPtr() : base::WeakPtr<tabs::TabInterface>();
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, uint32_t request, uint32_t kind,
           uint64_t target, base::WeakPtr<tabs::TabInterface> target_tab) {
          if (!owner) {
            return;
          }
          const auto *api = owner->session_.api();
          if (kind == KR_MENU_THEME) {
            owner->ShowThemeEditor(target ? target : owner->active_space_);
            if (api) {
              api->dismiss_menu(api->self, request);
            }
            return;
          }
          if (kind == KR_MENU_SETTINGS || kind == KR_MENU_SIDEBAR_LAYOUT) {
            if (api) {
              std::ignore =
                  api->show_settings(api->self, KR_SETTINGS_APPEARANCE);
            }
            if (api) {
              api->dismiss_menu(api->self, request);
            }
            return;
          }
          base::expected<void, std::string> result;
          if (kind == KR_MENU_APP) {
            result = owner->toolbar_->PresentAppMenu(request);
          } else if (kind == KR_MENU_ACTION &&
                     !NativeExtensions::Handles(target) &&
                     target != kTranslateAction && target != kDictationAction &&
                     !owner->actions_.contains((uint64_t{1} << 56) |
                                               (target & 0xffffffff))) {
            result = base::unexpected("The toolbar action is gone.");
          } else {
            result =
                owner->menus_->Present(request, kind, target, target_tab.get(),
                                       owner->view_->GetColorProvider());
          }
          if (!result.has_value()) {
            LOG(ERROR) << result.error();
            if (api) {
              api->dismiss_menu(api->self, request);
            }
          }
        },
        owner.weak_factory_.GetWeakPtr(), request, kind, target,
        std::move(target_tab)));
  };
  host.menu_command = [](void *self, uint32_t request, int32_t id) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    auto command = owner.toolbar_->CaptureMenuCommand(request, id);
    if (!command) {
      command = owner.menus_->CaptureCommand(request, id);
    }
    if (command) {
      std::ignore = owner.session_.Defer(std::move(command));
    }
  };
  host.menu_closed = [](void *self, uint32_t request) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, uint32_t request) {
          if (owner) {
            owner->toolbar_->OnNativeMenuClosed(request);
            owner->menus_->Closed(request);
          }
        },
        owner.weak_factory_.GetWeakPtr(), request));
  };
  host.window_command = [](void *self, uint32_t command) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(
        base::BindOnce(&BrowserAdapter::WindowCommand,
                       owner.weak_factory_.GetWeakPtr(), command));
  };
  host.set_content_bounds = [](void *self, KrRect bounds,
                               float radius) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (owner.session_.geometry().SetContent(bounds, radius)) {
      owner.view_->InvalidateLayout();
    }
  };
  host.set_shell_metrics = [](void *self,
                              const KrShellMetrics *metrics) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (metrics && owner.session_.geometry().SetMetrics(*metrics)) {
      owner.view_->InvalidateLayout();
    }
  };
  host.set_workspace_theme = [](void *self, const KrWorkspaceTheme *theme,
                                bool commit) noexcept {
    if (!theme || !KR_HAS_FIELD(theme, KrWorkspaceTheme, points)) {
      return;
    }
    KrWorkspaceTheme snapshot{};
    std::memcpy(&snapshot, theme,
                offsetof(KrWorkspaceTheme, points) + sizeof(snapshot.points));
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (!snapshot.space) {
      snapshot.space = owner.active_space_;
    }
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, KrWorkspaceTheme theme,
           bool commit) {
          if (owner) {
            owner->ApplyWorkspaceTheme(theme, commit);
          }
        },
        owner.weak_factory_.GetWeakPtr(), snapshot, commit));
  };
  host.set_window_backdrop = [](void *self,
                                const KrWindowBackdrop *backdrop) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if (backdrop && owner.backdrop_->Set(*backdrop)) {
      owner.UpdateFrameMaterial();
      owner.UpdatePageCornerOverlays();
      if (owner.page_corner_overlays_) {
        owner.page_corner_overlays_->BackdropChanged();
      }
      owner.view_->SchedulePaint();
      if (owner.multi_page_) {
        owner.multi_page_->ThemeChanged();
      }
    }
  };
  host.request_glass_backdrop =
      [](void *self, const KrGlassBackdropRequest *request) noexcept {
        auto &owner = *static_cast<BrowserAdapter *>(self);
        if (owner.closed_ || !request ||
            !KR_HAS_FIELD(request, KrGlassBackdropRequest, adapter_luid)) {
          return;
        }
        if (!owner.glass_ && request->region_count) {
          owner.glass_ = std::make_unique<NativeGlassBackdrop>(*owner.view_,
                                                               owner.session_);
        }
        if (owner.glass_) {
          owner.glass_->Request(*request);
        }
      };
  if (CanFolders()) {
    host.folder_command = [](void *self,
                             const KrFolderCommand *command) noexcept {
#if defined(__cpp_exceptions)
      try {
#endif
        if (!command || !KR_HAS_FIELD(command, KrFolderCommand, collapsed))
          return KR_ERR_INVALID_ARGUMENT;
        return static_cast<BrowserAdapter *>(self)->FolderCommand(*command);
#if defined(__cpp_exceptions)
      } catch (...) {
        return KR_ERR_PLATFORM;
      }
#endif
    };
  }
  host.set_anchor = [](void *self, uint32_t kind, uint64_t id, KrRect bounds,
                       bool visible) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    owner.anchors_.Update(kind, id, bounds, visible);
    owner.location_->OnAnchorChanged();
    owner.toolbar_->OnAnchorsChanged();
  };
  host.refresh_downloads = [](void *self) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    owner.browser_->GetProfile()
        ->GetDownloadManager()
        ->CheckForHistoryFilesRemoval();
  };
  host.edit_boost = [](void *self, uint32_t kind,
                       const KrBoost *value) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    return owner.site_customization_
               ? owner.site_customization_->EditBoost(kind, value)
               : KR_ERR_UNSUPPORTED;
  };
  host.save_routing_rule = [](void *self, const KrRoutingRule *value) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    return owner.site_customization_
               ? owner.site_customization_->SaveRoutingRule(value)
               : KR_ERR_UNSUPPORTED;
  };
  host.set_tab_icon = [](void *self, KrTabId tab, KrStr glyph) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    if ((!glyph.data && glyph.size) || glyph.size > 128) {
      return;
    }
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, KrTabId tab,
           std::string glyph) {
          if (owner && owner->site_customization_) {
            owner->site_customization_->SetTabIcon(tab, glyph);
          }
        },
        owner.weak_factory_.GetWeakPtr(), tab,
        glyph.data ? std::string(glyph.data, glyph.size) : std::string()));
  };
  host.set_layout_prefs = [](void *self, const KrLayoutPrefs *prefs) noexcept {
    if (!prefs || !KR_HAS_FIELD(prefs, KrLayoutPrefs, sidebar_width)) {
      return;
    }
    // Only a request: the profile pref change pushes the result to every
    // window, including this one.
    auto &owner = *static_cast<BrowserAdapter *>(self);
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner, KrLayoutPrefs layout) {
          if (owner) {
            WriteLayoutPrefs(owner->WindowPrefs(), layout);
          }
        },
        owner.weak_factory_.GetWeakPtr(),
        KrLayoutPrefs{sizeof(KrLayoutPrefs), prefs->flags,
                      prefs->sidebar_width}));
  };
  host.focus_page = [](void *self) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    auto *tab = owner.browser_->GetActiveTabInterface();
    if (!tab) {
      return;
    }
    std::ignore = owner.session_.Defer(base::BindOnce(
        [](base::WeakPtr<BrowserAdapter> owner,
           base::WeakPtr<tabs::TabInterface> tab) {
          if (!owner || !tab || !tab->IsActivated()) {
            return;
          }
          tab->GetContents()->Focus();
          owner->TakeKeyboardFocus();
        },
        owner.weak_factory_.GetWeakPtr(), tab->GetWeakPtr()));
  };
  host.unhandled_key = [](void *self, const void *message) noexcept {
    return message && static_cast<BrowserAdapter *>(self)->HandleUnhandledKey(
                          *static_cast<const MSG *>(message));
  };
  host.log = [](void *, uint32_t level, KrStr message) noexcept {
    if (level >= KR_LOG_WARNING) {
      LOG(WARNING) << "KryptonUI: " << Read(message);
    }
  };
  BindBrowserServices(host);
  host.multi_page_capabilities = [](void *) noexcept -> uint32_t {
    return base::FeatureList::IsEnabled(kKryptonMultiPage)
               ? KR_MULTI_PAGE_COMPLETE | KR_MULTI_PAGE_SNAP_LAYOUTS
               : 0;
  };
  host.multi_page_command = [](void *self,
                               const KrMultiPageCommand *command) noexcept {
#if defined(__cpp_exceptions)
    try {
#endif
      return command ? static_cast<BrowserAdapter *>(self)->MultiPageCommand(
                           *command)
                     : KR_ERR_INVALID_ARGUMENT;
#if defined(__cpp_exceptions)
    } catch (...) {
      return KR_ERR_PLATFORM;
    }
#endif
  };
  host.request_shortcuts = [](void *self) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    owner.shortcut_catalog_requested_ = true;
    owner.ShortcutsChanged();
  };
  host.set_shortcut = [](void *self, uint32_t command, KrShortcutChord chord,
                         uint32_t replace_conflict) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    auto *prefs = g_browser_process->local_state();
    if (!prefs || owner.closed_) {
      return KrShortcutEditResult{KR_SHORTCUT_EDIT_REJECTED, 0};
    }
    return SetShortcut(*prefs, *owner.browser_->GetProfile()->GetPrefs(),
                       command, chord, replace_conflict);
  };
  host.unassign_shortcut = [](void *self, uint32_t command) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    auto *prefs = g_browser_process->local_state();
    if (!prefs || owner.closed_) {
      return KrShortcutEditResult{KR_SHORTCUT_EDIT_REJECTED, 0};
    }
    return UnassignShortcut(*prefs, *owner.browser_->GetProfile()->GetPrefs(),
                            command);
  };
  host.reset_shortcut = [](void *self, uint32_t command) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    auto *prefs = g_browser_process->local_state();
    if (!prefs || owner.closed_) {
      return KrShortcutEditResult{KR_SHORTCUT_EDIT_REJECTED, 0};
    }
    return ResetShortcut(*prefs, *owner.browser_->GetProfile()->GetPrefs(),
                         command);
  };
  host.suspend_shortcuts = [](void *self, bool suspended) noexcept {
    auto &owner = *static_cast<BrowserAdapter *>(self);
    owner.shortcuts_suspended_ = !owner.closed_ && suspended;
  };
  return host;
}

} // namespace krypton_ui
