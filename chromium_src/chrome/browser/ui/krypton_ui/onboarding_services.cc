// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_services.h"

#include <algorithm>
#include <bit>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/importer/brave_external_process_importer_host.h"
#include "chrome/browser/importer/importer_list.h"
#include "chrome/browser/importer/profile_writer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_service_model.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/webui_url_constants.h"
#include "components/helium_services/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/base_window.h"
#include "url/gurl.h"

namespace krypton_ui {
namespace {
const char kFirstRunOwnerKey = 0;
class FirstRunOwner : public base::SupportsUserData::Data {};
KrStr Borrow(std::string_view value) {
  return {value.data(), value.size()};
}
bool SameSource(const user_data_importer::SourceProfile& a,
                const user_data_importer::SourceProfile& b) {
  return a.importer_type == b.importer_type && a.source_path == b.source_path &&
         a.profile == b.profile;
}
}  // namespace

void BrowserServices::MaybeShowFirstRun() {
  if (profile_->IsOffTheRecord() || profile_->IsGuestSession() ||
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kNoFirstRun) ||
      profile_->GetPrefs()->GetBoolean(prefs::kHeliumDidOnboarding) ||
      profile_->GetUserData(&kFirstRunOwnerKey))
    return;
  if (!ShowOnboarding(true))
    ShowSingletonTab(&*browser_, GURL(chrome::kHeliumSetupURL));
}

bool BrowserServices::ShowOnboarding(bool first_run) {
  const auto* api = session_->api();
  if (profile_->IsOffTheRecord() || profile_->IsGuestSession() || !api ||
      !KR_HAS_SLOT(api, KrShellUI, remove_search_engine) ||
      !api->show_onboarding || !api->upsert_import_source ||
      !api->remove_import_source || !api->set_import_progress ||
      !api->set_default_browser_state || !api->upsert_search_engine ||
      !api->remove_search_engine)
    return false;
  if (profile_->GetUserData(&kFirstRunOwnerKey) && !owns_first_run_)
    return false;
  if (!owns_first_run_) {
    profile_->SetUserData(&kFirstRunOwnerKey,
                          std::make_unique<FirstRunOwner>());
    owns_first_run_ = true;
  }
  const KrOnboardingRequest request{sizeof(request),
                                    first_run ? KR_ONBOARDING_FIRST_RUN : 0u,
                                    KR_ONBOARDING_STEP_IGNITION};
  if (api->show_onboarding(api->self, &request) == KR_OK)
    return true;
  ReleaseFirstRun();
  return false;
}

void BrowserServices::ReleaseFirstRun() {
  if (std::exchange(owns_first_run_, false))
    profile_->RemoveUserData(&kFirstRunOwnerKey);
}

void BrowserServices::OnboardingFinished(KrOnboardingResult result) {
  if (OnboardingOutcomeCompletesFirstRun(result.outcome) &&
      !profile_->IsOffTheRecord() && !profile_->IsGuestSession())
    profile_->GetPrefs()->SetBoolean(prefs::kHeliumDidOnboarding, true);
  ReleaseFirstRun();
}

void BrowserServices::DiscoverSources() {
  if (!onboarding_visible_ || profile_->IsOffTheRecord() ||
      profile_->IsGuestSession() || importer_list_)
    return;
  sources_ready_ = false;
  importer_list_ = std::make_unique<ImporterList>();
  importer_list_->DetectSourceProfiles(
      g_browser_process->GetApplicationLocale(), true,
      base::BindOnce(&BrowserServices::SourcesLoaded,
                     weak_factory_.GetWeakPtr()));
}

