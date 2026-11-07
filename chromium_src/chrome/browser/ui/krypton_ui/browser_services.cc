// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/browser_services.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/version_info/version_info.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/importer/external_process_importer_host.h"
#include "chrome/browser/importer/importer_list.h"
#include "chrome/browser/prefs/session_startup_pref.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_service_model.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/profiles/profile_picker.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/prefs/pref_service.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/base_window.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "url/gurl.h"

namespace krypton_ui {
namespace {
constexpr uint64_t Bit(uint32_t value) {
  return uint64_t{1} << value;
}
KrStr Borrow(std::string_view value) {
  return {value.data(), value.size()};
}
bool Managed(const PrefService& prefs, const char* key) {
  const auto* pref = prefs.FindPreference(key);
  return pref && pref->IsManaged();
}
bool SettingsApi(const KrShellUI* api) {
  return api && KR_HAS_SLOT(api, KrShellUI, settings_operation_result) &&
         api->set_settings_snapshot && api->settings_operation_result;
}
}  // namespace

BrowserServices::BrowserServices(BrowserWindowInterface& browser,
                                 WindowSession& session)
    : browser_(browser), session_(session), profile_(*browser.GetProfile()) {}

BrowserServices::~BrowserServices() {
  weak_factory_.InvalidateWeakPtrs();
  search_observation_.Reset();
  prefs_.RemoveAll();
  local_prefs_.RemoveAll();
  if (folder_)
    folder_->ListenerDestroyed();
  if (importer_) {
    auto* importer = importer_.get();
    importer_ = nullptr;
    importer->set_observer(nullptr);
    importer->Cancel();
  }
  ReleaseFirstRun();
}

bool BrowserServices::Visible() const {
  return onboarding_visible_ || settings_visible_;
}

void BrowserServices::OnboardingVisibility(bool visible) {
  onboarding_visible_ = visible;
  UpdateObservation();
  if (visible) {
    DiscoverSources();
    PublishImport();
  } else {
    importer_list_.reset();
    sources_ready_ = false;
  }
}

void BrowserServices::SettingsVisibility(uint32_t section, bool visible) {
  if (section > KR_SETTINGS_WORKSPACES)
    return;
  if (!visible && settings_section_ != section)
    return;
  settings_section_ = section;
  settings_visible_ = visible;
  UpdateObservation();
}

void BrowserServices::UpdateObservation() {
  const bool observe =
      Visible() && !profile_->IsOffTheRecord() && !profile_->IsGuestSession();
  if (observe != observing_) {
    observing_ = observe;
    if (observe) {
      search_ = TemplateURLServiceFactory::GetForProfile(&*profile_);
      if (search_) {
        search_observation_.Observe(search_.get());
        search_->Load();
      }
      prefs_.Init(profile_->GetPrefs());
      const auto changed = base::BindRepeating(
          &BrowserServices::PublishSettings, base::Unretained(this));
      for (const char* key :
           {prefs::kRestoreOnStartup, prefs::kURLsToRestoreOnStartup,
            prefs::kDownloadDefaultDirectory, prefs::kPromptForDownload,
            prefs::kProfileName})
        prefs_.Add(key, changed);
      local_prefs_.Init(g_browser_process->local_state());
      local_prefs_.Add(prefs::kBrowserAddPersonEnabled, changed);
      local_prefs_.Add(prefs::kDefaultBrowserSettingEnabled,
                       base::BindRepeating(&BrowserServices::QueryDefault,
                                           base::Unretained(this)));
    } else {
      search_observation_.Reset();
      search_ = nullptr;
      prefs_.RemoveAll();
      local_prefs_.RemoveAll();
      ++default_generation_;
      default_check_pending_ = false;
    }
  }
  if (observing_) {
    PublishSearch();
    PublishDefault();
    QueryDefault();
  }
  PublishSettings();
}

void BrowserServices::PublishSettings() {
  const auto* api = session_->api();
  if (!settings_visible_ || !SettingsApi(api))
    return;
  KrSettingsSnapshot value{};
  value.struct_size = sizeof(value);
  value.revision = ++snapshot_revision_;
  value.controls = Bit(KR_SETTINGS_VERSION);
  for (uint32_t action = 0; action <= KR_SETTINGS_ACTION_BROWSER_SETTINGS;
       ++action) {
    if (ActionAllowed(action))
      value.actions |= Bit(action);
  }
  std::string name, location;
  if (observing_) {
    const auto& prefs = *profile_->GetPrefs();
    name = prefs.GetString(prefs::kProfileName);
    auto* downloads = DownloadPrefs::FromBrowserContext(&*profile_);
    location = downloads->DownloadPath().AsUTF8Unsafe();
    value.ask_download_location = downloads->PromptForDownload();
    value.controls |=
        Bit(KR_SETTINGS_PROFILE) | Bit(KR_SETTINGS_DEFAULT_BROWSER) |
        Bit(KR_SETTINGS_DOWNLOAD_LOCATION) | Bit(KR_SETTINGS_ASK_DOWNLOAD);
    const auto startup = SessionStartupPref::GetStartupPref(&prefs);
    // Chromium's combined restore-and-URLs mode has no native equivalent. Its
    // existing editor stays available without presenting an inaccurate value.
    if (startup.type != SessionStartupPref::LAST_AND_URLS) {
      value.controls |= Bit(KR_SETTINGS_STARTUP);
      value.startup = startup.type == SessionStartupPref::LAST   ? 1
                      : startup.type == SessionStartupPref::URLS ? 2
                                                                 : 0;
    }
    if (SessionStartupPref::TypeIsManaged(&prefs))
      value.managed |= Bit(KR_SETTINGS_STARTUP);
    if (downloads->IsDownloadPathManaged())
      value.managed |=
          Bit(KR_SETTINGS_DOWNLOAD_LOCATION) | Bit(KR_SETTINGS_ASK_DOWNLOAD);
    if (Managed(prefs, prefs::kPromptForDownload))
      value.managed |= Bit(KR_SETTINGS_ASK_DOWNLOAD);
    if (DefaultManaged())
      value.managed |= Bit(KR_SETTINGS_DEFAULT_BROWSER);
    if (search_ && search_->loaded() && !engines_.empty()) {
      value.controls |= Bit(KR_SETTINGS_SEARCH_ENGINE);
      if (!search_->CanDefaultSearchProviderBeModifiedByUser())
        value.managed |= Bit(KR_SETTINGS_SEARCH_ENGINE);
    }
  }
  value.profile_name = Borrow(name);
  value.download_location = Borrow(location);
  value.product = Borrow("Krypton");
  value.version = Borrow(version_info::GetHeliumVersionNumber());
  const std::string build =
      "Chromium " + std::string(version_info::GetVersionNumber()) + " · " +
      std::string(version_info::GetOSType()) + " x64 · " +
      std::string(version_info::GetLastChange());
  value.build = Borrow(build);
  api->set_settings_snapshot(api->self, &value);
}

void BrowserServices::Complete(uint64_t request, KrResult result) {
  const auto* api = session_->api();
  if (!SettingsApi(api))
    return;
  const KrSettingsOperationResult value{sizeof(value), request, result};
  api->settings_operation_result(api->self, &value);
}

KrResult BrowserServices::EditSetting(const KrSettingsEdit& edit) {
  if (!settings_visible_ || !observing_ || !edit.request_id ||
      edit.setting > KR_SETTING_ASK_DOWNLOAD ||
      edit.value > (edit.setting == KR_SETTING_STARTUP ? 2u : 1u))
    return KR_ERR_INVALID_ARGUMENT;
  return session_->Defer(base::BindOnce(&BrowserServices::ApplySetting,
                                        weak_factory_.GetWeakPtr(), edit))
             ? KR_OK
             : KR_ERR_UNSUPPORTED;
}

void BrowserServices::ApplySetting(KrSettingsEdit edit) {
  if (!observing_ || !settings_visible_) {
    Complete(edit.request_id, KR_ERR_UNSUPPORTED);
    return;
  }
  auto* prefs = profile_->GetPrefs();
  KrResult result = KR_ERR_INVALID_ARGUMENT;
  if (edit.setting == KR_SETTING_STARTUP &&
      settings_section_ == KR_SETTINGS_GENERAL &&
      !SessionStartupPref::TypeIsManaged(prefs)) {
    auto startup = SessionStartupPref::GetStartupPref(prefs);
    if (startup.type != SessionStartupPref::LAST_AND_URLS) {
      startup.type = edit.value == 1   ? SessionStartupPref::LAST
                     : edit.value == 2 ? SessionStartupPref::URLS
                                       : SessionStartupPref::DEFAULT;
      SessionStartupPref::SetStartupPref(prefs, startup);
      result = KR_OK;
    }
  } else if (edit.setting == KR_SETTING_ASK_DOWNLOAD &&
             settings_section_ == KR_SETTINGS_DOWNLOADS &&
             !Managed(*prefs, prefs::kPromptForDownload) &&
             !DownloadPrefs::FromBrowserContext(&*profile_)
                  ->IsDownloadPathManaged()) {
    prefs->SetBoolean(prefs::kPromptForDownload, edit.value != 0);
    result = KR_OK;
  }
  PublishSettings();  // Authoritative value always precedes completion.
  Complete(edit.request_id, result);
}

bool BrowserServices::ActionAllowed(uint32_t action) const {
  if (action > KR_SETTINGS_ACTION_BROWSER_SETTINGS)
    return false;
  const bool regular =
      !profile_->IsOffTheRecord() && !profile_->IsGuestSession();
  switch (action) {
    case KR_SETTINGS_ACTION_MAKE_DEFAULT:
      return regular && default_known_ && default_can_set_ && !DefaultManaged();
    case KR_SETTINGS_ACTION_CHOOSE_DOWNLOAD_FOLDER:
      return regular && !DownloadPrefs::FromBrowserContext(&*profile_)
                             ->IsDownloadPathManaged();
    case KR_SETTINGS_ACTION_ADD_PROFILE:
      return regular && profiles::IsProfileCreationAllowed();
    case KR_SETTINGS_ACTION_PROFILE_PICKER:
    case KR_SETTINGS_ACTION_MANAGE_PROFILE:
    case KR_SETTINGS_ACTION_IMPORT:
      return regular;
    default:
      return SettingsDestination(action).has_value();
  }
}

KrResult BrowserServices::InvokeAction(uint64_t request, uint32_t action) {
  if (!settings_visible_ || !request || !ActionAllowed(action))
    return KR_ERR_INVALID_ARGUMENT;
  return session_->Defer(base::BindOnce(&BrowserServices::RunAction,
                                        weak_factory_.GetWeakPtr(), request,
                                        action))
             ? KR_OK
             : KR_ERR_UNSUPPORTED;
}

void BrowserServices::RunAction(uint64_t request, uint32_t action) {
  if (!settings_visible_ || !ActionAllowed(action)) {
    PublishSettings();
    Complete(request, KR_ERR_UNSUPPORTED);
    return;
  }
  if (action == KR_SETTINGS_ACTION_CHOOSE_DOWNLOAD_FOLDER) {
    auto* tab = browser_->GetActiveTabInterface();
    if (folder_ || !tab) {
      Complete(request, KR_ERR_UNSUPPORTED);
      return;
    }
    folder_request_ = request;
    folder_ = ui::SelectFileDialog::Create(
        this, std::make_unique<ChromeSelectFilePolicy>(tab->GetContents()));
    ui::SelectFileDialog::FileTypeInfo info;
    info.allowed_paths = ui::SelectFileDialog::FileTypeInfo::NATIVE_PATH;
    folder_->SelectFile(
        ui::SelectFileDialog::SELECT_FOLDER,
        l10n_util::GetStringUTF16(IDS_SETTINGS_DOWNLOAD_LOCATION),
        DownloadPrefs::FromBrowserContext(&*profile_)->DownloadPath(), &info, 0,
        base::FilePath::StringType(), browser_->GetWindow()->GetNativeWindow(),
        nullptr);
    return;
  }
  if (action == KR_SETTINGS_ACTION_ADD_PROFILE ||
      action == KR_SETTINGS_ACTION_PROFILE_PICKER) {
    ProfilePicker::Show(ProfilePicker::Params::FromEntryPoint(
        action == KR_SETTINGS_ACTION_ADD_PROFILE
            ? ProfilePicker::EntryPoint::kProfileMenuAddNewProfile
            : ProfilePicker::EntryPoint::kProfileMenuManageProfiles));
  } else if (const auto url = SettingsDestination(action)) {
    // Deliberately do not call ShowSettings[SubPage]: that function redirects
    // supported paths back into the native window.
    ShowSingletonTab(&*browser_, GURL(*url));
  } else {
    Complete(request, KR_ERR_UNSUPPORTED);
    return;
  }
  Complete(request, KR_OK);
}

void BrowserServices::FileSelected(const ui::SelectedFileInfo& file,
                                   int index) {
  const uint64_t request = std::exchange(folder_request_, 0);
  folder_.reset();
  auto* downloads = DownloadPrefs::FromBrowserContext(&*profile_);
  const bool allowed = !downloads->IsDownloadPathManaged() &&
                       file.path().IsAbsolute() &&
                       !file.path().ReferencesParent();
  if (allowed)
    downloads->SetDownloadPath(file.path());
  PublishSettings();
  Complete(request, allowed ? KR_OK : KR_ERR_INVALID_ARGUMENT);
}

void BrowserServices::FileSelectionCanceled() {
  const uint64_t request = std::exchange(folder_request_, 0);
  folder_.reset();
  // Canceling a folder picker is a successful no-op, not a browser error.
  PublishSettings();
  Complete(request, KR_OK);
}

void BrowserServices::PublishSearch() {
  const auto* api = session_->api();
  if (!observing_ || !search_ || !search_->loaded() || !api ||
      !KR_HAS_SLOT(api, KrShellUI, remove_search_engine) ||
      !api->upsert_search_engine || !api->remove_search_engine)
    return;
  const auto* current = search_->GetDefaultSearchProvider();
  const bool managed = !search_->CanDefaultSearchProviderBeModifiedByUser();
  base::flat_map<KrSearchEngineId, Engine> next;
  uint32_t order = 0;
  for (const auto& stored : search_->GetTemplateURLs()) {
    const auto* engine = stored.get();
    if (engine->id().value() <= 0 ||
        (engine != current && !search_->CanMakeDefault(engine)))
      continue;
    const auto id = static_cast<KrSearchEngineId>(engine->id().value());
    Engine value{base::UTF16ToUTF8(engine->short_name()),
                 base::UTF16ToUTF8(engine->keyword()),
                 (engine == current ? KR_SEARCH_ENGINE_DEFAULT : 0u) |
                     (managed ? KR_SEARCH_ENGINE_MANAGED : 0u),
                 order++};
    if (value.name.empty())
      continue;
    const auto previous = engines_.find(id);
    if (previous == engines_.end() || previous->second != value) {
      const KrSearchEngine wire{sizeof(wire),
                                id,
                                value.flags,
                                value.order,
                                Borrow(value.name),
                                Borrow(value.keyword),
                                0};
      api->upsert_search_engine(api->self, &wire);
    }
    next.emplace(id, std::move(value));
  }
  for (const auto& [id, ignored] : engines_) {
    if (!next.contains(id))
      api->remove_search_engine(api->self, id);
  }
  engines_ = std::move(next);
}

KrResult BrowserServices::ChooseSearch(KrSearchEngineId id) {
  if (!observing_ || !search_ || !search_->loaded() || !engines_.contains(id) ||
      !search_->CanDefaultSearchProviderBeModifiedByUser())
    return KR_ERR_INVALID_ARGUMENT;
  return session_->Defer(base::BindOnce(&BrowserServices::ApplySearch,
                                        weak_factory_.GetWeakPtr(), id))
             ? KR_OK
             : KR_ERR_UNSUPPORTED;
}

void BrowserServices::ApplySearch(KrSearchEngineId id) {
  auto* search = TemplateURLServiceFactory::GetForProfile(&*profile_);
  if (!search || !search->loaded())
    return;
  for (const auto& stored : search->GetTemplateURLs()) {
    auto* engine = stored.get();
    if (engine->id().value() > 0 &&
        static_cast<KrSearchEngineId>(engine->id().value()) == id &&
        search->CanMakeDefault(engine)) {
      search->SetUserSelectedDefaultSearchProvider(engine);
      break;
    }
  }
  PublishSearch();
  PublishSettings();
}

void BrowserServices::OnTemplateURLServiceChanged() {
  PublishSearch();
  PublishSettings();
}

void BrowserServices::OnTemplateURLServiceShuttingDown() {
  search_observation_.Reset();
  search_ = nullptr;
  PublishSettings();
}

bool BrowserServices::DefaultManaged() const {
  const auto* pref = g_browser_process->local_state()->FindPreference(
      prefs::kDefaultBrowserSettingEnabled);
  return pref && pref->IsManaged() && !pref->GetValue()->GetBool();
}

void BrowserServices::QueryDefault() {
  if (!observing_ || default_check_pending_)
    return;
  default_check_pending_ = true;
  const auto generation = ++default_generation_;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce([] {
        return std::pair{
            static_cast<int>(shell_integration::GetDefaultBrowser()),
            shell_integration::CanSetAsDefaultBrowser()};
      }),
      base::BindOnce(
          [](base::WeakPtr<BrowserServices> owner, uint64_t generation,
             std::pair<int, bool> result) {
            if (owner)
              owner->OnDefaultChecked(generation, result.first, result.second);
          },
          weak_factory_.GetWeakPtr(), generation));
}

