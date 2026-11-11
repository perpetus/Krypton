// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/site_permissions.h"

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/native_location_bar.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/page_info/chrome_page_info_delegate.h"
#include "chrome/browser/ui/page_info/chrome_page_info_ui_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "components/content_settings/core/browser/permission_settings_registry.h"
#include "components/permissions/permission_request.h"
#include "components/permissions/permission_uma_constants.h"
#include "components/permissions/permission_util.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "url/origin.h"

namespace krypton_ui {
namespace {
KrStr Borrow(const std::string& value) {
  return {value.data(), value.size()};
}
uint32_t Kind(ContentSettingsType type) {
  switch (type) {
    case ContentSettingsType::GEOLOCATION:
    case ContentSettingsType::GEOLOCATION_WITH_OPTIONS:
      return KR_PERMISSION_LOCATION;
    case ContentSettingsType::MEDIASTREAM_CAMERA:
      return KR_PERMISSION_CAMERA;
    case ContentSettingsType::MEDIASTREAM_MIC:
      return KR_PERMISSION_MICROPHONE;
    case ContentSettingsType::NOTIFICATIONS:
      return KR_PERMISSION_NOTIFICATIONS;
    case ContentSettingsType::CLIPBOARD_READ_WRITE:
      return KR_PERMISSION_CLIPBOARD;
    case ContentSettingsType::POPUPS:
      return KR_PERMISSION_POPUPS;
    case ContentSettingsType::SOUND:
      return KR_PERMISSION_SOUND;
    case ContentSettingsType::AUTOMATIC_DOWNLOADS:
      return KR_PERMISSION_AUTOMATIC_DOWNLOADS;
    case ContentSettingsType::USB_GUARD:
      return KR_PERMISSION_USB;
    case ContentSettingsType::BLUETOOTH_GUARD:
      return KR_PERMISSION_BLUETOOTH;
    case ContentSettingsType::SERIAL_GUARD:
      return KR_PERMISSION_SERIAL;
    default:
      return KR_PERMISSION_OTHER;
  }
}
uint32_t Setting(const PageInfo::PermissionInfo& value) {
  if (!value.setting)
    return KR_PERMISSION_DEFAULT;
  if (const auto* setting = std::get_if<ContentSetting>(&*value.setting))
    return static_cast<uint32_t>(*setting);
  const auto* info =
      content_settings::PermissionSettingsRegistry::GetInstance()->Get(
          value.type);
  if (info && info->delegate().IsBlocked(*value.setting))
    return KR_PERMISSION_BLOCK;
  if (info && info->delegate().IsAnyPermissionAllowed(*value.setting))
    return KR_PERMISSION_ALLOW;
  return KR_PERMISSION_ASK;
}
uint32_t Choices(const PageInfo::PermissionInfo& value,
                 ChromePageInfoUiDelegate& ui) {
  const auto* info =
      content_settings::PermissionSettingsRegistry::GetInstance()->Get(
          value.type);
  uint32_t choices = 1u << KR_PERMISSION_DEFAULT;
  if (!info)
    return choices;
  for (auto setting : {CONTENT_SETTING_ALLOW, CONTENT_SETTING_BLOCK,
                       CONTENT_SETTING_ASK, CONTENT_SETTING_SESSION_ONLY}) {
    if ((setting == CONTENT_SETTING_ALLOW && !ui.ShouldShowAllow(value.type)) ||
        (setting == CONTENT_SETTING_ASK && !ui.ShouldShowAsk(value.type)))
      continue;
    if (setting == CONTENT_SETTING_SESSION_ONLY &&
        !std::holds_alternative<ContentSetting>(value.default_setting))
      continue;
    const auto converted = info->delegate().ToPermissionSetting(setting);
    if (info->delegate().IsValid(converted))
      choices |= 1u << static_cast<uint32_t>(setting);
  }
  return choices;
}

class NativePermissionPrompt final : public permissions::PermissionPrompt {
 public:
  NativePermissionPrompt(base::WeakPtr<SitePermissions> owner, uint64_t request)
      : owner_(std::move(owner)), request_(request) {}
  ~NativePermissionPrompt() override {
    if (owner_)
      owner_->DismissPrompt(request_);
  }
  bool UpdateAnchor() override { return false; }
  TabSwitchingBehavior GetTabSwitchingBehavior() override {
    return kDestroyPromptButKeepRequestPending;
  }
  permissions::PermissionPromptDisposition GetPromptDisposition()
      const override {
    return permissions::PermissionPromptDisposition::ANCHORED_BUBBLE;
  }
  bool IsAskPrompt() const override { return true; }
  std::optional<gfx::Rect> GetViewBoundsInScreen() const override {
    return std::nullopt;
  }
  std::vector<permissions::ElementAnchoredBubbleVariant> GetPromptVariants()
      const override {
    return {};
  }
  std::optional<permissions::feature_params::PermissionElementPromptPosition>
  GetPromptPosition() const override {
    return std::nullopt;
  }

