// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/site_info_menu.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/content_settings/cookie_settings_factory.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/helium_noise/helium_noise_settings.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/native_location_bar.h"
#include "chrome/browser/ui/page_info/chrome_page_info_delegate.h"
#include "chrome/browser/ui/page_info/chrome_page_info_ui_delegate.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/grit/generated_resources.h"
#include "components/browsing_data/content/browsing_data_model.h"
#include "components/content_settings/browser/page_specific_content_settings.h"
#include "components/content_settings/browser/ui/cookie_controls_util.h"
#include "components/content_settings/core/browser/content_settings_observer.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/browser/permission_settings_registry.h"
#include "components/content_settings/core/common/cookie_controls_enforcement.h"
#include "components/page_info/page_info_ui.h"
#include "components/permissions/object_permission_context_base.h"
#include "components/permissions/permission_util.h"
#include "components/strings/grit/components_branded_strings.h"
#include "components/strings/grit/components_strings.h"
#include "components/strings/grit/privacy_sandbox_strings.h"
#include "components/tabs/public/tab_interface.h"
#include "components/url_formatter/elide_url.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "net/base/schemeful_site.h"
#include "net/cookies/cookie_util.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/menu_model_delegate.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
namespace {
enum Command {
  kOrigin = 0x6b000000,
  kConnection,
  kConnectionDescription,
  kConnectionDetails,
  kCertificate,
  kQualifiedCertificate,
  kSecurityHelp,
  kResetWarnings,
  kChangePassword,
  kAllowPassword,
  kBackToSafety,
  kMarkSafe,
  kNoise,
  kNoiseDescription,
  kNoiseBreakage,
  kNoiseGlobal,
  kNoiseToggle,
  kNoiseSettings,
  kCookies,
  kCookieDescription,
  kCookieToggle,
  kCookieEnforcement,
  kCookieExpiration,
  kCookieSettings,
  kStoredData,
  kDataDescription,
  kDataEmpty,
  kRelatedSites,
  kPermissions,
  kDevices,
  kResetPermissions,
  kSiteSettings,
  kPermissionFirst = 0x6b100000,
  kDeviceFirst = 0x6b200000,
  kDataFirst = 0x6b300000,
  kRangeEnd = 0x6b400000,
};
constexpr int kStride = 16;
bool Informational(int command) {
  if (command >= kPermissionFirst)
    return command < kRangeEnd && (command % kStride) == 1;
  switch (command) {
    case kOrigin:
    case kConnectionDescription:
    case kConnectionDetails:
    case kNoiseDescription:
    case kNoiseBreakage:
    case kNoiseGlobal:
    case kCookieDescription:
    case kCookieEnforcement:
    case kCookieExpiration:
    case kDataDescription:
    case kDataEmpty:
      return true;
    default:
      return false;
  }
}
std::u16string Text(int id) {
  return l10n_util::GetStringUTF16(id);
}

struct State {
  struct Row {
    std::u16string label, detail;
    bool enabled = true, visible = true, checked = false;
  };
  std::map<int, Row> rows;
};
class MenuPart : public ui::SimpleMenuModel {
 public:
  MenuPart(Delegate* delegate, State& state)
      : SimpleMenuModel(delegate), state_(state) {}
  ItemType GetTypeAt(size_t index) const override {
    return Informational(GetCommandIdAt(index))
               ? TYPE_TITLE
               : SimpleMenuModel::GetTypeAt(index);
  }
  std::u16string GetSecondaryLabelAt(size_t index) const override {
    const auto found = state_->rows.find(GetCommandIdAt(index));
    return found == state_->rows.end() ? std::u16string{}
                                       : found->second.detail;
  }

