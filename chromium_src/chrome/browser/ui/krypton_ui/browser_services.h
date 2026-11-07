// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#ifndef CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_SERVICES_H_
#define CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_SERVICES_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/importer/importer_progress_observer.h"
#include "chrome/browser/shell_integration.h"
#include "chrome/browser/ui/krypton_ui/include/krypton_ui/abi.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/search_engines/template_url_service_observer.h"
#include "components/user_data_importer/common/importer_data_types.h"
#include "ui/shell_dialogs/select_file_dialog.h"

class BrowserWindowInterface;
class ExternalProcessImporterHost;
class ImporterList;
class Profile;
class TemplateURLService;
namespace krypton_ui {
class WindowSession;

// Per-window services behind the optional onboarding/settings ABI. Browser
// mutations are deferred through WindowSession; observation is visibility
// scoped.
class BrowserServices : public TemplateURLServiceObserver,
                        public importer::ImporterProgressObserver,
                        public ui::SelectFileDialog::Listener {
 public:
  BrowserServices(BrowserWindowInterface& browser, WindowSession& session);
  ~BrowserServices() override;
  BrowserServices(const BrowserServices&) = delete;
  BrowserServices& operator=(const BrowserServices&) = delete;

  void MaybeShowFirstRun();
  bool ShowOnboarding(bool first_run);
  void OnWindowState(uint32_t flags);

 private:
  friend class BrowserAdapter;
  struct Engine {
    std::string name, keyword;
    uint32_t flags = 0, order = 0;
    bool operator==(const Engine&) const = default;
  };
  struct Source {
    KrImportSourceId id;
    user_data_importer::SourceProfile profile;
  };

  void OnboardingVisibility(bool visible);
  void SettingsVisibility(uint32_t section, bool visible);
  void UpdateObservation();
  bool Visible() const;
  void PublishSettings();
  void PublishSearch();
  void Complete(uint64_t request, KrResult result);
  KrResult EditSetting(const KrSettingsEdit& edit);
  void ApplySetting(KrSettingsEdit edit);
  KrResult InvokeAction(uint64_t request, uint32_t action);
  void RunAction(uint64_t request, uint32_t action);
  bool ActionAllowed(uint32_t action) const;
  KrResult ChooseSearch(KrSearchEngineId id);
  void ApplySearch(KrSearchEngineId id);
  void QueryDefault();
  void OnDefaultChecked(uint64_t generation, int state, bool can_set);
  KrResult RequestDefault();
  void SetDefault();
  void OnDefaultSet(shell_integration::DefaultWebClientState state);
  void PublishDefault();
  bool DefaultManaged() const;
  void DiscoverSources();
  void SourcesLoaded();
  KrResult StartImport(KrImportSourceId id, uint32_t items);
  void RunImport(KrImportSourceId id, uint32_t items);
  void PublishImport();
  void OnboardingFinished(KrOnboardingResult result);
  void ReleaseFirstRun();
  void OnTemplateURLServiceChanged() override;
  void OnTemplateURLServiceShuttingDown() override;
  void ImportStarted() override;
  void ImportItemStarted(user_data_importer::ImportItem item) override;
  void ImportItemEnded(user_data_importer::ImportItem item) override;
  void ImportEnded() override;
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void FileSelectionCanceled() override;

  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<WindowSession> session_;
  const raw_ref<Profile> profile_;
  raw_ptr<TemplateURLService> search_ = nullptr;
  base::ScopedObservation<TemplateURLService, TemplateURLServiceObserver>
      search_observation_{this};
  PrefChangeRegistrar prefs_, local_prefs_;
  base::flat_map<KrSearchEngineId, Engine> engines_;
  std::unique_ptr<ImporterList> importer_list_;
  std::vector<Source> sources_;
  // ExternalProcessImporterHost deletes itself after ImportEnded().
  raw_ptr<ExternalProcessImporterHost> importer_ = nullptr;
  scoped_refptr<shell_integration::DefaultBrowserWorker> default_worker_;
  scoped_refptr<ui::SelectFileDialog> folder_;
  KrImportProgress import_{sizeof(KrImportProgress), 0, KR_IMPORT_IDLE, 0};
  uint64_t snapshot_revision_ = 0, default_generation_ = 0, folder_request_ = 0;
  KrImportSourceId next_source_ = 1;
  uint32_t import_requested_ = 0, import_completed_ = 0;
  uint32_t settings_section_ = KR_SETTINGS_GENERAL;
  int default_state_ = 2;  // shell_integration::UNKNOWN_DEFAULT
  bool onboarding_visible_ = false, settings_visible_ = false;
  bool observing_ = false, window_active_ = false, owns_first_run_ = false;
  bool default_known_ = false, default_can_set_ = false;
  bool default_pending_ = false, default_check_pending_ = false;
  bool sources_ready_ = false, import_queued_ = false;
  base::WeakPtrFactory<BrowserServices> weak_factory_{this};
};
}  // namespace krypton_ui
#endif  // CHROME_BROWSER_UI_KRYPTON_UI_BROWSER_SERVICES_H_
