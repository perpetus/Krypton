// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/live_folders.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include "base/auto_reset.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/sessions/exit_type_service.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_service.h"
#include "chrome/browser/sessions/session_service_factory.h"
#include "chrome/browser/ui/browser_tabrestore.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/browser_adapter.h"
#include "chrome/browser/ui/krypton_ui/live_folder_model.h"
#include "chrome/browser/ui/krypton_ui/live_folder_session.h"
#include "chrome/browser/ui/krypton_ui/multi_page_session.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/sessions/core/serialized_navigation_entry.h"
#include "components/sessions/core/serialized_user_agent_override.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "net/base/isolation_info.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "services/data_decoder/public/cpp/data_decoder.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/origin.h"

namespace krypton_ui {
namespace {
constexpr char kServiceKey = 0;
constexpr size_t kMaxDismissals = 512;
constexpr auto kNetworkTimeout = base::Seconds(20);

bool ValidDigest(std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](char c) { return base::IsHexDigit(c); });
}
bool ValidValidator(std::string_view value) {
  return value.size() <= 1024 &&
         value.find_first_of("\r\n\0", 0, 3) == std::string_view::npos;
}
bool CanPrune(content::WebContents& contents,
              const LiveFolderTabState& state,
              bool active) {
  return !state.accessed && !active && !contents.IsLoading() &&
         !contents.GetPrimaryMainFrame()->IsRenderFrameLive() &&
         !SplitSessionForTab(&contents);
}
}  // namespace