 private:
  const raw_ref<State> state_;
};

class SiteInfoMenu final : public MenuPart,
                           public ui::SimpleMenuModel::Delegate,
                           public PageInfoUI,
                           public content_settings::Observer,
                           public content::WebContentsObserver {
 public:
  SiteInfoMenu(BrowserWindowInterface& browser, tabs::TabInterface& tab)
      : MenuPart(this, state_),
        WebContentsObserver(tab.GetContents()),
        browser_(browser),
        page_url_(tab.GetContents()->GetLastCommittedURL()),
        origin_(url::Origin::Create(page_url_).Serialize()),
        connection_(this, state_),
        noise_(this, state_),
        cookies_(this, state_),
        permissions_(this, state_),
        devices_(this, state_),
        data_(this, state_) {
    AddItem(kOrigin, {});
    AddSubMenu(kConnection, {}, &connection_);
    AddSubMenu(kNoise, {}, &noise_);
    AddSubMenu(kCookies, {}, &cookies_);
    AddSubMenu(kPermissions, {}, &permissions_);
    AddSubMenu(kDevices, {}, &devices_);
    AddItem(kResetPermissions, {});
    AddSeparator(ui::NORMAL_SEPARATOR);
    AddItem(kSiteSettings, {});
    for (const int id :
         {kConnectionDescription, kConnectionDetails, kCertificate,
          kQualifiedCertificate, kResetWarnings, kSecurityHelp, kChangePassword,
          kAllowPassword, kBackToSafety, kMarkSafe})
      connection_.AddItem(id, {});
    for (const int id : {kNoiseDescription, kNoiseGlobal, kNoiseBreakage})
      noise_.AddItem(id, {});
    noise_.AddCheckItem(kNoiseToggle, {});
    noise_.AddItem(kNoiseSettings, {});
    cookies_.AddItem(kCookieDescription, {});
    cookies_.AddCheckItem(kCookieToggle, {});
    for (const int id : {kCookieEnforcement, kCookieExpiration})
      cookies_.AddItem(id, {});
    cookies_.AddSubMenu(kStoredData, {}, &data_);
    cookies_.AddItem(kRelatedSites, {});
    cookies_.AddItem(kCookieSettings, {});
    data_.AddItem(kDataDescription, {});
    data_.AddItem(kDataEmpty, {});
    for (const int id :
         {kCertificate, kQualifiedCertificate, kResetWarnings, kChangePassword,
          kAllowPassword, kBackToSafety, kMarkSafe, kRelatedSites,
          kCookieToggle, kCookieEnforcement, kCookieExpiration, kPermissions,
          kDevices, kResetPermissions, kNoiseGlobal})
      state_.rows[id].visible = false;
    state_.rows[kOrigin].label = url_formatter::FormatUrlForSecurityDisplay(
        page_url_, url_formatter::SchemeDisplay::OMIT_HTTP_AND_HTTPS);
    state_.rows[kConnection].label = Text(IDS_PAGE_INFO_NOT_SECURE_SUMMARY);
    state_.rows[kConnectionDescription].label = state_.rows[kConnection].label;
    Label(kCertificate, IDS_PAGE_INFO_CERTIFICATE_DETAILS);
    Label(kQualifiedCertificate, IDS_PAGE_INFO_QWAC_STATUS_TITLE);
    Label(kResetWarnings,
          IDS_PAGE_INFO_RESET_INVALID_CERTIFICATE_DECISIONS_BUTTON);
    Label(kSecurityHelp, IDS_PAGE_INFO_HELP_CENTER_LINK);
    Label(kChangePassword, IDS_PAGE_INFO_CHANGE_PASSWORD_BUTTON);
    Label(kAllowPassword, IDS_PAGE_INFO_ALLOWLIST_PASSWORD_REUSE_BUTTON);
    Label(kBackToSafety, IDS_PAGE_INFO_SUSPICIOUS_SITE_BUTTON_BACK_TO_SAFETY);
    Label(kMarkSafe, IDS_PAGE_INFO_SUSPICIOUS_SITE_BUTTON_MARK_AS_SAFE);
    Label(kNoise, IDS_PAGE_INFO_HELIUM_NOISE_HEADER);
    Label(kNoiseDescription, IDS_PAGE_INFO_HELIUM_NOISE_DESCRIPTION);
    Label(kNoiseBreakage, IDS_PAGE_INFO_HELIUM_NOISE_BREAKAGE);
    Label(kNoiseGlobal, IDS_PAGE_INFO_HELIUM_NOISE_GLOBALLY_DISABLED);
    Label(kNoiseToggle, IDS_PAGE_INFO_HELIUM_NOISE_HEADER);
    Label(kNoiseSettings, IDS_PAGE_INFO_HELIUM_NOISE_SETTINGS_BUTTON);
    Label(kCookies, IDS_PAGE_INFO_COOKIES_HEADER);
    state_.rows[kCookieDescription].label =
        l10n_util::GetStringFUTF16(IDS_PAGE_INFO_COOKIES_DESCRIPTION,
                                   Text(IDS_PAGE_INFO_COOKIES_SETTINGS_LINK));
    Label(kCookieToggle, IDS_PAGE_INFO_COOKIES_THIRD_PARTY_COOKIES_LABEL);
    Label(kCookieSettings, IDS_PAGE_INFO_COOKIES_SETTINGS_LINK);
    Label(kStoredData, IDS_PAGE_INFO_COOKIES_DIALOG_BUTTON_TITLE);
    Label(kDataDescription, IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_SUBTITLE);
    Label(kDataEmpty, IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_EMPTY_STATE_LABEL);
    Label(kRelatedSites, IDS_PAGE_INFO_COOKIES);
    Label(kPermissions, IDS_SETTINGS_SITE_SETTINGS_PERMISSIONS);
    state_.rows[kDevices].label = u"Connected devices";
    Label(kResetPermissions, IDS_PAGE_INFO_RESET_PERMISSIONS);
    Label(kSiteSettings, IDS_PAGE_INFO_SITE_SETTINGS_LINK);
    ui_delegate_ =
        std::make_unique<ChromePageInfoUiDelegate>(web_contents(), page_url_);
    if (!PageInfo::IsFileOrInternalPage(page_url_) &&
        !page_url_.SchemeIs("chrome-extension")) {
      presenter_ = std::make_unique<PageInfo>(
          std::make_unique<ChromePageInfoDelegate>(web_contents()),
          web_contents(), page_url_);
      settings_.Observe(
          HostContentSettingsMapFactory::GetForProfile(browser_->GetProfile()));
      presenter_->InitializeUiState(this, base::DoNothing());
      RefreshNoise();
      RefreshData();
    } else {
      const int description =
          page_url_.SchemeIsFile() ? IDS_PAGE_INFO_FILE_PAGE
          : page_url_.SchemeIs("chrome-extension")
              ? IDS_PAGE_INFO_EXTENSION_PAGE
          : page_url_.SchemeIs("view-source") ? IDS_PAGE_INFO_VIEW_SOURCE_PAGE
          : page_url_.SchemeIs("devtools")    ? IDS_PAGE_INFO_DEVTOOLS_PAGE
                                              : IDS_PAGE_INFO_INTERNAL_PAGE;
      state_.rows[kConnection].label = Text(description);
      state_.rows[kConnectionDescription].label =
          state_.rows[kConnection].label;
      for (const int id : {kNoise, kCookies, kSecurityHelp, kSiteSettings})
        state_.rows[id].visible = false;
    }
  }
  ~SiteInfoMenu() override {
    weak_factory_.InvalidateWeakPtrs();
    settings_.Reset();
    if (presenter_ && web_contents())
      presenter_->OnUIClosing(nullptr);
  }
  bool IsCommandIdEnabled(int id) const override {
    const auto found = state_.rows.find(id);
    return Current() && found != state_.rows.end() && found->second.enabled;
  }
  bool valid() const { return Current(); }
  bool IsCommandIdVisible(int id) const override {
    if (id < 0)
      return true;
    const auto found = state_.rows.find(id);
    return found != state_.rows.end() && found->second.visible;
  }
  bool IsCommandIdChecked(int id) const override {
    const auto found = state_.rows.find(id);
    return found != state_.rows.end() && found->second.checked;
  }
  bool IsItemForCommandIdDynamic(int) const override { return true; }
  std::u16string GetLabelForCommandId(int id) const override {
    const auto found = state_.rows.find(id);
    return found == state_.rows.end() ? std::u16string{} : found->second.label;
  }
  void ExecuteCommand(int id, int) override;

 private:
  struct PermissionRow {
    PermissionRow(int command, SiteInfoMenu& owner)
        : command(command), menu(&owner, owner.state_) {}
    int command;
    MenuPart menu;
    PageInfo::PermissionInfo value;
    bool seen = false;
  };
  struct DeviceRow {
    DeviceRow(int command, SiteInfoMenu& owner)
        : command(command), menu(&owner, owner.state_) {}
    int command;
    MenuPart menu;
    std::unique_ptr<ChosenObjectInfo> value;
    bool seen = false;
  };
  struct DataRow {
    DataRow(int command, SiteInfoMenu& owner, url::Origin origin)
        : command(command),
          menu(&owner, owner.state_),
          origin(std::move(origin)) {}
    int command;
    MenuPart menu;
    url::Origin origin;
    ContentSetting setting = CONTENT_SETTING_ALLOW;
    bool seen = false, allowed = false, partitioned = true, managed = false;
  };
  bool Current() const {
    return document_valid_ && web_contents() &&
           !web_contents()->IsBeingDestroyed() &&
           browser_->GetActiveTabInterface() &&
           browser_->GetActiveTabInterface()->GetContents() == web_contents() &&
           web_contents()->GetLastCommittedURL() == page_url_;
  }
  void Label(int command, int resource) {
    state_.rows[command].label = Text(resource);
  }
  void Changed() {
    if (menu_model_delegate())
      menu_model_delegate()->OnMenuStructureChanged();
  }
  void RefreshNoise() {
    const auto noise = helium_noise::GetHeliumNoiseState(
        web_contents()->GetBrowserContext(), page_url_);
    state_.rows[kNoise].visible =
        helium_noise::CanSetHeliumNoiseForOrigin(page_url_) &&
        noise != helium_noise::HeliumNoiseState::kUnavailable;
    state_.rows[kNoiseToggle].enabled = state_.rows[kNoise].visible;
    state_.rows[kNoiseToggle].visible =
        noise != helium_noise::HeliumNoiseState::kGloballyDisabled;
    state_.rows[kNoiseBreakage].visible = state_.rows[kNoiseToggle].visible;
    state_.rows[kNoiseToggle].checked =
        noise == helium_noise::HeliumNoiseState::kEnabled;
    state_.rows[kNoiseGlobal].visible =
        noise == helium_noise::HeliumNoiseState::kGloballyDisabled;
  }
  void SetIdentityInfo(const IdentityInfo& value) override {
    const auto description = GetSecurityDescription(value);
    state_.rows[kConnection].label = description->summary;
    state_.rows[kConnectionDescription].label = description->summary;
    state_.rows[kConnectionDescription].detail = description->details;
    state_.rows[kConnectionDetails].label =
        base::UTF8ToUTF16(value.connection_status_description);
    state_.rows[kConnectionDetails].visible =
        !value.connection_status_description.empty();
    security_type_ = description->type;
    certificate_ = value.certificate;
    qualified_certificate_ = value.two_qwac;
    state_.rows[kCertificate].visible = !!certificate_;
    state_.rows[kQualifiedCertificate].visible = !!qualified_certificate_;
    if (certificate_) {
      state_.rows[kCertificate].label =
          Text(value.identity_status == PageInfo::SITE_IDENTITY_STATUS_ERROR
                   ? IDS_PAGE_INFO_CERTIFICATE_DETAILS
                   : IDS_PAGE_INFO_CERTIFICATE_IS_VALID);
      state_.rows[kCertificate].detail =
          base::UTF8ToUTF16(certificate_->issuer().GetDisplayName());
    }
    state_.rows[kResetWarnings].visible = value.show_ssl_decision_revoke_button;
    state_.rows[kChangePassword].visible = value.show_change_password_buttons;
    state_.rows[kAllowPassword].visible = value.show_change_password_buttons;
    state_.rows[kBackToSafety].visible =
        value.safe_browsing_status ==
        PageInfo::SAFE_BROWSING_STATUS_WARNABLE_SUSPICIOUS_SITE;
    state_.rows[kMarkSafe].visible = state_.rows[kBackToSafety].visible;
    Changed();
  }
  void SetCookieInfo(const CookiesInfo& value) override {
    cookies_state_ = value.controls_state;
    state_.rows[kCookieToggle].visible =
        value.controls_state != CookieControlsState::kHidden;
    state_.rows[kCookieToggle].checked =
        value.controls_state == CookieControlsState::kAllowed3pc;
    state_.rows[kCookieToggle].enabled =
        value.enforcement == CookieControlsEnforcement::kNoEnforcement;
    state_.rows[kCookieToggle].detail =
        Text(state_.rows[kCookieToggle].checked
                 ? IDS_TRACKING_PROTECTION_BUBBLE_3PC_ALLOWED_SUBTITLE
                 : IDS_TRACKING_PROTECTION_BUBBLE_3PC_BLOCKED_SUBTITLE);
    state_.rows[kCookieEnforcement].visible =
        value.enforcement != CookieControlsEnforcement::kNoEnforcement;
    if (state_.rows[kCookieEnforcement].visible)
      state_.rows[kCookieEnforcement].label =
          content_settings::CookieControlsUtil::GetEnforcedTooltip(
              value.enforcement);
    state_.rows[kCookieExpiration].visible = !value.expiration.is_null();
    if (state_.rows[kCookieExpiration].visible)
      state_.rows[kCookieExpiration].label = l10n_util::GetPluralStringFUTF16(
          IDS_PAGE_INFO_TRACKING_PROTECTION_COOKIES_BLOCKED_RESTART_TITLE,
          content_settings::CookieControlsUtil::GetDaysToExpiration(
              value.expiration));
    state_.rows[kStoredData].detail =
        value.allowed_sites_count < 0
            ? std::u16string{}
            : l10n_util::GetPluralStringFUTF16(
                  IDS_PAGE_INFO_COOKIES_ALLOWED_SITES_COUNT,
                  value.allowed_sites_count);
    state_.rows[kRelatedSites].visible = value.rws_info.has_value();
    if (value.rws_info)
      state_.rows[kRelatedSites].detail = value.rws_info->owner_name;
    RefreshData();
    Changed();
  }
  void SetPermissionInfo(const PermissionInfoList& values,
                         ChosenObjectInfoList objects) override;
  void RefreshData();
  void Refresh() {
    refresh_pending_ = false;
    if (!Current()) {
      Changed();
      return;
    }
    presenter_->UpdatePermissions();
    presenter_->UpdateSecurityState();
    RefreshNoise();
    RefreshData();
    Changed();
  }
  void OnContentSettingChanged(const ContentSettingsPattern&,
                               const ContentSettingsPattern&,
                               ContentSettingsTypeSet) override {
    if (refresh_pending_ || !presenter_)
      return;
    refresh_pending_ = true;
    if (auto* view = BrowserView::GetBrowserViewForBrowser(&*browser_);
        view && view->krypton_ui())
      std::ignore = view->krypton_ui()->session().Defer(
          base::BindOnce(&SiteInfoMenu::Refresh, weak_factory_.GetWeakPtr()));
  }
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (handle->IsInPrimaryMainFrame() && handle->HasCommitted() && !Current())
      Changed();
  }
  void PrimaryPageChanged(content::Page&) override {
    document_valid_ = false;
    Changed();
  }
  void DidChangeVisibleSecurityState() override {
    if (presenter_ && Current())
      presenter_->UpdateSecurityState();
  }
  void OnVisibilityChanged(content::Visibility visibility) override {
    if (visibility != content::Visibility::VISIBLE)
      Changed();
  }
  void WebContentsDestroyed() override { Changed(); }
  void UpdatePermission(PermissionRow& row, int option);
  void ResetPermissions();
  State state_;
  const raw_ref<BrowserWindowInterface> browser_;
  const GURL page_url_;
  const std::string origin_;
  MenuPart connection_, noise_, cookies_, permissions_, devices_, data_;
  std::unique_ptr<ChromePageInfoUiDelegate> ui_delegate_;
  std::unique_ptr<PageInfo> presenter_;
  base::ScopedObservation<HostContentSettingsMap, content_settings::Observer>
      settings_{this};
  std::map<std::pair<ContentSettingsType, std::string>,
           std::unique_ptr<PermissionRow>>
      permission_rows_;
  std::map<std::pair<ContentSettingsType, std::string>,
           std::unique_ptr<DeviceRow>>
      device_rows_;
  std::map<url::Origin, std::unique_ptr<DataRow>> data_rows_;
  scoped_refptr<net::X509Certificate> certificate_, qualified_certificate_;
  SecurityDescriptionType security_type_ = SecurityDescriptionType::CONNECTION;
  CookieControlsState cookies_state_ = CookieControlsState::kHidden;
  int next_permission_ = kPermissionFirst, next_device_ = kDeviceFirst,
      next_data_ = kDataFirst;
  bool refresh_pending_ = false;
  bool document_valid_ = true;
  base::WeakPtrFactory<SiteInfoMenu> weak_factory_{this};
};