void BrowserServices::OnDefaultChecked(uint64_t generation,
                                       int state,
                                       bool can_set) {
  if (generation != default_generation_ || !observing_)
    return;
  default_check_pending_ = false;
  default_state_ = state;
  default_can_set_ = can_set;
  default_known_ = true;
  PublishDefault();
  PublishSettings();
}

void BrowserServices::PublishDefault() {
  const auto* api = session_->api();
  if (!Visible() || !default_known_ || !api ||
      !KR_HAS_SLOT(api, KrShellUI, set_default_browser_state) ||
      !api->set_default_browser_state)
    return;
  const KrDefaultBrowserState state{
      sizeof(state), (default_state_ == shell_integration::IS_DEFAULT
                          ? KR_DEFAULT_BROWSER_IS_DEFAULT
                          : 0u) |
                         (default_can_set_ ? KR_DEFAULT_BROWSER_CAN_SET : 0u) |
                         (DefaultManaged() ? KR_DEFAULT_BROWSER_MANAGED : 0u) |
                         (default_pending_ ? KR_DEFAULT_BROWSER_PENDING : 0u)};
  api->set_default_browser_state(api->self, &state);
}

KrResult BrowserServices::RequestDefault() {
  if (!observing_ || !default_known_ || !default_can_set_ || DefaultManaged() ||
      default_pending_)
    return KR_ERR_UNSUPPORTED;
  default_pending_ = true;
  if (!session_->Defer(base::BindOnce(&BrowserServices::SetDefault,
                                      weak_factory_.GetWeakPtr()))) {
    default_pending_ = false;
    return KR_ERR_UNSUPPORTED;
  }
  PublishDefault();
  return KR_OK;
}

void BrowserServices::SetDefault() {
  if (!Visible() || DefaultManaged()) {
    default_pending_ = false;
    PublishDefault();
    return;
  }
  ++default_generation_;
  default_check_pending_ = false;
  default_worker_ =
      base::MakeRefCounted<shell_integration::DefaultBrowserWorker>();
  default_worker_->StartSetAsDefault(base::BindOnce(
      &BrowserServices::OnDefaultSet, weak_factory_.GetWeakPtr()));
}

void BrowserServices::OnDefaultSet(
    shell_integration::DefaultWebClientState state) {
  default_worker_.reset();
  default_pending_ = false;
  default_state_ = state;
  default_known_ = true;
  PublishDefault();
  PublishSettings();
}

void BrowserServices::OnWindowState(uint32_t flags) {
  const bool active = (flags & KR_WINDOW_ACTIVE) != 0;
  if (active && !window_active_ && observing_)
    QueryDefault();  // OS default-apps changes are checked on activation, never
                     // polled.
  window_active_ = active;
}

}  // namespace krypton_ui
