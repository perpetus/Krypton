// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"

#include "base/functional/bind.h"
#include "chrome/browser/ui/krypton_ui/browser_services.h"
#include "chrome/browser/ui/krypton_ui/site_permissions.h"

namespace krypton_ui {

bool BrowserAdapter::ShowOnboarding() {
  return browser_services_ && browser_services_->ShowOnboarding(false);
}

void BrowserAdapter::BindBrowserServices(KrBrowser& host) {
  site_permissions_ = std::make_unique<SitePermissions>(*browser_, session_);
  host.site_permissions_visibility_changed = [](void* self, KrStr origin,
                                                bool visible) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    if (owner.site_permissions_)
      owner.site_permissions_->Visibility(
          origin.data ? std::string(origin.data, origin.size) : std::string{},
          visible);
  };
  host.set_site_permission = [](void* self, KrPermissionId id,
                                uint32_t setting) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    return owner.site_permissions_
               ? owner.site_permissions_->SetPermission(id, setting)
               : KR_ERR_UNSUPPORTED;
  };
  host.reset_site_permissions = [](void* self, KrStr origin) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    return owner.site_permissions_
               ? owner.site_permissions_->Reset(
                     origin.data ? std::string(origin.data, origin.size)
                                 : std::string{})
               : KR_ERR_UNSUPPORTED;
  };
  host.permission_prompt_response = [](void* self, uint64_t request,
                                       uint32_t decision) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    if (owner.site_permissions_)
      owner.site_permissions_->Respond(request, decision);
  };
  browser_services_ = std::make_unique<BrowserServices>(*browser_, session_);
  host.onboarding_visibility_changed = [](void* self, bool visible) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    if (owner.browser_services_)
      std::ignore = owner.session_.Defer(base::BindOnce(
          &BrowserServices::OnboardingVisibility,
          owner.browser_services_->weak_factory_.GetWeakPtr(), visible));
  };
  host.settings_visibility_changed = [](void* self, uint32_t section,
                                        bool visible) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    if (owner.browser_services_)
      std::ignore = owner.session_.Defer(
          base::BindOnce(&BrowserServices::SettingsVisibility,
                         owner.browser_services_->weak_factory_.GetWeakPtr(),
                         section, visible));
  };
  host.start_import = [](void* self, KrImportSourceId source,
                         uint32_t items) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    return owner.browser_services_
               ? owner.browser_services_->StartImport(source, items)
               : KR_ERR_UNSUPPORTED;
  };
  host.request_default_browser = [](void* self) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    return owner.browser_services_ ? owner.browser_services_->RequestDefault()
                                   : KR_ERR_UNSUPPORTED;
  };
  host.set_default_search_engine = [](void* self,
                                      KrSearchEngineId engine) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    return owner.browser_services_
               ? owner.browser_services_->ChooseSearch(engine)
               : KR_ERR_UNSUPPORTED;
  };
  host.onboarding_finished = [](void* self,
                                const KrOnboardingResult* result) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    if (!owner.browser_services_ ||
        !KR_HAS_FIELD(result, KrOnboardingResult, last_step) ||
        result->outcome > KR_ONBOARDING_CLOSED ||
        result->last_step > KR_ONBOARDING_STEP_READY)
      return;
    const KrOnboardingResult copy{sizeof(copy), result->outcome,
                                  result->last_step};
    std::ignore = owner.session_.Defer(base::BindOnce(
        &BrowserServices::OnboardingFinished,
        owner.browser_services_->weak_factory_.GetWeakPtr(), copy));
  };
  host.edit_setting = [](void* self, const KrSettingsEdit* edit) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    if (!owner.browser_services_ || !KR_HAS_FIELD(edit, KrSettingsEdit, value))
      return KR_ERR_INVALID_ARGUMENT;
    const KrSettingsEdit copy{sizeof(copy), edit->request_id, edit->setting,
                              edit->value};
    return owner.browser_services_->EditSetting(copy);
  };
  host.invoke_settings_action = [](void* self, uint64_t request,
                                   uint32_t action) noexcept {
    auto& owner = *static_cast<BrowserAdapter*>(self);
    return owner.browser_services_
               ? owner.browser_services_->InvokeAction(request, action)
               : KR_ERR_UNSUPPORTED;
  };
}

}  // namespace krypton_ui