void SiteInfoMenu::SetPermissionInfo(const PermissionInfoList& values,
                                     ChosenObjectInfoList objects) {
  for (auto& [key, row] : permission_rows_)
    row->seen = false;
  bool can_reset = false;
  for (const auto& value : values) {
    const auto key =
        std::pair{value.type, value.requesting_origin
                                  ? value.requesting_origin->Serialize()
                                  : std::string{}};
    auto& row = permission_rows_[key];
    if (!row) {
      row = std::make_unique<PermissionRow>(next_permission_, *this);
      next_permission_ += kStride;
      row->menu.AddItem(row->command + 1, {});
      for (int option = 0; option <= CONTENT_SETTING_SESSION_ONLY; ++option)
        row->menu.AddRadioItem(row->command + 2 + option, {}, row->command);
      row->menu.AddCheckItem(row->command + 7, {});
      row->menu.AddItem(row->command + 8, {});
      permissions_.AddSubMenu(row->command, {}, &row->menu);
    }
    row->seen = true;
    row->value = value;
    const auto* definition =
        content_settings::PermissionSettingsRegistry::GetInstance()->Get(
            value.type);
    const bool managed = value.source != content_settings::SettingSource::kUser;
    state_.rows[row->command].label =
        PageInfoUI::PermissionTypeToUIString(value.type);
    state_.rows[row->command].detail =
        PageInfoUI::PermissionStateToUIString(ui_delegate_.get(), value);
    state_.rows[row->command + 1].label = state_.rows[row->command].label;
    state_.rows[row->command + 1].detail =
        ui_delegate_->GetPermissionDetail(value.type);
    if (value.requesting_origin)
      state_.rows[row->command + 1].detail +=
          u" " + base::UTF8ToUTF16(value.requesting_origin->Serialize());
    for (int option = 0; option <= CONTENT_SETTING_SESSION_ONLY; ++option) {
      const auto setting = static_cast<ContentSetting>(option);
      bool available =
          definition &&
          !(setting == CONTENT_SETTING_SESSION_ONLY &&
            !std::holds_alternative<ContentSetting>(value.default_setting));
      if (available && setting != CONTENT_SETTING_DEFAULT)
        available = definition->delegate().IsValid(
            definition->delegate().ToPermissionSetting(setting));
      if (setting == CONTENT_SETTING_ALLOW &&
          !ui_delegate_->ShouldShowAllow(value.type))
        available = false;
      if (setting == CONTENT_SETTING_ASK &&
          !ui_delegate_->ShouldShowAsk(value.type))
        available = false;
      auto sample = value;
      sample.is_one_time = false;
      if (setting == CONTENT_SETTING_DEFAULT)
        sample.setting.reset();
      else if (available)
        sample.setting = definition->delegate().ToPermissionSetting(setting);
      auto& choice = state_.rows[row->command + 2 + option];
      choice.label =
          PageInfoUI::PermissionStateToUIString(ui_delegate_.get(), sample);
      choice.visible = available;
      choice.enabled = available && !managed;
      choice.checked = sample.setting == value.setting;
    }
    auto& remember = state_.rows[row->command + 7];
    remember.label =
        Text(IDS_PAGE_INFO_PERMISSIONS_SUBPAGE_REMEMBER_THIS_SETTING);
    remember.visible =
        value.setting && definition &&
        permissions::PermissionUtil::DoesSupportTemporaryGrants(value.type) &&
        definition->delegate().IsAnyPermissionAllowed(*value.setting);
    remember.enabled = !managed;
    remember.checked = !value.is_one_time;
    state_.rows[row->command + 8].label =
        Text(IDS_PAGE_INFO_PERMISSIONS_SUBPAGE_MANAGE_BUTTON);
    can_reset |= !managed && value.setting.has_value();
  }
  for (auto& [key, row] : permission_rows_)
    state_.rows[row->command].visible = row->seen;
  state_.rows[kPermissions].visible = !values.empty();
  for (auto& [key, row] : device_rows_)
    row->seen = false;
  for (auto& object : objects) {
    const auto key = std::pair{
        object->ui_info->content_settings_type,
        base::Value(object->chooser_object->value.Clone()).DebugString()};
    auto& row = device_rows_[key];
    if (!row) {
      row = std::make_unique<DeviceRow>(next_device_, *this);
      next_device_ += kStride;
      row->menu.AddItem(row->command + 1, {});
      row->menu.AddItem(row->command + 2, {});
      devices_.AddSubMenu(row->command, {}, &row->menu);
    }
    row->seen = true;
    row->value = std::move(object);
    const auto& value = *row->value;
    auto* context = presenter_->GetChooserContextFromUIInfo(*value.ui_info);
    state_.rows[row->command].label =
        context
            ? context->GetObjectDisplayNameForUI(value.chooser_object->value)
            : Text(value.ui_info->description_string_id);
    const bool managed =
        value.chooser_object->source != content_settings::SettingSource::kUser;
    state_.rows[row->command + 1].label =
        Text(managed ? value.ui_info->allowed_by_policy_description_string_id
                     : value.ui_info->description_string_id);
    state_.rows[row->command + 2].label = Text(IDS_DELETE);
    state_.rows[row->command + 2].enabled = !managed;
    can_reset |= !managed;
  }
  bool has_devices = false;
  for (auto& [key, row] : device_rows_) {
    state_.rows[row->command].visible = row->seen;
    has_devices |= row->seen;
  }
  state_.rows[kDevices].visible = has_devices;
  state_.rows[kResetPermissions].visible = can_reset;
  state_.rows[kResetPermissions].label = l10n_util::GetPluralStringFUTF16(
      IDS_PAGE_INFO_RESET_PERMISSIONS, static_cast<int>(values.size()));
  Changed();
}

