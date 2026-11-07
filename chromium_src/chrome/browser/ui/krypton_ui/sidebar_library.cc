#include "chrome/browser/ui/krypton_ui/sidebar_library.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/cancelable_task_tracker.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/layout_prefs.h"
#include "chrome/browser/ui/krypton_ui/sidebar_downloads.h"
#include "chrome/browser/ui/krypton_ui/site_customization.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/grit/generated_resources.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_service_observer.h"
#include "components/history/core/browser/history_types.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/menus/simple_menu_model.h"

namespace krypton_ui {
namespace {
KrStr Borrow(const std::string& text) {
  return {text.data(), text.size()};
}
std::string Day(base::Time time) {
  return time.is_null() ? std::string{}
                        : base::UTF16ToUTF8(base::TimeFormatShortDate(time));
}
}  // namespace
class SidebarLibrary::Impl final : public history::HistoryServiceObserver {
 public:
  Impl(BrowserWindowInterface& browser,
       WindowSession& session,
       SidebarDownloads& downloads)
      : browser_(browser), session_(session), downloads_(downloads) {}
  ~Impl() override = default;
  void Initialize() {
    const auto* api = Api();
    if (!api) {
      return;
    }
    const bool private_window = browser_->GetProfile()->IsOffTheRecord();
    prefs_.Init(browser_->GetProfile()->GetPrefs());
    prefs_.Add(kLibraryMediaEnabledPref,
               base::BindRepeating(&Impl::MediaChoiceChanged,
                                   weak_factory_.GetWeakPtr()));
    SetSection(KR_LIBRARY_MEDIA,
               MediaEnabled() ? KR_LIBRARY_SECTION_ENABLED : 0);
    SetSection(KR_LIBRARY_DOWNLOADS, KR_LIBRARY_SECTION_ENABLED);
    SetSection(KR_LIBRARY_BOOSTS, KR_LIBRARY_SECTION_ENABLED);
    SetSection(KR_LIBRARY_ROUTING, private_window ? KR_LIBRARY_SECTION_HIDDEN
                                                  : KR_LIBRARY_SECTION_ENABLED);
    SetSection(KR_LIBRARY_SPACES, private_window ? KR_LIBRARY_SECTION_HIDDEN
                                                 : KR_LIBRARY_SECTION_ENABLED);
    SetSection(KR_LIBRARY_HISTORY, private_window ? KR_LIBRARY_SECTION_HIDDEN
                                                  : KR_LIBRARY_SECTION_ENABLED);
    downloads_->SetLibraryChanged(base::BindRepeating(
        &Impl::DownloadsChanged, weak_factory_.GetWeakPtr()));
  }
  void Visibility(uint32_t section, bool visible) {
    if (visible &&
        (section == KR_LIBRARY_DOWNLOADS || section == KR_LIBRARY_MEDIA)) {
      browser_->GetProfile()
          ->GetDownloadManager()
          ->CheckForHistoryFilesRemoval();
    }

    if (section > KR_LIBRARY_HISTORY || !Api()) {
      return;
    }
    visible_[section] = visible;
    if (section == KR_LIBRARY_MEDIA && visible && MediaEnabled()) {
      RefreshMedia();
    }
    if (section != KR_LIBRARY_HISTORY) {
      return;
    }
    history_tasks_.TryCancelAll();
    history_observation_.Reset();
    if (!visible || browser_->GetProfile()->IsOffTheRecord()) {
      return;
    }
    auto* service = History();
    if (!service) {
      SetSection(section, KR_LIBRARY_SECTION_HIDDEN);
      return;
    }
    history_observation_.Observe(service);
    QueryHistory();
  }
  void Command(uint32_t section, uint64_t id, uint32_t command) {
    if (section == KR_LIBRARY_BOOSTS || section == KR_LIBRARY_ROUTING) {
      if (auto* features = Customization()) {
        features->LibraryCommand(section, id, command);
      }
      return;
    }

    if (section > KR_LIBRARY_HISTORY || !Api()) {
      return;
    }
    if (section == KR_LIBRARY_MEDIA && command == KR_LIBRARY_ENABLE &&
        id == 0) {
      browser_->GetProfile()->GetPrefs()->SetBoolean(kLibraryMediaEnabledPref,
                                                     true);
      MediaChoiceChanged();
      return;
    }
    auto* entry = Find(id);
    // Native menu execution is deferred until after popup dismissal. Visibility
    // controls data subscriptions, not whether that captured command may run.
    if (!entry || entry->section != section ||
        (section == KR_LIBRARY_MEDIA && !MediaEnabled())) {
      return;
    }
    if (command == KR_LIBRARY_OPEN) {
      if (section == KR_LIBRARY_MEDIA) {
        downloads_->Command(entry->download, KR_DOWNLOAD_OPEN);
        return;
      }
      NavigateParams params(browser_->GetProfile(), entry->url,
                            ui::PAGE_TRANSITION_LINK);
      params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
      Navigate(&params);
    } else if (command == KR_LIBRARY_REMOVE && section == KR_LIBRARY_HISTORY) {
      if (auto* history = History()) {
        history->DeleteURLs({entry->url});
      }
    }
  }
  std::unique_ptr<ui::MenuModel> CreateMenu(uint64_t id) {
    if ((id >> 60) == 0xc) {
      if (auto* features = Customization()) {
        return features->RoutingMenu(id);
      }
    }
    if ((id >> 60) == 0xb) {
      if (auto* features = Customization()) {
        return features->BoostMenu(id);
      }
    }

    auto* entry = Find(id);
    if (!entry) {
      return nullptr;
    }
    if (entry->section == KR_LIBRARY_MEDIA) {
      return downloads_->CreateMenu(entry->download);
    }
    if (entry->section == KR_LIBRARY_HISTORY) {
      return std::make_unique<HistoryMenu>(weak_factory_.GetWeakPtr(), id);
    }
    return nullptr;
  }