class LiveFolderService final : public base::SupportsUserData::Data,
                                public ProfileObserver {
 public:
  static LiveFolderService& Get(Profile& profile) {
    if (!profile.GetUserData(&kServiceKey))
      profile.SetUserData(&kServiceKey,
                          std::make_unique<LiveFolderService>(profile));
    return *static_cast<LiveFolderService*>(profile.GetUserData(&kServiceKey));
  }
  explicit LiveFolderService(Profile& profile) : profile_(profile) {
    profile_->AddObserver(this);
    prefs_.Init(profile_->GetPrefs());
    prefs_.Add(kFoldersPref,
               base::BindRepeating(&LiveFolderService::ConfigChanged,
                                   weak_factory_.GetWeakPtr()));
    ConfigChanged();
  }
  ~LiveFolderService() override {
    if (!shutdown_)
      profile_->RemoveObserver(this);
  }
  void OnProfileWillBeDestroyed(Profile*) override {
    shutdown_ = true;
    timer_.Stop();
    loader_.reset();
    prefs_.RemoveAll();
    weak_factory_.InvalidateWeakPtrs();
    profile_->RemoveObserver(this);
  }

  void Add(LiveFolders& client) {
    clients_.push_back(client.weak_factory_.GetWeakPtr());
    Changed();
  }
  void Remove(LiveFolders& client) {
    std::erase_if(clients_, [&](const auto& value) {
      return !value || value.get() == &client;
    });
    if (clients_.empty()) {
      timer_.Stop();
      loader_.reset();
      fetching_ = 0;
      ++generation_;
    }
  }
  std::string Status(KrFolderId id) const {
    const auto found = folders_.find(id);
    return found == folders_.end() ? std::string() : found->second.status;
  }
  void Refresh(KrFolderId id, bool restore) {
    const auto found = folders_.find(id);
    if (found == folders_.end())
      return;
    if (restore) {
      found->second.dismissed.clear();
      found->second.saturated = false;
      Save();
    }
    if (fetching_ != id)
      found->second.due = base::TimeTicks::Now();
    Changed();
  }
  void Dismiss(const LiveFolderTabState& state) {
    const auto found = folders_.find(state.folder);
    if (found == folders_.end() || !ValidDigest(state.item))
      return;
    auto& dismissed = found->second.dismissed;
    if (dismissed.contains(state.item))
      return;
    // Stop adding new entries rather than forget user dismissals at the cap.
    // The user can explicitly restore dismissed entries from the folder menu.
    if (dismissed.size() >= kMaxDismissals) {
      found->second.status =
          "Dismissal limit reached. Restore dismissed items "
          "to resume adding entries.";
      found->second.saturated = true;
    } else {
      dismissed.insert(state.item);
    }
    Save();
    Changed();
  }

 private:
  struct FolderState {
    LiveFolderConfig config;
    LiveFolderItems items;
    std::set<std::string> dismissed;
    std::string etag;
    std::string modified;
    std::string status = "Not refreshed yet.";
    base::Time last_success;
    base::TimeTicks due;
    unsigned failures = 0;
    bool valid_cache = false;
    bool saturated = false;
  };

  FolderState Load(KrFolderId id, const LiveFolderConfig& config) {
    FolderState state;
    state.config = config;
    state.due = base::TimeTicks::Now();
    const auto* cached = profile_->GetPrefs()
                             ->GetDict(kLiveFolderCachePref)
                             .FindDict(base::NumberToString(id));
    if (!cached || ReadLiveFolderConfig(*cached) != config)
      return state;
    state.valid_cache = cached->FindBool("valid").value_or(false);
    state.saturated = cached->FindBool("saturated").value_or(false);
    if (const auto* values = cached->FindList("dismissed")) {
      for (const auto& value : *values) {
        if (state.dismissed.size() == kMaxDismissals)
          break;
        if (value.is_string() && ValidDigest(value.GetString()))
          state.dismissed.insert(value.GetString());
      }
    }
    if (const auto* items = cached->FindList("items")) {
      std::set<std::string> seen;
      for (const auto& value : *items) {
        if (state.items.size() == config.max_items)
          break;
        const auto* row = value.GetIfDict();
        if (!row)
          continue;
        const auto* key = row->FindString("id");
        const auto* title = row->FindString("title");
        const auto* link = row->FindString("url");
        if (!key || !title || !link || !ValidDigest(*key) ||
            title->size() > 4096 || !base::IsStringUTF8(*title) ||
            link->size() > 2048 || !GURL(*link).SchemeIsHTTPOrHTTPS() ||
            GURL(*link).has_username() || GURL(*link).has_password() ||
            !seen.insert(*key).second)
          continue;
        state.items.push_back({*key, *title, *link});
      }
    }
    if (const auto* value = cached->FindString("etag");
        value && ValidValidator(*value))
      state.etag = *value;
    if (const auto* value = cached->FindString("modified");
        value && ValidValidator(*value))
      state.modified = *value;
    state.last_success = base::Time::FromMillisecondsSinceUnixEpoch(
        cached->FindDouble("last_success").value_or(0));
    const auto now = base::Time::Now();
    if (state.last_success > now)
      state.last_success = {};
    const auto remaining =
        state.last_success + base::Minutes(config.interval_minutes) - now;
    state.due += std::max(base::TimeDelta(), remaining);
    return state;
  }

  void Save() {
    if (shutdown_)
      return;
    base::DictValue cache;
    for (const auto& [id, state] : folders_) {
      base::DictValue row;
      WriteLiveFolderConfig(row, state.config);
      row.Set("valid", state.valid_cache);
      row.Set("saturated", state.saturated);
      row.Set("etag", state.etag);
      row.Set("modified", state.modified);
      row.Set("last_success",
              state.last_success.InMillisecondsFSinceUnixEpoch());
      base::ListValue items;
      for (const auto& item : state.items) {
        base::DictValue value;
        value.Set("id", item.id);
        value.Set("title", item.title);
        value.Set("url", item.url);
        items.Append(std::move(value));
      }
      row.Set("items", std::move(items));
      base::ListValue dismissed;
      for (const auto& item : state.dismissed)
        dismissed.Append(item);
      row.Set("dismissed", std::move(dismissed));
      cache.Set(base::NumberToString(id), std::move(row));
    }
    if (profile_->GetPrefs()->GetDict(kLiveFolderCachePref) != cache)
      profile_->GetPrefs()->SetDict(kLiveFolderCachePref, std::move(cache));
  }

  void ConfigChanged() {
    const auto catalog = ReadFolders(*profile_->GetPrefs());
    std::map<KrFolderId, FolderState> next;
    for (const auto& folder : catalog.items) {
      if (!folder.live.enabled() || next.size() == kMaxLiveFolders)
        continue;
      auto old = folders_.find(folder.id);
      if (old != folders_.end() && old->second.config == folder.live)
        next.emplace(folder.id, std::move(old->second));
      else
        next.emplace(folder.id, Load(folder.id, folder.live));
    }
    folders_ = std::move(next);
    if (fetching_ && (!folders_.contains(fetching_) ||
                      folders_.at(fetching_).config != request_config_)) {
      loader_.reset();
      fetching_ = 0;
      ++generation_;
    }
    Save();
    Changed();
  }

  void Changed() {
    if (shutdown_ || changed_pending_ || clients_.empty())
      return;
    changed_pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&LiveFolderService::ApplyChanges,
                                  weak_factory_.GetWeakPtr()));
  }
  void ApplyChanges() {
    changed_pending_ = false;
    std::erase_if(clients_, [](const auto& client) { return !client; });
    if (clients_.empty())
      return;
    Reconcile();
    for (auto& client : clients_)
      if (client)
        client->PublishStatus();
    Pump();
  }

  void Pump() {
    timer_.Stop();
    if (shutdown_ || fetching_ || clients_.empty() || folders_.empty())
      return;
    const auto next = std::ranges::min_element(
        folders_, {}, [](const auto& row) { return row.second.due; });
    const auto delay = next->second.due - base::TimeTicks::Now();
    if (delay.is_positive()) {
      timer_.Start(
          FROM_HERE, delay,
          base::BindOnce(&LiveFolderService::Pump, weak_factory_.GetWeakPtr()));
      return;
    }
    Fetch(next->first);
  }

  void Fetch(KrFolderId id) {
    auto& state = folders_.at(id);
    fetching_ = id;
    request_config_ = state.config;
    const auto generation = ++generation_;
    // Let GitHub validate its own session. Cookie names are not a stable
    // authentication API, and a preflight must not prevent a valid request.
    FetchRequest(id, generation);
  }

  void FetchRequest(KrFolderId id, uint64_t generation) {
    auto& state = folders_.at(id);
    const auto url = LiveFolderRequestUrl(state.config);
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url;
    request->method = "GET";
    request->credentials_mode = state.config.kind == KR_LIVE_FOLDER_RSS
                                    ? network::mojom::CredentialsMode::kOmit
                                    : network::mojom::CredentialsMode::kInclude;
    if (state.config.kind != KR_LIVE_FOLDER_RSS) {
      const auto origin = url::Origin::Create(url);
      request->site_for_cookies = net::SiteForCookies::FromUrl(url);
      request->request_initiator = origin;
      request->trusted_params.emplace();
      request->trusted_params->isolation_info =
          net::IsolationInfo::CreateForInternalRequest(origin);
      // A first-party profile request, with normal cookie policy. Never copy
      // or decrypt session cookies ourselves.
      request->mode = network::mojom::RequestMode::kSameOrigin;
      request->load_flags = net::LOAD_DISABLE_CACHE;
    }
    request->headers.SetHeader(
        "Accept", state.config.kind == KR_LIVE_FOLDER_RSS
                      ? "application/atom+xml,application/"
                        "rss+xml,application/xml,text/xml"
                  : state.config.kind == KR_LIVE_FOLDER_GITHUB_ISSUES
                      ? "text/html"
                      : "application/json,text/html");
    request->headers.SetHeader("Accept-Language", "en-US,en;q=0.9");
    if (state.valid_cache && !state.etag.empty())
      request->headers.SetHeader("If-None-Match", state.etag);
    if (state.valid_cache && !state.modified.empty())
      request->headers.SetHeader("If-Modified-Since", state.modified);
    static constexpr auto annotation =
        net::DefineNetworkTrafficAnnotation("krypton_live_folders", R"(
          semantics {
            sender: "Krypton Live Folders"
            description: "Fetches a feed or GitHub list chosen by the user to "
                         "update a workspace folder. No requests occur until "
                         "the user configures a Live Folder."
            trigger: "User refresh or the configured refresh deadline."
            data: "The configured feed URL or GitHub search filters. GitHub "
                  "requests use the existing profile's GitHub cookies; feed "
                  "requests omit cookies."
            destination: OTHER
            destination_other: "The user-selected feed host or github.com."
          }
          policy {
            cookies_allowed: YES
            cookies_store: "user"
            setting: "Remove or disable the Live Folder to stop requests."
            policy_exception_justification: "A user-configured optional feature."
          })");
    loader_ = network::SimpleURLLoader::Create(std::move(request), annotation);
    if (state.config.kind != KR_LIVE_FOLDER_RSS) {
      // GitHub reports a signed-out dashboard as 404; its bounded body
      // distinguishes that from other errors.
      loader_->SetAllowHttpErrorResults(true);
      loader_->SetOnRedirectCallback(
          base::BindRepeating(&LiveFolderService::GitHubRedirect,
                              weak_factory_.GetWeakPtr(), id, generation));
    }
    loader_->SetTimeoutDuration(kNetworkTimeout);
    state.status = "Refreshing...";
    Changed();
    loader_->DownloadToString(
        profile_->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess()
            .get(),
        base::BindOnce(&LiveFolderService::Downloaded,
                       weak_factory_.GetWeakPtr(), id, generation),
        kMaxLiveResponseBytes);
  }

  void GitHubRedirect(KrFolderId id,
                      uint64_t generation,
                      const GURL&,
                      const net::RedirectInfo& redirect,
                      const network::mojom::URLResponseHead&,
                      std::vector<std::string>*) {
    if (generation != generation_ || fetching_ != id || !loader_)
      return;
    const auto& target = redirect.new_url;
    const bool same_origin =
        target.is_valid() && !target.has_username() && !target.has_password() &&
        url::Origin::Create(LiveFolderRequestUrl(request_config_))
            .IsSameOriginWith(target);
    const bool login = same_origin && target.path() == "/login";
    if (same_origin && !login)
      return;
    // SimpleURLLoader permits deletion in this callback. Never follow a
    // credentialed provider request onto another origin.
    loader_.reset();
    Complete(id, generation,
             LiveFolderResult(base::unexpected(
                 login ? kLiveFolderGithubSignedOut
                       : "GitHub redirected outside its dashboard. Cached "
                         "items are kept.")));
  }

  void Downloaded(KrFolderId id,
                  uint64_t generation,
                  std::optional<std::string> body) {
    if (generation != generation_ || fetching_ != id || !loader_)
      return;
    const auto* head = loader_->ResponseInfo();
    const auto headers = head ? head->headers : nullptr;
    const int status = headers ? headers->response_code() : 0;
    const int network_error = loader_->NetError();
    const auto final_url = loader_->GetFinalURL();
    const auto requested = LiveFolderRequestUrl(request_config_);
    if (status == 304 && folders_.at(id).valid_cache) {
      loader_.reset();
      Complete(id, generation, std::nullopt);
      return;
    }
    pending_etag_ =
        headers ? headers->GetNormalizedHeader("etag").value_or("") : "";
    pending_modified_ =
        headers ? headers->GetNormalizedHeader("last-modified").value_or("")
                : "";
    loader_.reset();
    const auto location =
        headers ? headers->GetNormalizedHeader("location") : std::nullopt;
    const auto redirect = location ? requested.Resolve(*location) : GURL();
    if (request_config_.kind != KR_LIVE_FOLDER_RSS &&
        (status == 401 ||
         (status >= 300 && status < 400 && redirect.SchemeIs("https") &&
          redirect.host() == "github.com" && redirect.path() == "/login"))) {
      Complete(id, generation,
               LiveFolderResult(base::unexpected(kLiveFolderGithubSignedOut)));
      return;
    }
    if (request_config_.kind != KR_LIVE_FOLDER_RSS && status == 404 && body) {
      // A 404 is signed out only when its body says so. Classify it off the
      // UI thread; no cookies, filters or response bodies are logged.
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::TaskPriority::BEST_EFFORT},
          base::BindOnce(&ParseLiveGithubError, status, std::move(*body)),
          base::BindOnce(&LiveFolderService::Parsed, weak_factory_.GetWeakPtr(),
                         id, generation));
      return;
    }
    if (request_config_.kind != KR_LIVE_FOLDER_RSS && status != 200) {
      // Report bounded status only: no cookies, filters or response body logs.
      Complete(
          id, generation,
          LiveFolderResult(base::unexpected(
              status ? "GitHub returned HTTP " + base::NumberToString(status) +
                           ". Cached items are kept."
                     : "Could not reach GitHub (" +
                           std::string(net::ErrorToString(network_error)) +
                           "). Cached items are kept.")));
      return;
    }
    if (!body || status != 200 || !final_url.SchemeIsHTTPOrHTTPS() ||
        (requested.SchemeIs("https") && !final_url.SchemeIs("https"))) {
      Complete(id, generation,
               LiveFolderResult(base::unexpected(
                   request_config_.kind == KR_LIVE_FOLDER_RSS
                       ? "Could not fetch the feed. Cached items are kept."
                       : "Could not fetch GitHub. Cached items are kept.")));
      return;
    }
    if (request_config_.kind == KR_LIVE_FOLDER_RSS) {
      data_decoder::DataDecoder::ParseXmlIsolated(
          *body, data_decoder::mojom::XmlParser::WhitespaceBehavior::kIgnore,
          base::BindOnce(&LiveFolderService::XmlDecoded,
                         weak_factory_.GetWeakPtr(), id, generation,
                         final_url));
    } else {
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::TaskPriority::BEST_EFFORT},
          base::BindOnce(&ParseLiveGithub, std::move(*body), request_config_),
          base::BindOnce(&LiveFolderService::Parsed, weak_factory_.GetWeakPtr(),
                         id, generation));
    }
  }
  void XmlDecoded(KrFolderId id,
                  uint64_t generation,
                  GURL base_url,
                  data_decoder::DataDecoder::ValueOrError tree) {
    if (generation != generation_ || fetching_ != id)
      return;
    if (!tree.has_value()) {
      Complete(id, generation,
               LiveFolderResult(base::unexpected(
                   "Could not read the feed. Cached items are kept.")));
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::TaskPriority::BEST_EFFORT},
        base::BindOnce(&ParseLiveFeed, std::move(*tree), std::move(base_url),
                       request_config_),
        base::BindOnce(&LiveFolderService::Parsed, weak_factory_.GetWeakPtr(),
                       id, generation));
  }
  void Parsed(KrFolderId id, uint64_t generation, LiveFolderResult result) {
    Complete(id, generation, std::move(result));
  }
  void Complete(KrFolderId id,
                uint64_t generation,
                std::optional<LiveFolderResult> result) {
    if (generation != generation_ || fetching_ != id || !folders_.contains(id))
      return;
    auto& state = folders_.at(id);
    const bool success = !result || result->has_value();
    if (success) {
      if (result) {
        state.items = std::move(**result);
        state.etag = ValidValidator(pending_etag_) ? pending_etag_ : "";
        state.modified =
            ValidValidator(pending_modified_) ? pending_modified_ : "";
      }
      state.valid_cache = true;
      state.failures = 0;
      state.last_success = base::Time::Now();
      state.status = !state.items.empty() ? "Up to date."
                     : state.config.kind == KR_LIVE_FOLDER_RSS
                         ? "No feed entries."
                     : state.config.kind == KR_LIVE_FOLDER_GITHUB_PULLS
                         ? "No matching pull requests."
                         : "No matching issues.";
    } else {
      state.status = result->error();
      state.failures = std::min(state.failures + 1, 4u);
    }
    const auto minutes =
        std::min(480u, state.config.interval_minutes * (1u << state.failures));
    state.due = base::TimeTicks::Now() + base::Minutes(minutes);
    fetching_ = 0;
    Save();
    Changed();
  }

  void Reconcile();
  friend class LiveFolders;
  const raw_ref<Profile> profile_;
  PrefChangeRegistrar prefs_;
  std::vector<base::WeakPtr<LiveFolders>> clients_;
  std::map<KrFolderId, FolderState> folders_;
  base::OneShotTimer timer_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  LiveFolderConfig request_config_;
  std::string pending_etag_;
  std::string pending_modified_;
  KrFolderId fetching_ = 0;
  uint64_t generation_ = 0;
  bool changed_pending_ = false;
  bool reconciling_ = false;
  bool shutdown_ = false;
  base::WeakPtrFactory<LiveFolderService> weak_factory_{this};
};