void SiteInfoMenu::RefreshData() {
  if (!Current())
    return;
  auto* content_settings =
      content_settings::PageSpecificContentSettings::GetForFrame(
          web_contents()->GetPrimaryMainFrame());
  if (!content_settings)
    return;
  for (auto& [key, row] : data_rows_) {
    row->seen = false;
    row->allowed = false;
    row->partitioned = true;
  }
  auto* cookie_settings =
      HostContentSettingsMapFactory::GetForProfile(browser_->GetProfile());
  const auto collect = [&](BrowsingDataModel* model, bool allowed) {
    if (!model)
      return;
    for (const auto& entry : *model) {
      const auto origin = std::visit(
          [&](const auto& owner) -> url::Origin {
            if constexpr (std::is_same_v<std::decay_t<decltype(owner)>,
                                         url::Origin>)
              return owner;
            else
              return url::Origin::Create(net::cookie_util::CookieOriginToURL(
                  owner, page_url_.SchemeIsCryptographic()));
          },
          *entry.data_owner);
      auto& row = data_rows_[origin];
      if (!row) {
        row = std::make_unique<DataRow>(next_data_, *this, origin);
        next_data_ += kStride;
        row->menu.AddItem(row->command + 1, {});
        for (int option = CONTENT_SETTING_DEFAULT;
             option <= CONTENT_SETTING_SESSION_ONLY; ++option)
          if (option != CONTENT_SETTING_ASK)
            row->menu.AddRadioItem(row->command + 2 + option, {}, row->command);
        row->menu.AddSeparator(ui::NORMAL_SEPARATOR);
        row->menu.AddItem(row->command + 8, {});
        data_.AddSubMenu(row->command, {}, &row->menu);
      }
      row->seen = true;
      row->allowed |= allowed;
      row->partitioned &= entry.GetThirdPartyPartitioningSite().has_value();
      content_settings::SettingInfo info;
      row->setting = cookie_settings->GetContentSetting(
          origin.GetURL(), page_url_, ContentSettingsType::COOKIES, &info);
      row->managed = info.source != content_settings::SettingSource::kUser;
      state_.rows[row->command].label = base::UTF8ToUTF16(origin.Serialize());
    }
  };
  collect(content_settings->allowed_browsing_data_model(), true);
  collect(content_settings->blocked_browsing_data_model(), false);
  bool has_data = false;
  for (auto& [origin, row] : data_rows_) {
    state_.rows[row->command].visible = row->seen;
    has_data |= row->seen;
    if (!row->seen)
      continue;
    const int description =
        !row->allowed
            ? IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_BLOCKED_STATE_SUBTITLE
        : row->setting == CONTENT_SETTING_SESSION_ONLY
            ? (row->partitioned
                   ? IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_PARTITIONED_SESSION_ONLY_STATE_SUBTITLE
                   : IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_SESSION_ONLY_STATE_SUBTITLE)
        : row->partitioned
            ? IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_PARTITIONED_STATE_SUBTITLE
            : IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_ALLOWED_STATE_SUBTITLE;
    state_.rows[row->command].detail = Text(description);
    state_.rows[row->command + 1].label = state_.rows[row->command].label;
    state_.rows[row->command + 1].detail = state_.rows[row->command].detail;
    for (const auto [option, label] :
         {std::pair{CONTENT_SETTING_DEFAULT, IDS_PAGE_INFO_SITE_SETTINGS_LINK},
          {CONTENT_SETTING_ALLOW,
           IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_ALLOW_MENU_ITEM},
          {CONTENT_SETTING_BLOCK,
           IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_BLOCK_MENU_ITEM},
          {CONTENT_SETTING_SESSION_ONLY,
           IDS_PAGE_SPECIFIC_SITE_DATA_DIALOG_SESSION_ONLY_MENU_ITEM}}) {
      auto& choice = state_.rows[row->command + 2 + option];
      choice.label =
          option == CONTENT_SETTING_DEFAULT ? u"Use default" : Text(label);
      choice.enabled = !row->managed && origin.GetURL().SchemeIsHTTPOrHTTPS();
      choice.checked = option == row->setting;
    }
    state_.rows[row->command + 8].label = Text(IDS_DELETE);
  }
  state_.rows[kDataEmpty].visible = !has_data;
}