 private:
  SiteCustomization* Customization() {
    auto* view = BrowserView::GetBrowserViewForBrowser(&*browser_);
    return view && view->krypton_ui() ? view->krypton_ui()->site_customization()
                                      : nullptr;
  }

  struct Entry {
    uint64_t id = 0, download = 0;
    uint32_t section = 0, flags = 0;
    GURL url;
    base::Time time;
    std::string key, title, subtitle, group;
    bool SameData(const Entry& other) const {
      return download == other.download && section == other.section &&
             flags == other.flags && url == other.url && time == other.time &&
             title == other.title && subtitle == other.subtitle &&
             group == other.group;
    }
  };
  class HistoryMenu final : public ui::SimpleMenuModel,
                            public ui::SimpleMenuModel::Delegate {
   public:
    HistoryMenu(base::WeakPtr<Impl> owner, uint64_t item)
        : ui::SimpleMenuModel(this), owner_(owner), item_(item) {
      AddItem(kOpen, l10n_util::GetStringUTF16(IDS_HISTORY_OPEN));
      AddItem(kRemove, l10n_util::GetStringUTF16(IDS_HISTORY_DELETE));
    }
    bool IsCommandIdEnabled(int) const override {
      return owner_ && owner_->Find(item_);
    }
    void ExecuteCommand(int command, int) override {
      if (owner_ && (command == kOpen || command == kRemove)) {
        owner_->Command(KR_LIBRARY_HISTORY, item_,
                        command == kOpen ? KR_LIBRARY_OPEN : KR_LIBRARY_REMOVE);
      }
    }

   private:
    static constexpr int kOpen = 1, kRemove = 2;
    base::WeakPtr<Impl> owner_;
    uint64_t item_;
  };
  const KrShellUI* Api() const {
    const auto* api = session_->api();
    return api && KR_HAS_SLOT(api, KrShellUI, upsert_library_item) &&
                   api->upsert_library_item &&
                   KR_HAS_SLOT(api, KrShellUI, remove_library_item) &&
                   api->remove_library_item &&
                   KR_HAS_SLOT(api, KrShellUI, set_library_section) &&
                   api->set_library_section
               ? api
               : nullptr;
  }
  bool MediaEnabled() const {
    return browser_->GetProfile()->GetPrefs()->GetBoolean(
        kLibraryMediaEnabledPref);
  }
  history::HistoryService* History() const {
    return browser_->GetProfile()->IsOffTheRecord()
               ? nullptr
               : HistoryServiceFactory::GetForProfile(
                     browser_->GetProfile(),
                     ServiceAccessType::EXPLICIT_ACCESS);
  }
  void SetSection(uint32_t section, uint32_t flags) {
    if (const auto* api = Api()) {
      const KrLibrarySectionState state{sizeof(state), section, flags};
      api->set_library_section(api->self, &state);
    }
  }
  void MediaChoiceChanged() {
    SetSection(KR_LIBRARY_MEDIA,
               MediaEnabled() ? KR_LIBRARY_SECTION_ENABLED : 0);
    if (visible_[KR_LIBRARY_MEDIA] && MediaEnabled()) {
      RefreshMedia();
    }
  }
  void DownloadsChanged() {
    if (visible_[KR_LIBRARY_MEDIA] && MediaEnabled()) {
      RefreshMedia();
    }
  }
  void RefreshMedia() {
    std::vector<Entry> entries;
    for (const auto& [id, item] : downloads_->items()) {
      const auto mime = item->GetMimeType();
      if (item->GetState() != download::DownloadItem::COMPLETE ||
          item->GetFileExternallyRemoved() ||
          !(mime.starts_with("image/") || mime.starts_with("video/"))) {
        continue;
      }
      const auto time = item->GetEndTime().is_null() ? item->GetStartTime()
                                                     : item->GetEndTime();
      Entry entry;
      entry.download = id;
      entry.section = KR_LIBRARY_MEDIA;
      entry.flags = KR_LIBRARY_ITEM_FILE |
                    (mime.starts_with("video/") ? KR_LIBRARY_ITEM_VIDEO : 0);
      entry.key = "media:" + base::NumberToString(id);
      entry.title = item->GetFileNameToReportUser().AsUTF8Unsafe();
      entry.subtitle = item->GetOriginalUrl().host();
      entry.group = Day(time);
      entry.time = time;
      entries.push_back(std::move(entry));
    }
    Reconcile(KR_LIBRARY_MEDIA, std::move(entries));
  }
  void QueryHistory() {
    if (!visible_[KR_LIBRARY_HISTORY] || history_scheduled_) {
      return;
    }
    history_scheduled_ = true;
    std::ignore = session_->Defer(
        base::BindOnce(&Impl::StartHistoryQuery, weak_factory_.GetWeakPtr()));
  }
  void StartHistoryQuery() {
    history_scheduled_ = false;
    if (!visible_[KR_LIBRARY_HISTORY]) {
      return;
    }
    auto* service = History();
    if (!service) {
      return;
    }
    history_tasks_.TryCancelAll();
    SetSection(KR_LIBRARY_HISTORY,
               KR_LIBRARY_SECTION_ENABLED | KR_LIBRARY_SECTION_LOADING);
    history::QueryOptions options;
    options.max_count = 200;
    options.duplicate_policy = history::QueryOptions::REMOVE_ALL_DUPLICATES;
    service->QueryHistory(
        {}, options,
        base::BindOnce(&Impl::HistoryResult, weak_factory_.GetWeakPtr()),
        &history_tasks_);
  }
  void HistoryResult(history::QueryResults result) {
    if (!visible_[KR_LIBRARY_HISTORY]) {
      return;
    }
    std::vector<Entry> entries;
    entries.reserve(result.size());
    for (const auto& row : result) {
      Entry entry;
      entry.section = KR_LIBRARY_HISTORY;
      entry.url = row.url();
      entry.time = row.visit_time();
      // A new latest visit is a new item: remove/insert keeps chronological
      // grouping correct without rebuilding the native list or extending ABI.
      entry.key = "history:" + row.url().spec() + "\n" +
                  base::NumberToString(row.visit_time().ToInternalValue());
      entry.title = row.title().empty() ? row.url().spec()
                                        : base::UTF16ToUTF8(row.title());
      entry.subtitle = row.url().spec();
      entry.group = Day(row.visit_time());
      entries.push_back(std::move(entry));
    }
    Reconcile(KR_LIBRARY_HISTORY, std::move(entries));
    SetSection(KR_LIBRARY_HISTORY, KR_LIBRARY_SECTION_ENABLED);
  }
  void Reconcile(uint32_t section, std::vector<Entry> next) {
    const auto* api = Api();
    if (!api || !visible_[section]) {
      return;
    }
    base::flat_set<std::string> live;
    for (const auto& entry : next) {
      live.insert(entry.key);
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.section == section && !live.contains(it->first)) {
        api->remove_library_item(api->self, it->second.id);
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    std::ranges::sort(next, [section](const auto& a, const auto& b) {
      return section == KR_LIBRARY_HISTORY ? a.time > b.time : a.time < b.time;
    });
    KrLibraryItemId before = 0;
    for (auto it = next.rbegin(); it != next.rend(); ++it) {
      auto& entry = *it;
      const auto existing = entries_.find(entry.key);
      if (existing != entries_.end() && existing->second.SameData(entry)) {
        before = existing->second.id;
        continue;
      }
      entry.id = existing != entries_.end() ? existing->second.id : next_id_++;
      const auto key = entry.key;
      auto& stored = entries_[key];
      stored = std::move(entry);
      const KrLibraryItem value{sizeof(KrLibraryItem), stored.id,
                                stored.section,        stored.flags,
                                Borrow(stored.title),  Borrow(stored.subtitle),
                                Borrow(stored.group),  0};
      api->upsert_library_item(api->self, &value, before);
      before = stored.id;
    }
  }
  Entry* Find(uint64_t id) {
    for (auto& [key, entry] : entries_) {
      if (entry.id == id) {
        return &entry;
      }
    }
    return nullptr;
  }
  void OnURLVisited(history::HistoryService*,
                    const history::VisitedURLInfo&) override {
    QueryHistory();
  }
  void OnURLsModified(history::HistoryService*,
                      const history::URLRows&) override {
    QueryHistory();
  }
  void OnHistoryDeletions(history::HistoryService*,
                          const history::DeletionInfo&) override {
    QueryHistory();
  }
  void HistoryServiceBeingDeleted(history::HistoryService*) override {
    history_tasks_.TryCancelAll();
    history_observation_.Reset();
  }
  const raw_ref<BrowserWindowInterface> browser_;
  const raw_ref<WindowSession> session_;
  const raw_ref<SidebarDownloads> downloads_;
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<history::HistoryService,
                          history::HistoryServiceObserver>
      history_observation_{this};
  base::CancelableTaskTracker history_tasks_;
  std::array<bool, KR_LIBRARY_HISTORY + 1> visible_{};
  base::flat_map<std::string, Entry> entries_;
  uint64_t next_id_ = 1;
  bool history_scheduled_ = false;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};
SidebarLibrary::SidebarLibrary(BrowserWindowInterface& browser,
                               WindowSession& session,
                               SidebarDownloads& downloads)
    : impl_(std::make_unique<Impl>(browser, session, downloads)) {}
SidebarLibrary::~SidebarLibrary() = default;
void SidebarLibrary::Initialize() {
  impl_->Initialize();
}
void SidebarLibrary::Visibility(uint32_t section, bool visible) {
  impl_->Visibility(section, visible);
}
void SidebarLibrary::Command(uint32_t section,
                             uint64_t item,
                             uint32_t command) {
  impl_->Command(section, item, command);
}
std::unique_ptr<ui::MenuModel> SidebarLibrary::CreateMenu(uint64_t item) {
  return impl_->CreateMenu(item);
}
}  // namespace krypton_ui