 private:
  base::WeakPtr<SitePermissions> owner_;
  const uint64_t request_;
};
}  // namespace

SitePermissions::SitePermissions(BrowserWindowInterface& browser,
                                 WindowSession& session)
    : browser_(browser), session_(session) {}
SitePermissions::~SitePermissions() {
  DismissPrompt(prompt_);
  Close(false);
}
bool SitePermissions::TargetValid() const {
  return web_contents() &&
         browser_->GetTabStripModel()->GetActiveWebContents() ==
             web_contents() &&
         web_contents()->GetLastCommittedURL() == page_url_ &&
         url::Origin::Create(page_url_).Serialize() == origin_;
}
void SitePermissions::Visibility(std::string origin, bool visible) {
  const auto request = ++visibility_request_;
  std::ignore = session_->Defer(base::BindOnce(
      &SitePermissions::ApplyVisibility, weak_factory_.GetWeakPtr(),
      std::move(origin), visible, request));
}
void SitePermissions::ApplyVisibility(std::string origin,
                                      bool visible,
                                      uint64_t request) {
  if (request != visibility_request_)
    return;
  if (!visible) {
    if (origin.empty() || origin == origin_)
      Close(false);
    return;
  }
  auto* contents = browser_->GetTabStripModel()->GetActiveWebContents();
  if (!contents || !contents->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    Close(true);
    return;
  }
  const auto current =
      url::Origin::Create(contents->GetLastCommittedURL()).Serialize();
  if (origin.empty()) {
    if (const auto* api = session_->api())
      api->show_site_permissions(api->self, Borrow(current));
    return;
  }
  if (origin != current) {
    Close(true);
    return;
  }
  Close(false);
  origin_ = current;
  page_url_ = contents->GetLastCommittedURL();
  Observe(contents);
  ui_delegate_ =
      std::make_unique<ChromePageInfoUiDelegate>(contents, page_url_);
  presenter_ = std::make_unique<PageInfo>(
      std::make_unique<ChromePageInfoDelegate>(contents), contents, page_url_);
  settings_observation_.Observe(
      HostContentSettingsMapFactory::GetForProfile(browser_->GetProfile()));
  presenter_->InitializeUiState(this, base::DoNothing());
}
void SitePermissions::Close(bool dismiss_surface) {
  ++generation_;
  refresh_pending_ = false;
  settings_observation_.Reset();
  Observe(nullptr);
  if (const auto* api = session_->api()) {
    for (const auto& [id, value] : permissions_)
      api->remove_site_permission(api->self, id);
    if (dismiss_surface)
      if (KR_HAS_SLOT(api, KrShellUI, dismiss_site_permissions) &&
          api->dismiss_site_permissions)
        api->dismiss_site_permissions(api->self);
  }
  permissions_.clear();
  objects_.clear();
  if (presenter_)
    presenter_->OnUIClosing(nullptr);
  presenter_.reset();
  ui_delegate_.reset();
  origin_.clear();
  page_url_ = GURL();
}
void SitePermissions::SetPermissionInfo(const PermissionInfoList& values,
                                        ChosenObjectInfoList objects) {
  const auto* api = session_->api();
  if (!api || !TargetValid())
    return;
  objects_ = std::move(objects);
  base::flat_map<KrPermissionId, PageInfo::PermissionInfo> next;
  for (const auto& value : values) {
    const auto previous =
        std::ranges::find_if(permissions_, [&](const auto& entry) {
          return entry.second.type == value.type &&
                 entry.second.requesting_origin == value.requesting_origin;
        });
    const auto id =
        previous == permissions_.end() ? ++next_id_ : previous->first;
    const auto label =
        base::UTF16ToUTF8(PageInfoUI::PermissionTypeToUIString(value.type));
    const auto description = base::UTF16ToUTF8(
        PageInfoUI::PermissionStateToUIString(ui_delegate_.get(), value));
    const KrSitePermission item{
        sizeof(item),
        id,
        Borrow(origin_),
        Borrow(label),
        Borrow(description),
        Kind(value.type),
        Setting(value),
        Choices(value, *ui_delegate_),
        value.source != content_settings::SettingSource::kUser};
    api->upsert_site_permission(api->self, &item);
    next.emplace(id, value);
  }
  for (const auto& [id, value] : permissions_)
    if (!next.contains(id))
      api->remove_site_permission(api->self, id);
  permissions_ = std::move(next);
}
KrResult SitePermissions::SetPermission(KrPermissionId id, uint32_t setting) {
  if (!TargetValid() || !permissions_.contains(id) ||
      setting > KR_PERMISSION_SESSION)
    return KR_ERR_INVALID_ARGUMENT;
  return session_->Defer(base::BindOnce(&SitePermissions::Apply,
                                        weak_factory_.GetWeakPtr(), id, setting,
                                        generation_))
             ? KR_OK
             : KR_ERR_PLATFORM;
}
void SitePermissions::Apply(KrPermissionId id,
                            uint32_t setting,
                            uint64_t generation) {
  if (generation != generation_ || !TargetValid() || !presenter_)
    return;
  presenter_->UpdatePermissions();
  const auto found = permissions_.find(id);
  if (found == permissions_.end())
    return;
  const auto value = found->second;
  if (value.source != content_settings::SettingSource::kUser ||
      !(Choices(value, *ui_delegate_) & (1u << setting)))
    return;
  const auto* info =
      content_settings::PermissionSettingsRegistry::GetInstance()->Get(
          value.type);
  if (!info)
    return;
  const auto converted =
      setting == KR_PERMISSION_DEFAULT
          ? std::nullopt
          : std::make_optional(info->delegate().ToPermissionSetting(
                static_cast<ContentSetting>(setting)));
  presenter_->OnSitePermissionChanged(value.type, converted,
                                      value.requesting_origin, false);
  presenter_->UpdatePermissions();
}
KrResult SitePermissions::Reset(std::string origin) {
  if (!TargetValid() || (!origin.empty() && origin != origin_))
    return KR_ERR_INVALID_ARGUMENT;
  return session_->Defer(base::BindOnce(&SitePermissions::ApplyReset,
                                        weak_factory_.GetWeakPtr(), origin_,
                                        generation_))
             ? KR_OK
             : KR_ERR_PLATFORM;
}
void SitePermissions::ApplyReset(std::string origin, uint64_t generation) {
  if (generation != generation_ || origin != origin_ || !TargetValid() ||
      !presenter_)
    return;
  const auto values = permissions_;
  auto objects = std::move(objects_);
  for (const auto& [id, value] : values)
    if (value.source == content_settings::SettingSource::kUser && value.setting)
      Apply(id, KR_PERMISSION_DEFAULT, generation);
  for (const auto& object : objects)
    if (object->chooser_object->source ==
        content_settings::SettingSource::kUser)
      presenter_->OnSiteChosenObjectDeleted(
          *object->ui_info, base::Value(object->chooser_object->value.Clone()));
  presenter_->UpdatePermissions();
}
void SitePermissions::OnContentSettingChanged(
    const ContentSettingsPattern& primary,
    const ContentSettingsPattern& secondary,
    ContentSettingsTypeSet types) {
  if (refresh_pending_ || !presenter_)
    return;
  refresh_pending_ = session_->Defer(base::BindOnce(
      &SitePermissions::Refresh, weak_factory_.GetWeakPtr(), generation_));
}
void SitePermissions::Refresh(uint64_t generation) {
  if (generation != generation_)
    return;
  refresh_pending_ = false;
  if (presenter_ && TargetValid())
    presenter_->UpdatePermissions();
}
void SitePermissions::DidFinishNavigation(content::NavigationHandle* handle) {
  if (handle->IsInPrimaryMainFrame() && handle->HasCommitted() &&
      !TargetValid())
    Close(true);
}
void SitePermissions::WebContentsDestroyed() {
  Close(true);
}
void SitePermissions::OnVisibilityChanged(content::Visibility visibility) {
  if (visibility != content::Visibility::VISIBLE)
    Close(true);
}

std::unique_ptr<permissions::PermissionPrompt> SitePermissions::CreatePrompt(
    content::WebContents* contents,
    permissions::PermissionPrompt::Delegate* delegate) {
  const auto* api = session_->api();
  if (!api || !delegate || delegate->Requests().empty() ||
      browser_->GetTabStripModel()->GetActiveWebContents() != contents ||
      delegate->ShouldCurrentRequestUseQuietUI() ||
      delegate->GetGeolocationPromptType().has_value() ||
      std::ranges::any_of(delegate->Requests(), [](const auto& request) {
        return request->IsEmbeddedPermissionElementInitiated();
      }))
    return nullptr;
  std::vector<std::string> labels;
  std::vector<KrPermissionRequest> requests;
  labels.reserve(delegate->Requests().size());
  requests.reserve(delegate->Requests().size());
  bool once = true;
  for (const auto& request : delegate->Requests()) {
    labels.push_back(base::UTF16ToUTF8(request->GetMessageTextFragment()));
    requests.push_back({sizeof(KrPermissionRequest),
                        Kind(request->GetContentSettingsType()),
                        Borrow(labels.back())});
    once &= permissions::PermissionUtil::DoesSupportTemporaryGrants(
        request->GetContentSettingsType());
  }
  DismissPrompt(prompt_);
  const auto request = ++next_prompt_;
  const auto origin =
      url::Origin::Create(delegate->GetRequestingOrigin()).Serialize();
  const auto embedding =
      url::Origin::Create(delegate->GetEmbeddingOrigin()).Serialize();
  KrRect anchor{};
  if (auto* view = BrowserView::GetBrowserViewForBrowser(&*browser_);
      view && view->krypton_ui()) {
    auto bounds = view->krypton_ui()->location_bar().Bounds();
    if (bounds.IsEmpty()) {
      bounds = views::View::ConvertRectFromScreen(
          view, contents->GetContainerBounds());
      bounds.set_height(0);
    }
    anchor = {static_cast<float>(bounds.x()), static_cast<float>(bounds.y()),
              static_cast<float>(bounds.width()),
              static_cast<float>(bounds.height())};
  }
  const KrPermissionPrompt value{sizeof(value),
                                 request,
                                 Borrow(origin),
                                 Borrow(embedding),
                                 {},
                                 requests.data(),
                                 requests.size(),
                                 sizeof(KrPermissionRequest),
                                 anchor,
                                 once,
                                 true};
  prompt_ = request;
  prompt_allow_once_ = once;
  prompt_delegate_ = delegate->GetWeakPtr();
  prompt_contents_ = contents->GetWeakPtr();
  if (api->show_permission_prompt(api->self, &value) != KR_OK) {
    DismissPrompt(request);
    return nullptr;
  }
  delegate->SetPromptShown();
  return std::make_unique<NativePermissionPrompt>(weak_factory_.GetWeakPtr(),
                                                  request);
}
void SitePermissions::DismissPrompt(uint64_t request) {
  if (!request || request != prompt_)
    return;
  prompt_ = 0;
  prompt_delegate_.reset();
  prompt_contents_.reset();
  if (const auto* api = session_->api())
    api->dismiss_permission_prompt(api->self, request);
}
void SitePermissions::Respond(uint64_t request, uint32_t decision) {
  if (request == prompt_ && decision <= KR_PERMISSION_DISMISS)
    std::ignore = session_->Defer(
        base::BindOnce(&SitePermissions::ApplyResponse,
                       weak_factory_.GetWeakPtr(), request, decision));
}
void SitePermissions::ApplyResponse(uint64_t request, uint32_t decision) {
  if (request != prompt_ || !prompt_delegate_ || !prompt_contents_ ||
      browser_->GetTabStripModel()->GetActiveWebContents() !=
          prompt_contents_.get())
    return;
  if (decision == KR_PERMISSION_ALLOW_ONCE && !prompt_allow_once_)
    return;
  auto delegate = prompt_delegate_;
  DismissPrompt(request);
  if (!delegate)
    return;
  delegate->SetDecisionTime();
  switch (decision) {
    case KR_PERMISSION_ALLOW_ONCE:
      delegate->AcceptThisTime(std::monostate{});
      break;
    case KR_PERMISSION_ALLOW_ALWAYS:
      delegate->Accept(std::monostate{});
      break;
    case KR_PERMISSION_DENY:
      delegate->Deny(std::monostate{});
      break;
    case KR_PERMISSION_DISMISS:
      delegate->Dismiss(std::monostate{});
      break;
  }
}
std::unique_ptr<permissions::PermissionPrompt> CreatePermissionPrompt(
    content::WebContents* contents,
    permissions::PermissionPrompt::Delegate* delegate) {
  auto* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  auto* view = tab ? BrowserView::GetBrowserViewForBrowser(
                         tab->GetBrowserWindowInterface())
                   : nullptr;
  auto* adapter = view ? view->krypton_ui() : nullptr;
  return adapter && adapter->site_permissions()
             ? adapter->site_permissions()->CreatePrompt(contents, delegate)
             : nullptr;
}
}  // namespace krypton_ui