void SiteInfoMenu::UpdatePermission(PermissionRow& row, int option) {
  if (!row.seen)
    return;
  const auto value = row.value;
  if (option == 8) {
    presenter_->OpenContentSettingsExceptions(value.type);
    return;
  }
  if (value.source != content_settings::SettingSource::kUser)
    return;
  if (option == 7) {
    if (state_.rows[row.command + option].visible && value.setting)
      presenter_->OnSitePermissionChanged(value.type, value.setting,
                                          value.requesting_origin,
                                          !value.is_one_time);
    return;
  }
  const int setting = option - 2;
  if (setting < CONTENT_SETTING_DEFAULT ||
      setting > CONTENT_SETTING_SESSION_ONLY ||
      !state_.rows[row.command + option].enabled ||
      !state_.rows[row.command + option].visible)
    return;
  const auto* definition =
      content_settings::PermissionSettingsRegistry::GetInstance()->Get(
          value.type);
  if (!definition)
    return;
  presenter_->OnSitePermissionChanged(
      value.type,
      setting == CONTENT_SETTING_DEFAULT
          ? std::nullopt
          : std::make_optional(definition->delegate().ToPermissionSetting(
                static_cast<ContentSetting>(setting))),
      value.requesting_origin, false);
}
void SiteInfoMenu::ResetPermissions() {
  std::vector<PageInfo::PermissionInfo> values;
  for (const auto& [key, row] : permission_rows_)
    if (row->seen &&
        row->value.source == content_settings::SettingSource::kUser &&
        row->value.setting)
      values.push_back(row->value);
  for (const auto& value : values)
    presenter_->OnSitePermissionChanged(value.type, std::nullopt,
                                        value.requesting_origin, false);
  for (const auto& [key, row] : device_rows_)
    if (row->seen && row->value &&
        row->value->chooser_object->source ==
            content_settings::SettingSource::kUser)
      presenter_->OnSiteChosenObjectDeleted(
          *row->value->ui_info,
          base::Value(row->value->chooser_object->value.Clone()));
}
void SiteInfoMenu::ExecuteCommand(int id, int) {
  if (!Current() || !presenter_ || Informational(id))
    return;
  presenter_->UpdatePermissions();
  RefreshNoise();
  RefreshData();
  if (!IsCommandIdEnabled(id) || !IsCommandIdVisible(id))
    return;
  if (id >= kPermissionFirst && id < kDeviceFirst) {
    for (auto& [key, row] : permission_rows_)
      if (id >= row->command && id < row->command + kStride) {
        UpdatePermission(*row, id - row->command);
        break;
      }
  } else if (id >= kDeviceFirst && id < kDataFirst) {
    for (auto& [key, row] : device_rows_)
      if (id == row->command + 2 && row->seen && row->value &&
          row->value->chooser_object->source ==
              content_settings::SettingSource::kUser) {
        presenter_->OnSiteChosenObjectDeleted(
            *row->value->ui_info,
            base::Value(row->value->chooser_object->value.Clone()));
        break;
      }
  } else if (id >= kDataFirst && id < kRangeEnd) {
    for (auto& [origin, row] : data_rows_) {
      if (id < row->command || id >= row->command + kStride || !row->seen)
        continue;
      const int option = id - row->command;
      if (option == 8) {
        auto* settings =
            content_settings::PageSpecificContentSettings::GetForFrame(
                web_contents()->GetPrimaryMainFrame());
        if (settings)
          for (auto* model : {settings->allowed_browsing_data_model(),
                              settings->blocked_browsing_data_model()})
            if (model) {
              model->RemoveBrowsingData(origin.host(), base::DoNothing());
              model->RemovePartitionedBrowsingData(
                  origin.host(), net::SchemefulSite(origin), base::DoNothing());
            }
      } else if (!row->managed && origin.GetURL().SchemeIsHTTPOrHTTPS()) {
        auto* settings =
            CookieSettingsFactory::GetForProfile(browser_->GetProfile()).get();
        settings->ResetCookieSetting(origin.GetURL());
        if (option != 2)
          settings->SetCookieSetting(origin.GetURL(),
                                     static_cast<ContentSetting>(option - 2));
      }
      break;
    }
  } else
    switch (id) {
      case kCertificate:
        if (certificate_)
          presenter_->OpenCertificateDialog(certificate_.get());
        break;
      case kQualifiedCertificate:
        if (qualified_certificate_)
          presenter_->OpenCertificateDialog(qualified_certificate_.get());
        break;
      case kResetWarnings:
        presenter_->OnRevokeSSLErrorBypassButtonPressed();
        break;
      case kChangePassword:
        presenter_->OnChangePasswordButtonPressed();
        break;
      case kAllowPassword:
        presenter_->OnAllowlistPasswordReuseButtonPressed();
        break;
      case kBackToSafety:
        presenter_->OnSuspiciousSiteBackToSafety();
        break;
      case kMarkSafe:
        presenter_->OnSuspiciousSiteMarkAsSafe();
        break;
      case kSecurityHelp:
        if (security_type_ == SecurityDescriptionType::SAFETY_TIP)
          presenter_->OpenSafetyTipHelpCenterPage();
        else if (security_type_ == SecurityDescriptionType::SAFE_BROWSING)
          presenter_->OpenSafeBrowsingHelpCenterPage();
        else {
          const ui::KeyEvent event(ui::EventType::kKeyPressed, ui::VKEY_RETURN,
                                   0);
          presenter_->OpenConnectionHelpCenterPage(event);
        }
        break;
      case kNoiseToggle:
        helium_noise::SetHeliumNoiseEnabledForOrigin(
            web_contents()->GetBrowserContext(), page_url_,
            !state_.rows[kNoiseToggle].checked);
        presenter_->OnHeliumNoiseSettingChanged();
        break;
      case kNoiseSettings:
        ui_delegate_->ShowNoiseSettings();
        break;
      case kCookieToggle:
        presenter_->OnThirdPartyToggleClicked(cookies_state_ ==
                                              CookieControlsState::kAllowed3pc);
        break;
      case kCookieSettings:
        presenter_->OpenCookiesSettingsView();
        break;
      case kRelatedSites:
        presenter_->OpenAllSitesViewFilteredToRws();
        break;
      case kResetPermissions:
        ResetPermissions();
        break;
      case kSiteSettings:
        if (auto* view = BrowserView::GetBrowserViewForBrowser(&*browser_);
            view && view->krypton_ui())
          if (const auto* api = view->krypton_ui()->session().api())
            api->show_site_permissions(api->self,
                                       {origin_.data(), origin_.size()});
        break;
    }
  if (Current())
    Refresh();
}
}  // namespace