LiveFolders::LiveFolders(BrowserAdapter& owner) : owner_(owner) {
  restored_ =
      SessionRestore::RegisterOnSessionRestoredCallback(base::BindRepeating(
          [](base::WeakPtr<LiveFolders> self, Profile* profile, int) {
            if (self && self->owner_->browser_->GetProfile() == profile)
              self->DeferStart();
          },
          weak_factory_.GetWeakPtr()));
  if (auto* exit = ExitTypeService::GetInstanceForProfile(
          owner_->browser_->GetProfile())) {
    exit->AddCrashAckCallback(
        base::BindOnce(&LiveFolders::DeferStart, weak_factory_.GetWeakPtr()));
  }
  DeferStart();
}
LiveFolders::~LiveFolders() {
  TabStripModelObserver::StopObservingAll(this);
  if (service_)
    service_->Remove(*this);
}
bool LiveFolders::Ready() const {
  const auto* exit =
      ExitTypeService::GetInstanceForProfile(owner_->browser_->GetProfile());
  return !owner_->closed_ &&
         !SessionRestore::IsRestoring(owner_->browser_->GetProfile()) &&
         (!exit || !exit->waiting_for_user_to_ack_crash());
}
void LiveFolders::DeferStart() {
  if (!owner_->closed_)
    std::ignore = owner_->session_.Defer(
        base::BindOnce(&LiveFolders::Start, weak_factory_.GetWeakPtr()));
}
void LiveFolders::Start() {
  if (!Ready())
    return;
  if (service_) {
    service_->Changed();
    return;
  }
  service_ = &LiveFolderService::Get(*owner_->browser_->GetProfile());
  service_->Add(*this);
}
void LiveFolders::PublishStatus() {
  if (!service_ || !Ready())
    return;
  if (!observing_ && !service_->folders_.empty()) {
    owner_->browser_->GetTabStripModel()->AddObserver(this);
    observing_ = true;
  } else if (observing_ && service_->folders_.empty()) {
    TabStripModelObserver::StopObservingAll(this);
    observing_ = false;
  }
  for (auto& folder : owner_->folders_.items)
    folder.live_status = service_->Status(folder.id);
  owner_->PublishFolders();
}
void LiveFolders::StoreTabState(content::WebContents* contents,
                                LiveFolderTabState state) {
  SetLiveFolderTabState(contents, std::move(state));
  if (auto* session = SessionServiceFactory::GetForProfile(
          owner_->browser_->GetProfile())) {
    const auto tab_id = sessions::SessionTabHelper::IdForTab(contents);
    if (tab_id.is_valid())
      session->AddTabExtraData(
          owner_->browser_->GetSessionID(), tab_id, kLiveFolderItemSessionKey,
          EncodeLiveFolderTabState(LiveFolderStateForTab(contents)));
  }
}
void LiveFolders::OnTabStripModelChanged(
    TabStripModel* model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (!service_ || !Ready() || service_->reconciling_)
    return;
  if (change.type() == TabStripModelChange::kReplaced) {
    const auto* replaced = change.GetReplace();
    auto state = LiveFolderStateForTab(replaced->old_contents);
    if (state.folder) {
      state.accessed |=
          replaced->old_contents->GetPrimaryMainFrame()->IsRenderFrameLive();
      StoreTabState(replaced->new_contents, std::move(state));
    }
  }
  if (change.type() == TabStripModelChange::kRemoved && !model->closing_all()) {
    for (const auto& removed : change.GetRemove()->contents)
      if (TabRemoveReasonUtils::WillDeleteWebContents(removed.remove_reason))
        service_->Dismiss(LiveFolderStateForTab(removed.contents));
  }
  if (selection.active_tab_changed() && selection.new_contents) {
    auto state = LiveFolderStateForTab(selection.new_contents);
    if (state.folder && !state.accessed) {
      state.accessed = true;
      StoreTabState(selection.new_contents, std::move(state));
    }
  }
}
KrResult LiveFolders::Command(const KrFolderCommand& command) {
  if (!Ready())
    return KR_ERR_UNSUPPORTED;
  Start();
  if (!service_)
    return KR_ERR_UNSUPPORTED;
  if (command.kind == KR_FOLDER_REFRESH_LIVE ||
      command.kind == KR_FOLDER_RESTORE_LIVE_ITEMS) {
    service_->Refresh(command.folder,
                      command.kind == KR_FOLDER_RESTORE_LIVE_ITEMS);
    return KR_OK;
  }
  if (!KR_HAS_FIELD(&command, KrFolderCommand, live_max_items) ||
      (!command.live_source.data && command.live_source.size) ||
      command.live_source.size > 2048)
    return KR_ERR_INVALID_ARGUMENT;
  LiveFolderConfig config{
      command.live_kind,
      command.live_source.size
          ? std::string(command.live_source.data, command.live_source.size)
          : "",
      command.live_interval_minutes, command.live_max_items};
  base::TrimWhitespaceASCII(config.source, base::TRIM_ALL, &config.source);
  if (!config.enabled())
    config = {};
  if (!ValidLiveFolderConfig(config))
    return KR_ERR_INVALID_ARGUMENT;
  auto catalog = ReadFolders(*owner_->browser_->GetProfile()->GetPrefs());
  const auto found =
      std::ranges::find(catalog.items, command.folder, &TabFolder::id);
  if (found == catalog.items.end())
    return KR_ERR_INVALID_ARGUMENT;
  found->live = std::move(config);
  return WriteFolders(*owner_->browser_->GetProfile()->GetPrefs(), catalog)
             ? KR_OK
             : KR_ERR_INVALID_ARGUMENT;
}