void BrowserServices::SourcesLoaded() {
  const auto* api = session_->api();
  if (!onboarding_visible_ || !importer_list_ || !api ||
      !KR_HAS_SLOT(api, KrShellUI, remove_import_source) ||
      !api->upsert_import_source || !api->remove_import_source)
    return;
  std::vector<Source> next;
  for (size_t index = 0; index < importer_list_->count(); ++index) {
    const auto& source = importer_list_->GetSourceProfileAt(index);
    if (!NativeImportItems(source.services_supported))
      continue;
    const auto previous = std::ranges::find_if(
        sources_,
        [&](const auto& value) { return SameSource(value.profile, source); });
    next.push_back(
        {previous == sources_.end() ? next_source_++ : previous->id, source});
  }
  // Keep the running import's identity even if the source browser disappears.
  if (importer_ || import_queued_) {
    const auto active =
        std::ranges::find(sources_, import_.source, &Source::id);
    if (active != sources_.end() &&
        std::ranges::find(next, active->id, &Source::id) == next.end())
      next.push_back(*active);
  }
  uint32_t order = 0;
  for (const auto& source : next) {
    auto name = base::UTF16ToUTF8(source.profile.importer_name);
    const auto profile = base::UTF16ToUTF8(source.profile.profile);
    if (!profile.empty())
      name += " · " + profile;
    const KrImportSource value{
        sizeof(value),
        source.id,
        NativeImportItems(source.profile.services_supported),
        order++,
        Borrow(name),
        0};
    api->upsert_import_source(api->self, &value);
  }
  for (const auto& source : sources_) {
    if (std::ranges::find(next, source.id, &Source::id) == next.end())
      api->remove_import_source(api->self, source.id);
  }
  sources_ = std::move(next);
  sources_ready_ = true;
  PublishImport();
}

KrResult BrowserServices::StartImport(KrImportSourceId id, uint32_t items) {
  if (!onboarding_visible_ || !sources_ready_ || importer_ || import_queued_ ||
      !items)
    return KR_ERR_UNSUPPORTED;
  const auto source = std::ranges::find(sources_, id, &Source::id);
  if (source == sources_.end() ||
      (items & ~NativeImportItems(source->profile.services_supported)))
    return KR_ERR_INVALID_ARGUMENT;
  import_queued_ = true;
  import_requested_ = items;
  import_completed_ = 0;
  import_ = {sizeof(import_), id, KR_IMPORT_RUNNING, 0};
  if (!session_->Defer(base::BindOnce(&BrowserServices::RunImport,
                                      weak_factory_.GetWeakPtr(), id, items))) {
    import_queued_ = false;
    return KR_ERR_UNSUPPORTED;
  }
  PublishImport();
  return KR_OK;
}

void BrowserServices::RunImport(KrImportSourceId id, uint32_t items) {
  import_queued_ = false;
  const auto source = std::ranges::find(sources_, id, &Source::id);
  if (!onboarding_visible_ || source == sources_.end()) {
    import_.state = KR_IMPORT_FAILED;
    PublishImport();
    return;
  }
  // Copy before StartImportSettings: lock cancellation can finish
  // synchronously.
  const auto profile = source->profile;
  importer_ = new BraveExternalProcessImporterHost();
  importer_->set_observer(this);
  importer_->set_parent_window(browser_->GetWindow()->GetNativeWindow());
  if (auto* contents = browser_->GetTabStripModel()->GetActiveWebContents())
    importer_->set_parent_view(contents->GetNativeView());
  importer_->StartImportSettings(profile, &*profile_, BrowserImportItems(items),
                                 new ProfileWriter(&*profile_));
}

void BrowserServices::PublishImport() {
  const auto* api = session_->api();
  if (onboarding_visible_ && api &&
      KR_HAS_SLOT(api, KrShellUI, set_import_progress) &&
      api->set_import_progress)
    api->set_import_progress(api->self, &import_);
}

void BrowserServices::ImportStarted() {
  import_.state = KR_IMPORT_RUNNING;
  PublishImport();
}

void BrowserServices::ImportItemStarted(user_data_importer::ImportItem item) {}

void BrowserServices::ImportItemEnded(user_data_importer::ImportItem item) {
  import_completed_ |= NativeImportItems(item) & import_requested_;
  import_.fraction = static_cast<float>(std::popcount(import_completed_)) /
                     static_cast<float>(std::popcount(import_requested_));
  PublishImport();
}

void BrowserServices::ImportEnded() {
  importer_->set_observer(nullptr);
  importer_ = nullptr;
  import_.state = import_completed_ == import_requested_ ? KR_IMPORT_SUCCEEDED
                                                         : KR_IMPORT_FAILED;
  if (import_.state == KR_IMPORT_SUCCEEDED)
    import_.fraction = 1;
  PublishImport();
}

}  // namespace krypton_ui