std::unique_ptr<ui::MenuModel> CreateSiteInfoMenu(
    BrowserWindowInterface& browser,
    tabs::TabInterface& tab) {
  return std::make_unique<SiteInfoMenu>(browser, tab);
}
bool SiteInfoKeepsOpen(int command) {
  return command == kNoiseToggle || command == kCookieToggle ||
         command == kResetWarnings || command == kResetPermissions ||
         (command >= kPermissionFirst && command < kRangeEnd &&
          !(command < kDeviceFirst && (command % kStride) == 8));
}
bool SiteInfoIsValid(const ui::MenuModel& menu) {
  return static_cast<const SiteInfoMenu&>(menu).valid();
}
bool RequestSiteInfo(content::WebContents* contents) {
  auto* tab =
      contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
  auto* browser = tab ? tab->GetBrowserWindowInterface() : nullptr;
  auto* view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  auto* adapter = view ? view->krypton_ui() : nullptr;
  if (!adapter)
    return false;
  if (browser->GetActiveTabInterface() == tab && adapter->ready()) {
    auto bounds = adapter->location_bar().Bounds();
    if (bounds.IsEmpty()) {
      bounds = views::View::ConvertRectFromScreen(
          view, contents->GetContainerBounds());
      bounds.set_height(0);
    }
    const auto* api = adapter->session().api();
    api->request_context_menu(
        api->self, KR_MENU_SECURITY, 0,
        {static_cast<float>(bounds.x()), static_cast<float>(bounds.y()),
         static_cast<float>(bounds.width()),
         static_cast<float>(bounds.height())});
  }
  return true;
}
std::string_view SiteInfoGlyph(int command) {
  if (command >= kDataFirst)
    return "\uE7F4";
  if (command >= kDeviceFirst)
    return "\uE88E";
  if (command >= kPermissionFirst)
    return "\uE8D7";
  switch (command) {
    case kConnection:
      return "\uE946";
    case kCertificate:
    case kQualifiedCertificate:
      return "\uE8A5";
    case kNoise:
    case kNoiseToggle:
      return "\uEA18";
    case kCookies:
    case kCookieToggle:
    case kStoredData:
      return "\uE7F4";
    case kPermissions:
      return "\uE8D7";
    case kDevices:
      return "\uE88E";
    case kSiteSettings:
    case kNoiseSettings:
    case kCookieSettings:
      return "\uE713";
    case kResetPermissions:
    case kResetWarnings:
      return "\uE777";
    default:
      return {};
  }
}
}  // namespace krypton_ui