void LiveFolderService::Reconcile() {
  if (reconciling_ || clients_.empty() ||
      std::ranges::any_of(clients_, [](const auto& client) {
        return !client || !client->Ready();
      }))
    return;
  const base::AutoReset<bool> guard(&reconciling_, true);
  size_t created = 0;
  // Snapshot identities: tab insertion/removal synchronously notifies the host.
  struct Existing {
    base::WeakPtr<LiveFolders> client;
    base::WeakPtr<tabs::TabInterface> tab;
    LiveFolderTabState state;
  };
  std::vector<Existing> existing;
  for (const auto& client : clients_) {
    for (auto* tab : *client->owner_->browser_->GetTabStripModel()) {
      auto state = LiveFolderStateForTab(tab->GetContents());
      if (state.folder)
        existing.push_back({client, tab->GetWeakPtr(), std::move(state)});
    }
  }
  std::map<KrFolderId, LiveFolders*> owners;
  std::set<std::pair<KrFolderId, std::string>> present;
  for (const auto& record : existing) {
    if (!record.client || !record.tab)
      continue;
    auto* contents = record.tab->GetContents();
    if (!record.state.accessed &&
        (record.tab->IsActivated() ||
         contents->GetPrimaryMainFrame()->IsRenderFrameLive())) {
      auto accessed = record.state;
      accessed.accessed = true;
      record.client->StoreTabState(contents, std::move(accessed));
    }
    const auto found = folders_.find(record.state.folder);
    if (found == folders_.end()) {
      // Disabling/deleting a source converts its surviving tabs to normal tabs.
      record.client->StoreTabState(contents, {});
      continue;
    }
    auto& state = found->second;
    if (FolderForTab(contents) != record.state.folder ||
        !record.tab->IsPinned()) {
      // Moving an item out is a dismissal; it must not reappear at refresh.
      Dismiss(record.state);
      record.client->StoreTabState(contents, {});
      continue;
    }
    owners.try_emplace(record.state.folder, record.client.get());
    const bool duplicate =
        !present.emplace(record.state.folder, record.state.item).second;
    const bool stale = state.valid_cache &&
                       std::ranges::none_of(state.items, [&](const auto& item) {
                         return item.id == record.state.item;
                       });
    if (!record.state.accessed &&
        !contents->GetPrimaryMainFrame()->IsRenderFrameLive() &&
        !record.tab->IsActivated()) {
      const auto item = std::ranges::find(state.items, record.state.item,
                                          &LiveFolderItem::id);
      auto* entry = contents->GetController().GetLastCommittedEntry();
      if (item != state.items.end() && entry) {
        const auto title = base::UTF8ToUTF16(item->title);

        if (entry->GetTitle() != title) {
          entry->SetTitle(title);

          contents->NotifyNavigationStateChanged(
              content::INVALIDATE_TYPE_TITLE);
        }
      }
    }
    if (duplicate || stale || state.dismissed.contains(record.state.item)) {
      if (CanPrune(*contents, record.state, record.tab->IsActivated())) {
        record.client->owner_->browser_->GetTabStripModel()->CloseWebContents(
            contents, TabCloseTypes::CLOSE_NONE);
      } else {
        // Never close a page the user opened, including after discard/restart.
        record.client->StoreTabState(contents, {});
      }
    }
  }
  for (const auto& [id, state] : folders_) {
    if (state.saturated)
      continue;
    LiveFolders* client =
        owners.contains(id) ? owners.at(id) : clients_.front().get();
    if (!client || !client->Ready())
      continue;
    auto& adapter = *client->owner_;
    const auto* folder = adapter.folders_.Find(id);
    if (!folder)
      continue;
    for (const auto& item : state.items) {
      if (state.dismissed.contains(item.id) || present.contains({id, item.id}))
        continue;
      bool adopted = false;
      for (auto* tab : *adapter.browser_->GetTabStripModel()) {
        if (FolderForTab(tab->GetContents()) == id &&
            !LiveFolderStateForTab(tab->GetContents()).folder &&
            tab->GetContents()->GetVisibleURL() == GURL(item.url)) {
          client->StoreTabState(tab->GetContents(), {id, item.id, true});
          adopted = true;
          present.emplace(id, item.id);
          break;
        }
      }
      if (adopted)
        continue;
      // Yield between small batches instead of constructing a large feed's
      // WebContents in one UI task. This is queued work, not state polling.
      if (created == 2) {
        Changed();
        return;
      }
      ++created;
      sessions::SerializedNavigationEntry navigation;
      navigation.set_index(0);
      navigation.set_virtual_url(GURL(item.url));
      navigation.set_original_request_url(GURL(item.url));
      navigation.set_title(base::UTF8ToUTF16(item.title));
      navigation.set_timestamp(base::Time::Now());
      std::map<std::string, std::string> extra;
      EncodeWorkspaceSessionData({folder->space, 0, id}, extra);
      extra[kLiveFolderItemSessionKey] =
          EncodeLiveFolderTabState({id, item.id, false});
      const base::AutoReset<KrSpaceId> workspace(&adapter.new_tab_space_,
                                                 folder->space);
      const base::AutoReset<KrFolderId> parent(&adapter.new_folder_, id);
      chrome::AddRestoredTab(
          &*adapter.browser_, base::span_from_ref(navigation),
          adapter.browser_->GetTabStripModel()->IndexOfFirstNonPinnedTab(), 0,
          {}, std::nullopt, false, true, {}, {}, nullptr,
          sessions::SerializedUserAgentOverride(), extra, false, std::nullopt,
          chrome::RestoredTabLoadPolicy::kDeferUntilActivation);
      present.emplace(id, item.id);
    }
  }
}
}  // namespace krypton_ui
