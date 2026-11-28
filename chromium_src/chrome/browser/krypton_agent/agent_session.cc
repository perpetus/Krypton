#include "chrome/browser/krypton_agent/agent_session.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/base64url.h"
#include "base/callback_list.h"
#include "base/command_line.h"
#include "base/containers/adapters.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/unguessable_token.h"
#include "base/values.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_metrics.h"
#include "chrome/browser/actor/actor_tab_data.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "chrome/browser/actor/enterprise_policy_checker.h"
#include "chrome/browser/actor/tab_observation_strategy.h"
#include "chrome/browser/actor/tools/history_tool_request.h"
#include "chrome/browser/actor/tools/tool_request.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/krypton_agent/agent_tools.h"
#include "chrome/browser/krypton_agent/browser_prompt_gate.h"
#include "chrome/browser/krypton_agent/chat/chat_card.h"
#include "chrome/browser/krypton_agent/confirmation_gate.h"
#include "chrome/browser/krypton_agent/history_index.h"
#include "chrome/browser/krypton_agent/history_maintenance.h"
#include "chrome/browser/krypton_agent/history_prefs.h"
#include "chrome/browser/krypton_agent/history_space.h"
#include "chrome/browser/krypton_agent/launch_authorization.h"
#include "chrome/browser/krypton_agent/organization.h"
#include "chrome/browser/krypton_agent/page_reader.h"
#include "chrome/browser/krypton_agent/privacy_filter.h"
#include "chrome/browser/krypton_agent/research_load.h"
#include "chrome/browser/krypton_agent/saved_transcript_reader.h"
#include "chrome/browser/krypton_agent/sidecar_process.h"
#include "chrome/browser/krypton_agent/site_allowlist.h"
#include "chrome/browser/krypton_agent/source_records.h"
#include "chrome/browser/krypton_agent/tool_pipe.h"
#include "chrome/browser/krypton_agent/ui_state_manager.h"
#include "chrome/browser/krypton_privacy_features/registry.h"
#include "chrome/browser/page_content_annotations/page_content_screenshot_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/krypton_ui/workspace_session.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/common/actor/action_result.h"
#include "components/actor/core/task_source_info.h"
#include "components/javascript_dialogs/app_modal_dialog_controller.h"
#include "components/javascript_dialogs/app_modal_dialog_queue.h"
#include "components/krypton_privacy_features/privacy_features.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/page_content_annotations/content/page_content_screenshot_service.h"
#include "components/paint_preview/common/mojom/paint_preview_types.mojom.h"
#include "components/permissions/permission_request_manager.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_service.h"
#include "components/sessions/core/session_id.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/base/url_util.h"
#include "pdf/buildflags.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/mojom/clear_data_filter.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/win/screen_win.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/win/hwnd_util.h"
#include "url/origin.h"

#if BUILDFLAG(ENABLE_PDF)
#include "components/pdf/browser/pdf_document_helper.h"
#endif

namespace krypton_agent {
namespace {

constexpr char kDevelopmentSwitch[] = "krypton-agent-dev-pipe";
constexpr size_t kResponseBytes = 40 * 1024;
constexpr std::string_view kContextTabInstruction =
    "\n\n[Krypton: The user attached a tab to this message. "
    "Use read_context_tab to read its filtered text if relevant.]";

base::DictValue Envelope(std::string_view id) {
  return base::DictValue().Set("v", 1).Set("type", "response").Set("id", id);
}

base::DictValue ProtocolError(std::string_view id, std::string_view code,
                              std::string_view message) {
  return Envelope(id).Set(
      "error", base::DictValue().Set("code", code).Set("message", message));
}

base::DictValue ToolResult(std::string_view id, std::string text, bool error,
                           int continuation = 0) {
  auto result =
      base::DictValue()
          .Set("content",
               base::ListValue().Append(base::DictValue()
                                            .Set("type", "text")
                                            .Set("text", std::move(text))))
          .Set("isError", error);
  if (continuation) {
    result.Set("continuation", continuation);
  }
  return Envelope(id).Set("result", std::move(result));
}

std::string_view ActionFailureReason(actor::mojom::ActionResultCode code) {
  using Code = actor::mojom::ActionResultCode;
  switch (code) {
  case Code::kUrlBlocked:
  case Code::kTriggeredNavigationBlocked:
  case Code::kActionsBlockedForSiteRisk:
    return "Browser site rules blocked the action";
  case Code::kActionsBlockedSafeBrowsingDisabled:
    return "The action requires Safe Browsing";
  case Code::kActionsBlockedByEnterprisePolicy:
  case Code::kActionBlockedByEnterpriseContentScan:
    return "Enterprise policy blocked the action";
  case Code::kActionsBlockedForScheme:
  case Code::kExternalProtocolNavigationBlocked:
    return "The browser blocked this URL scheme";
  case Code::kActionsBlockedOnErrorPage:
  case Code::kNavigateCommittedErrorPage:
  case Code::kHistoryErrorPage:
    return "The browser reached an error page";
  case Code::kCrossOriginNavigation:
    return "The page moved outside the task's allowed origin";
  case Code::kFrameWentAway:
  case Code::kInvalidDomNodeId:
  case Code::kFrameLocationChangedSinceObservation:
  case Code::kObservedTargetElementDestroyed:
  case Code::kObservedTargetElementChanged:
  case Code::kHistoryNavigationEntryChanged:
  case Code::kUserNavigatedAway:
    return "The page changed. Read it again before retrying";
  case Code::kElementDisabled:
  case Code::kSelectOptionDisabled:
    return "The target is disabled";
  case Code::kElementOffscreen:
  case Code::kCoordinatesOutOfBounds:
    return "The target is outside the visible page";
  case Code::kTargetNodeInteractionPointObscured:
    return "Another element covers the target";
  case Code::kToolTimeout:
    return "The action timed out";
  case Code::kNavigateInvalidUrl:
    return "The navigation URL is invalid";
  case Code::kArgumentsInvalid:
  case Code::kEmptyActionSequence:
  case Code::kClickMissingTarget:
  case Code::kClickMissingType:
  case Code::kClickInvalidCount:
  case Code::kScrollMissingDirection:
  case Code::kScrollMissingDistance:
  case Code::kTypeUnsupportedCharacters:
  case Code::kTypeFailedMappingCharToKey:
  case Code::kTypeInvalidTextEncoding:
    return "The action arguments are invalid or unsupported";
  case Code::kTabWentAway:
  case Code::kWindowWentAway:
    return "The shared tab or window closed";
  case Code::kTaskWentAway:
  case Code::kInvalidTaskStateForAct:
    return "The browser task is no longer available";
  case Code::kTaskPaused:
    return "The browser task is paused";
  case Code::kActionTargetCrossProfile:
    return "The target belongs to another profile";
  case Code::kNavigateFailedToStart:
  case Code::kHistoryNoNavigationsCreated:
  case Code::kHistoryCancelledBeforeStart:
  case Code::kHistoryFailedBeforeCommit:
    return "The navigation could not complete";
  case Code::kHistoryNoBackEntries:
  case Code::kHistoryNoForwardEntries:
    return "There is no page in that direction in the tab history";
  case Code::kScrollNoScrollingElement:
  case Code::kScrollTargetNotUserScrollable:
    return "The target cannot be scrolled";
  case Code::kScrollOffsetDidNotChange:
    return "The target's scroll position did not change";
  case Code::kSelectInvalidElement:
    return "The target is not a selection control";
  case Code::kSelectNoSuchOption:
    return "The requested option is unavailable";
  case Code::kTypeTargetNotElement:
  case Code::kTypeTargetNotFocusable:
    return "The target cannot receive text input";
  case Code::kClickSuppressed:
  case Code::kTypeKeyDownSuppressed:
    return "The browser suppressed the input event";
  case Code::kFindAndHighlightTextNotFound:
    return "The requested text was not found";
  case Code::kSearchServiceUnavailable:
  case Code::kDefaultSearchProviderNotSet:
  case Code::kSearchUrlGenerationFailed:
    return "The default search engine could not supply a search URL";
  default:
    return "The browser could not complete the action";
  }
}

std::string DescribeActionFailure(actor::mojom::ActionResultCode code) {
  return base::StrCat({ActionFailureReason(code), " (browser action code ",
                       base::NumberToString(static_cast<int>(code)), ")."});
}

std::string_view HandoffName(HandoffReason reason) {
  switch (reason) {
  case HandoffReason::kSignIn:
    return "sign_in";
  case HandoffReason::kVerify:
    return "verify";
  case HandoffReason::kEnterInformation:
    return "enter_information";
  case HandoffReason::kNone:
    break;
  }
  return "take_over";
}

bool IsUrlTool(std::string_view name) {
  return name == "navigate" || name == "open_url";
}

bool IsHistoryTool(std::string_view name) {
  return name == "go_back" || name == "go_forward";
}

bool IsNavigation(std::string_view name) {
  return IsUrlTool(name) || IsHistoryTool(name) || name == "perform_search" ||
         name == "reload_page";
}

bool IsResearchTool(std::string_view name) {
  return name == "web_search" || name == "read_url";
}

bool IsPageTool(std::string_view name) {
  return !name.empty() && name != "read_context_tab" && name != "html_render" &&
         name != "html_preview" && name != "html_guide" &&
         name != "krypton_help" && name != "krypton_list_work_tabs" &&
         name != "krypton_close_work_tab" && !IsResearchTool(name) &&
         !Organization::Find(name);
}

std::string ResearchSite(const GURL &url) {
  auto site = net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  if (site.empty()) {
    return std::string(url.host());
  }
  return site;
}

GURL SearchResultDestination(const GURL &engine, const GURL &link) {
  if (link.DomainIs("googleadservices.com") ||
      link.DomainIs("doubleclick.net") || link.path() == "/aclk") {
    return GURL();
  }
  std::string destination;
  if (engine.DomainIs("duckduckgo.com") && link.DomainIs("duckduckgo.com") &&
      link.path() == "/l/" &&
      net::GetValueForKeyInQuery(link, "uddg", &destination)) {
    return GURL(destination);
  }
  if (engine.DomainIs("bing.com") && link.DomainIs("bing.com") &&
      link.path() == "/ck/a") {
    std::string encoded;
    if (net::GetValueForKeyInQuery(link, "u", &encoded) &&
        encoded.starts_with("a1") &&
        base::Base64UrlDecode(std::string_view(encoded).substr(2),
                              base::Base64UrlDecodePolicy::IGNORE_PADDING,
                              &destination)) {
      return GURL(destination);
    }
    return GURL();
  }
  if (url::Origin::Create(engine) != url::Origin::Create(link)) {
    return link;
  }
  if (link.path() == "/url" &&
      (net::GetValueForKeyInQuery(link, "q", &destination) ||
       net::GetValueForKeyInQuery(link, "url", &destination))) {
    return GURL(destination);
  }
  return GURL();
}

std::string MarkdownText(std::string_view text) {
  std::string escaped;
  for (char character : text) {
    if (character == '\r' || character == '\n') {
      escaped.push_back(' ');
      continue;
    }
    if (std::string_view("\\*_[]\x60#<>|").find(character) !=
        std::string_view::npos) {
      escaped.push_back('\\');
    }
    escaped.push_back(character);
  }
  return escaped;
}

bool HasHighEntropySegment(std::string_view text) {
  std::array<bool, 128> seen{};
  size_t length = 0;
  size_t distinct = 0;
  for (unsigned char c : text) {
    if (!(base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) || c == '-' ||
          c == '_')) {
      seen.fill(false);
      length = 0;
      distinct = 0;
      continue;
    }
    distinct += !seen[c];
    seen[c] = true;
    if (++length >= 24 && distinct >= 12) {
      return true;
    }
  }
  return false;
}

std::optional<url::Origin> TaintInValue(const PrivacyFilter &filter,
                                        const base::Value &value,
                                        const url::Origin &destination,
                                        bool allow_same_origin) {
  if (value.is_string()) {
    return filter.FindTaint(value.GetString(), destination, allow_same_origin);
  }
  if (value.is_list()) {
    for (const auto &child : value.GetList()) {
      if (auto origin =
              TaintInValue(filter, child, destination, allow_same_origin)) {
        return origin;
      }
    }
  }
  if (value.is_dict()) {
    for (const auto [key, child] : value.GetDict()) {
      if (auto origin =
              TaintInValue(filter, child, destination, allow_same_origin)) {
        return origin;
      }
    }
  }
  return std::nullopt;
}

// T3's limit for one turn's input (PROVIDER_SEND_TURN_MAX_INPUT_CHARS),
// counted in UTF-16 code units as T3 counts it.
constexpr size_t kMaxPromptChars = 120000;

std::string_view Field(const base::DictValue &data, std::string_view key) {
  const auto *value = data.FindString(key);
  return value ? std::string_view(*value) : std::string_view();
}

bool PromptFits(std::string_view text) {
  // A UTF-8 byte encodes at most one UTF-16 code unit.
  return text.size() <= kMaxPromptChars ||
         base::UTF8ToUTF16(text).size() <= kMaxPromptChars;
}

class AgentSession final
    : public content::WebContentsObserver,
      public permissions::PermissionRequestManager::Observer {
public:
  AgentSession(tabs::TabInterface &tab, actor::ActorKeyedService &service)
      : content::WebContentsObserver(tab.GetContents()), tab_(tab.GetHandle()),
        window_(tab.GetBrowserWindowInterface()->GetSessionID()),
        standalone_(tab.GetContents()->GetLastCommittedURL() ==
                    GURL(kAgentNewTabUrl)),
        origin_(
            tab.GetContents()->GetPrimaryMainFrame()->GetLastCommittedOrigin()),
        shared_origin_(origin_), service_(service.GetWeakPtr()) {}

  ~AgentSession() override {
    events_.Reset();
    Stop();
  }

  void StartProduction(SessionOptions options, SessionEvent events) {
    options_ = std::move(options);
    events_ = std::move(events);
    auto *profile = tab_.Get()->GetBrowserWindowInterface()->GetProfile();
    profile_path_ = profile->GetPath();
    auto catalog = krypton_ui::WorkspaceCatalogForTab(web_contents());
    if (catalog.empty()) {
      catalog = profile_path_.BaseName().AsUTF8Unsafe();
    }
    space_id_ =
        HistorySpaceKey(catalog, krypton_ui::WorkspaceForTab(web_contents()));
    chat_card_ = ChatCard::Create(
        *profile, base::BindRepeating(&AgentSession::ReviewChatLink,
                                      weak_factory_.GetWeakPtr()));
    if (chat_card_ && options_->suggest_prompt) {
      chat_card_->SetPromptSuggestionHandler(base::BindRepeating(
          &AgentSession::SuggestPrompt, weak_factory_.GetWeakPtr()));
    }
    component_.Ensure(
        profile->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess(),
        base::BindOnce(&AgentSession::OnComponent, weak_factory_.GetWeakPtr()));
  }

  bool SendMessage(std::string message, std::string model,
                   std::string reasoning_option, std::string reasoning,
                   std::optional<ContextTab> context_tab) {
    if (!session_ready_ || !InScope() || !history_revocation_.empty() ||
        message.empty() || !PromptFits(message)) {
      return false;
    }
    auto &window = *tab_.Get()->GetBrowserWindowInterface();
    if (context_tab && !context_tab->GetContents(window)) {
      return false;
    }
    const auto provider_message =
        context_tab ? base::StrCat({message, kContextTabInstruction}) : message;
    if (!PromptFits(provider_message)) {
      return false;
    }
    ResetIdleTimer();
    RecordHistoryActivity();
    auto params = base::DictValue()
                      .Set("session_id", session_id_)
                      .Set("user_message", provider_message);
    if (!model.empty()) {
      params.Set("model", std::move(model));
    }
    if (!reasoning.empty()) {
      params.Set("reasoning", base::DictValue()
                                  .Set("option", std::move(reasoning_option))
                                  .Set("value", std::move(reasoning)));
    }
    if (!SendSidecar("session.send", std::move(params))) {
      return false;
    }
    context_tab_ = std::move(context_tab);
    AddChatPrompt(message);
    sources_.StartTurn();
    return true;
  }

  ChatCard *chat_card() { return chat_card_.get(); }

  bool OpenChat(BrowserWindowInterface &window) {
    return chat_card_ && OwnsChatWindow(window) && chat_card_->Focus();
  }

  bool SetChatBounds(BrowserWindowInterface &window, const gfx::Rect &bounds) {
    if (!chat_card_ || !OwnsChatWindow(window)) {
      return false;
    }
    return PlaceAgentChatCard(*chat_card_, window, bounds);
  }

  bool OwnsChatWindow(BrowserWindowInterface &window) const {
    return IsProductionAgentEnabled() && window.GetSessionID() == window_ &&
           window.GetProfile() &&
           window.GetProfile()->GetPath() == profile_path_;
  }

  bool stopped() const { return !web_contents(); }

  bool UsesTab(tabs::TabInterface &tab) const {
    return tab_ == tab.GetHandle();
  }
  bool ShowsWorkingOn(tabs::TabInterface &tab) const {
    return actor_tab_bound_ && PageTab() == tab.GetHandle() && PageInScope();
  }

  void StopTurn() {
    if (!stopped()) {
      Stop();
    }
  }

  const base::FilePath &profile_path() const { return profile_path_; }

  bool OpenSavedTranscript(std::string session_id, int cursor,
                           SavedTranscriptReply reply) {
    if (!session_ready_ || !history_revocation_.empty() || transcript_reply_ ||
        session_id.empty() || session_id.size() > 128 || cursor < 0) {
      return false;
    }
    auto *os_crypt = g_browser_process->os_crypt_async();
    if (!os_crypt) {
      return false;
    }
    transcript_session_ = std::move(session_id);
    transcript_reply_ = std::move(reply);
    const auto request = ++transcript_sequence_;
    transcript_timeout_.Start(
        FROM_HERE, base::Seconds(20),
        base::BindOnce(&AgentSession::CancelSavedTranscript,
                       weak_factory_.GetWeakPtr()));
    os_crypt->GetInstance(base::BindOnce(&AgentSession::OnTranscriptEncryptor,
                                         weak_factory_.GetWeakPtr(), request,
                                         cursor));
    return true;
  }

  void PurgeHistory(base::ListValue ids) {
    if (transcript_reply_ && ids.contains(transcript_session_)) {
      CancelSavedTranscript();
    }
    const bool active_deleted = ids.contains(session_id_);
    const bool sent =
        hello_ && SendSidecar("history.purge",
                              base::DictValue().Set("ids", std::move(ids)));
    if (!active_deleted) {
      return;
    }
    if (!sent) {
      Stop();
      return;
    }
    history_revocation_ =
        base::StrCat({"b-", base::NumberToString(message_id_)});
    FailQueuedCalls("This saved session was deleted.");
    AbortCall("session_deleted", "This saved session was deleted.");
    if (auto *task = service_ ? service_->GetTask(task_id_) : nullptr;
        task && task->IsUnderActorControl()) {
      task->Pause(/*from_actor=*/true);
    }
    pipe_.reset();
    handshake_.Start(
        FROM_HERE, base::Seconds(5),
        base::BindOnce(&AgentSession::Stop, weak_factory_.GetWeakPtr()));
  }

  void Start() {
    if (!BindActorTask(tab_, base::BindOnce(&AgentSession::OnTabBound,
                                            weak_factory_.GetWeakPtr()))) {
      Stop();
    }
  }

  bool BindActorTask(tabs::TabHandle tab,
                     actor::ActorTask::AddTabCallback callback) {
    if (!service_) {
      return false;
    }
    task_id_ = service_->CreateTaskWithOptions(
        actor::TaskSourceInfo(actor::TaskSourceInfo::Client::kKrypton,
                              "krypton"),
        actor::GetNullEnterprisePolicyChecker(), nullptr, gate_.GetWeakPtr(),
        &ui_, std::nullopt,
        actor::ActorKeyedService::AllowedSchemes::kRequireHttpsOrHttp);
    auto *task = service_->GetTask(task_id_);
    if (!task) {
      return false;
    }
    task_state_observation_ = service_->AddTaskStateChangedCallback(
        base::BindRepeating(&AgentSession::OnActorTaskStateChanged,
                            weak_factory_.GetWeakPtr()));
    task->AddActorSurface(actor::ActorSurfaceHandle::From(tab),
                          /*stop_task_on_detach=*/true, std::move(callback));
    return true;
  }

  void OnActorTaskStateChanged(actor::ActorTask &task) {
    if (task.id() == task_id_ && service_ && !service_->GetTask(task_id_)) {
      SetActorTabBound(false);
    }
  }

  void SetActorTabBound(bool bound) {
    if (std::exchange(actor_tab_bound_, bound) == bound) {
      return;
    }
    // Task completion can happen during tab detachment. Publish once the tab
    // model has finished handling that change.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](tabs::TabHandle handle, SessionID window_id) {
                         auto *tab = handle.Get();
                         auto *window =
                             tab ? tab->GetBrowserWindowInterface() : nullptr;
                         if (window && window->GetSessionID() == window_id) {
                           window->GetTabStripModel()->NotifyTabChanged(
                               tab, TabChangeType::kAll);
                         }
                       },
                       PageTab(), window_));
  }

  void WebContentsDestroyed() override { Stop(); }

  void OnPermissionRequestManagerDestructed() override {
    permissions_observation_.Reset();
  }

  void OnPromptAdded() override { PauseForNativePrompt(); }

  bool PauseForNativePrompt() {
    if (handed_off_ || !PageInScope()) {
      return false;
    }
    AbortCall("handoff_required", "The user has control.");
    BeginHandoff("take_over", {}, {});
    return true;
  }

  void OnRequestsFinalized() override {
    if (!pending_handoff_) {
      return;
    }
    auto decision = std::move(*pending_handoff_);
    pending_handoff_.reset();
    OnHandoffDecision(std::move(decision.request_id), std::move(decision.reply),
                      decision.done);
  }

  void DidFinishNavigation(content::NavigationHandle *navigation) override {
    if (!navigation->IsInPrimaryMainFrame() || !navigation->HasCommitted()) {
      return;
    }
    if (standalone_) {
      if (navigation->GetURL() != GURL(kAgentNewTabUrl)) {
        Stop();
      }
      return;
    }
    if (active_work_tab_) {
      const auto origin =
          web_contents()->GetPrimaryMainFrame()->GetLastCommittedOrigin();
      if (origin != shared_origin_ ||
          !ConfirmationGate::IsWebNavigation(navigation->GetURL())) {
        shared_site_revoked_ = true;
      }
      return;
    }
    OnPageNavigation(*navigation);
  }

private:
  class WorkTab final : public content::WebContentsObserver {
  public:
    WorkTab(AgentSession &session, tabs::TabInterface &tab)
        : content::WebContentsObserver(tab.GetContents()),
          handle(tab.GetHandle()),
          id(base::UnguessableToken::Create().ToString()), session_(session) {
      detach_ = tab.RegisterWillDetach(base::BindRepeating(
          [](WorkTab *self, tabs::TabInterface *,
             tabs::TabInterface::DetachReason reason) {
            if (reason == tabs::TabInterface::DetachReason::kDelete) {
              self->session_->OnWorkTabClosed(*self);
            }
            self->session_->OnWorkTabLost(*self);
          },
          base::Unretained(this)));
      discard_ = tab.RegisterWillDiscardContents(base::BindRepeating(
          [](WorkTab *self, tabs::TabInterface *, content::WebContents *,
             content::WebContents *) { self->session_->OnWorkTabLost(*self); },
          base::Unretained(this)));
    }

    void DidFinishNavigation(content::NavigationHandle *navigation) override {
      if (!navigation->IsInPrimaryMainFrame() || !navigation->HasCommitted()) {
        return;
      }
      if (session_->active_work_tab_ == this) {
        session_->OnPageNavigation(*navigation);
      } else if (!navigation->IsSameDocument()) {
        session_->OnWorkTabLost(*this);
      }
    }

    void BeforeUnloadFired(bool proceed) override {
      if (!proceed && session_->closing_work_tab_ == id) {
        session_->CompleteWorkTabClose("Closing the work tab was cancelled.",
                                       /*error=*/true);
      }
    }

    void WebContentsDestroyed() override {
      session_->OnWorkTabClosed(*this);
      session_->OnWorkTabLost(*this);
    }
    content::WebContents *contents() const { return web_contents(); }
    void Revoke() {
      detach_ = {};
      discard_ = {};
      Observe(nullptr);
    }

    const tabs::TabHandle handle;
    const std::string id;

  private:
    const raw_ref<AgentSession> session_;
    base::CallbackListSubscription detach_;
    base::CallbackListSubscription discard_;
  };

  void OnPageNavigation(content::NavigationHandle &navigation) {
    page_.reset();
    if (site_revoked_) {
      return;
    }
    const url::Origin current = CurrentOrigin();
    const bool web_navigation =
        ConfirmationGate::IsWebNavigation(navigation.GetURL());
    const bool authorized_navigation =
        web_navigation && navigation_grant_ &&
        navigation_grant_->navigation_id == navigation.GetNavigationId() &&
        navigation_grant_->origin == current;
    if (authorized_navigation && !navigation.IsErrorPage()) {
      filter_.ClearPlaceholders();
      UpdatePageOrigin(current);
      return;
    }
    if (web_navigation && current == origin_) {
      return;
    }
    filter_.ClearPlaceholders();
    if (!web_navigation || current.opaque() ||
        !net::registry_controlled_domains::SameDomainOrHost(
            current, origin_,
            net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)) {
      site_revoked_ = true;
      pending_handoff_.reset();
      constexpr std::string_view reason =
          "The browser tab moved to another site without agent approval.";
      handed_off_ = false;
      if (IsPageTool(active_tool_)) {
        AbortCall("origin_boundary", reason);
      }
      FailQueuedCalls(reason);
      if (active_work_tab_) {
        auto *revoked = active_work_tab_.get();
        ReleasePage();
        revoked->Revoke();
        work_tab_lost_ = true;
      } else {
        shared_site_revoked_ = true;
        StopActorTask();
      }
      Notify("session.origin_boundary", reason);
      return;
    }
    if (!handed_off_ && !authorized_navigation) {
      AbortCall("page_changed", "The shared page changed.");
    }
    UpdatePageOrigin(current);
  }

  void UpdatePageOrigin(const url::Origin &origin) {
    origin_ = origin;
    if (!active_work_tab_ && !standalone_) {
      shared_origin_ = origin;
    }
  }

  struct NavigationGrant {
    GURL url;
    url::Origin origin;
    std::optional<int64_t> navigation_id;
    std::optional<int> history_entry_id = std::nullopt;
  };

  Profile &SharedProfile() const {
    return *Profile::FromBrowserContext(web_contents()->GetBrowserContext());
  }

  NavigationPermission GetNavigationMode() const {
    return GetNavigationPermission(*SharedProfile().GetPrefs());
  }

  bool OnAgentNavigation(content::NavigationHandle &navigation,
                         bool redirected) {
    const GURL &url = navigation.GetURL();
    if (!PageInScope() || navigation.GetWebContents() != PageContents() ||
        handed_off_ || GetNavigationMode() == NavigationPermission::kBlock ||
        !navigation.IsInPrimaryMainFrame() || navigation.IsPost() ||
        !ConfirmationGate::IsWebNavigation(url)) {
      return false;
    }
    const auto destination = url::Origin::Create(url);
    if (navigation_grant_) {
      auto &grant = *navigation_grant_;
      if (grant.navigation_id == navigation.GetNavigationId()) {
        return destination == grant.origin;
      }
      if (grant.history_entry_id) {
        const auto *entry = navigation.GetNavigationEntry();
        if (!grant.navigation_id && !redirected &&
            !navigation.IsRendererInitiated() && navigation.IsHistory() &&
            !navigation.IsSameDocument() && entry &&
            entry->GetUniqueID() == *grant.history_entry_id &&
            url == grant.url && destination == grant.origin) {
          grant.navigation_id = navigation.GetNavigationId();
          return true;
        }
        return false;
      }
      if (!grant.navigation_id && !redirected &&
          !navigation.IsRendererInitiated() &&
          ui::PageTransitionCoreTypeIs(navigation.GetPageTransition(),
                                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL) &&
          destination == grant.origin &&
          (grant.url.is_empty() || url == grant.url)) {
        // An empty URL represents the browser-generated default search URL.
        grant.navigation_id = navigation.GetNavigationId();
        return true;
      }
    }
    return destination == origin_;
  }

  void OnTabBound(actor::mojom::ActionResultPtr result) {
    if (!result || !actor::IsOk(*result) || !PageInScope()) {
      const std::string reason =
          result && !actor::IsOk(*result)
              ? DescribeActionFailure(result->code)
              : "The browser could not attach the shared tab.";
      LOG(WARNING) << "Krypton Agent could not attach the shared tab: "
                   << reason;
      Notify("session.error", reason);
      Stop();
      return;
    }
    SetActorTabBound(true);
    OpenToolPipe();
  }

  void OpenToolPipe() {
    auto handler = base::BindRepeating(&AgentSession::HandleMessage,
                                       weak_factory_.GetWeakPtr());
    if (options_) {
      const auto *pin = PinnedAgentComponent();
      if (!pin || !sandbox_) {
        Stop();
        return;
      }
      const bool unsandboxed = pin->unsigned_development &&
                               options_->unsandboxed_development_provider;
      pipe_ = ToolPipe::StartVerified(
          std::move(handler), component_path_, *pin,
          unsandboxed || !sandbox_->sid
              ? std::nullopt
              : std::make_optional(sandbox_->sid->Clone()),
          base::BindOnce(&AgentSession::OnEndpoint, weak_factory_.GetWeakPtr()),
          pin->unsigned_development
              ? base::BindRepeating(&AgentSession::OnRelayDecision,
                                    weak_factory_.GetWeakPtr())
              : ToolPipe::ReportDecision(),
          sandbox_->isolation == ProviderIsolation::kJob);
    } else {
      pipe_ = ToolPipe::Start(std::move(handler));
    }
    if (!pipe_) {
      Stop();
      return;
    }
    ResetIdleTimer();
    if (!standalone_ && !BindPageGates(*web_contents())) {
      Stop();
    }
  }

  bool BindPageGates(content::WebContents &contents) {
    prompt_gate_bound_ = BindBrowserPromptGate(
        contents,
        base::BindRepeating(
            [](base::WeakPtr<AgentSession> session) {
              return session && session->PauseForNativePrompt();
            },
            weak_factory_.GetWeakPtr()),
        base::BindRepeating(
            [](base::WeakPtr<AgentSession> session,
               content::NavigationHandle &navigation, bool redirected) {
              return session &&
                     session->OnAgentNavigation(navigation, redirected);
            },
            weak_factory_.GetWeakPtr()));
    if (!prompt_gate_bound_) {
      return false;
    }
    if (auto *permissions =
            permissions::PermissionRequestManager::FromWebContents(&contents)) {
      permissions_observation_.Observe(permissions);
      if (permissions->IsRequestInProgress()) {
        OnPromptAdded();
      }
    }
    return true;
  }

  void
  OnComponent(base::expected<base::FilePath, AgentComponent::Error> result) {
    if (!result || !IsProductionAgentEnabled() || !service_ ||
        !web_contents()) {
      Notify("session.error", "The verified AI component is unavailable.");
      Stop();
      return;
    }
    component_path_ = std::move(*result);
    auto *os_crypt = g_browser_process->os_crypt_async();
    if (!os_crypt) {
      Stop();
      return;
    }
    os_crypt->GetInstance(base::BindOnce(&AgentSession::OnHistoryEncryptor,
                                         weak_factory_.GetWeakPtr()));
  }

  void OnHistoryEncryptor(scoped_refptr<os_crypt_async::Encryptor> encryptor) {
    if (!IsProductionAgentEnabled() || !IsBoundToSharedTab()) {
      Stop();
      return;
    }
    const bool resume = !options_->resume_session_id.empty();
    session_id_ = resume ? options_->resume_session_id
                         : base::UnguessableToken::Create().ToString();
    const bool scheduled = HistoryTaskRunner()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(resume ? &ResumeHistorySession : &CreateHistorySession,
                       profile_path_, std::move(encryptor),
                       NewHistorySession{session_id_, options_->provider,
                                         options_->model, space_id_,
                                         options_->prompt}),
        base::BindOnce(&AgentSession::OnHistoryCreated,
                       weak_factory_.GetWeakPtr()));
    if (!scheduled) {
      Stop();
    }
  }

  void OnHistoryCreated(std::optional<HistoryContext> context) {
    if (!context || !IsProductionAgentEnabled() || !IsBoundToSharedTab()) {
      Notify("session.error", "The browser could not save the session key.");
      Stop();
      return;
    }
    history_context_ = std::move(context);
    Emit(base::DictValue()
             .Set("event", "session.identity")
             .Set("data", base::DictValue().Set("session_id", session_id_)));
    if (history_context_->instruction_like) {
      gate_.FlagPageInstructions();
      history_flag_saved_ = true;
    }
    RescheduleHistoryMaintenance(profile_path_);
    if (!base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE, {base::MayBlock()},
            base::BindOnce(&PrepareProviderSandbox, options_->approved_program,
                           history_context_->profile_token, options_->provider,
                           options_->isolation, nullptr),
            base::BindOnce(&AgentSession::OnSandbox,
                           weak_factory_.GetWeakPtr()))) {
      Stop();
    }
  }

  void OnSandbox(std::optional<ProviderSandbox> sandbox) {
    if (!sandbox || !IsProductionAgentEnabled() || !IsBoundToSharedTab()) {
      Notify("session.error", "The selected provider sandbox is unavailable.");
      Stop();
      return;
    }
    sandbox_ = std::move(sandbox);
    if (standalone_) {
      OpenToolPipe();
    } else {
      Start();
    }
  }

  void OnEndpoint(ToolPipe::Endpoint endpoint) {
    if (endpoint.name.empty() || !IsProductionAgentEnabled() || !InScope()) {
      Stop();
      return;
    }
    const auto *pin = PinnedAgentComponent();
    if (!pin || !sandbox_) {
      Stop();
      return;
    }
    endpoint_ = std::move(endpoint);
    sidecar_ = SidecarProcess::Start(
        component_path_, *pin, std::move(*sandbox_),
        base::BindRepeating(&AgentSession::OnSidecarMessage,
                            weak_factory_.GetWeakPtr()),
        base::BindOnce(&AgentSession::Stop, weak_factory_.GetWeakPtr()),
        base::BindOnce(&AgentSession::OnSidecarReady,
                       weak_factory_.GetWeakPtr()));
    sandbox_.reset();
    if (!sidecar_) {
      Stop();
    }
  }

  void OnRelayDecision(ToolPipe::ClientDecision decision) {
    if (!hello_) {
      if (pending_relay_decisions_.size() < 64) {
        pending_relay_decisions_.push_back(decision);
      }
      return;
    }
    if (!sidecar_) {
      return;
    }
    const char *name = nullptr;
    switch (decision) {
    case ToolPipe::ClientDecision::kAccepted:
      name = "accepted";
      break;
    case ToolPipe::ClientDecision::kWrongAppContainerSid:
      name = "wrong_app_container_sid";
      break;
    case ToolPipe::ClientDecision::kWrongImage:
      name = "wrong_image";
      break;
    case ToolPipe::ClientDecision::kFailedSignatureCheck:
      name = "failed_signature_check";
      break;
    case ToolPipe::ClientDecision::kBadToken:
      name = "bad_token";
      break;
    }
    sidecar_->Send(base::DictValue()
                       .Set("v", 1)
                       .Set("type", "event")
                       .Set("event", "relay.decision")
                       .Set("data", base::DictValue().Set("decision", name)));
  }

  bool SendSidecar(std::string_view method, base::DictValue params) {
    if (!sidecar_) {
      return false;
    }
    const auto id = base::StrCat({"b-", base::NumberToString(++message_id_)});
    return sidecar_->Send(base::DictValue()
                              .Set("v", 1)
                              .Set("type", "request")
                              .Set("id", id)
                              .Set("method", method)
                              .Set("params", std::move(params)));
  }

  void
  OnTranscriptEncryptor(int sequence, int cursor,
                        scoped_refptr<os_crypt_async::Encryptor> encryptor) {
    if (!transcript_reply_ || sequence != transcript_sequence_) {
      return;
    }
    if (!HistoryTaskRunner()->PostTaskAndReplyWithResult(
            FROM_HERE,
            base::BindOnce(&PrepareTranscriptRequest, profile_path_,
                           std::move(encryptor), transcript_session_, cursor),
            base::BindOnce(&AgentSession::OnTranscriptPrepared,
                           weak_factory_.GetWeakPtr(), sequence))) {
      CancelSavedTranscript();
    }
  }

  void OnTranscriptPrepared(int sequence,
                            std::optional<TranscriptRequest> request) {
    if (!transcript_reply_ || sequence != transcript_sequence_) {
      return;
    }
    if (!request || !IsProductionAgentEnabled() ||
        !SendSidecar("session.open", std::move(request->params))) {
      CancelSavedTranscript();
      return;
    }
    transcript_message_ =
        base::StrCat({"b-", base::NumberToString(message_id_)});
  }

  void CancelSavedTranscript() { FinishSavedTranscript(std::nullopt); }

  void FinishSavedTranscript(std::optional<base::DictValue> page) {
    transcript_timeout_.Stop();
    transcript_session_.clear();
    transcript_message_.clear();
    if (transcript_reply_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(transcript_reply_), std::move(page)));
    }
  }

  void RecordHistoryActivity(
      std::optional<base::DictValue> provider_session = std::nullopt) {
    if (session_id_.empty() || profile_path_.empty()) {
      return;
    }
    HistoryTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(base::IgnoreResult(&UpdateHistorySession), profile_path_,
                       session_id_, gate_.requires_confirmation(),
                       std::move(provider_session)));
  }

  void OnSidecarReady(base::DictValue authorization) {
    const auto *pin = PinnedAgentComponent();
    if (!pin || !history_context_ || !InScope() ||
        !SendSidecar(
            "hello",
            base::DictValue()
                .Set("protocol_version", pin->protocol)
                .Set("profile_token", history_context_->profile_token)
                .Set("history", std::move(history_context_->history))
                .Set("credentials", std::move(history_context_->credentials))
                .Set("launch_authorization", std::move(authorization))
                // Testing mode: the locally pinned component keeps a session
                // log of its traffic in the agent folder.
                .Set("diagnostics", pin->unsigned_development))) {
      Stop();
      return;
    }
    handshake_.Start(
        FROM_HERE, base::Seconds(15),
        base::BindOnce(&AgentSession::Stop, weak_factory_.GetWeakPtr()));
  }

  void OnSidecarMessage(base::DictValue message) {
    if (!IsBoundToSharedTab() || !IsProductionAgentEnabled()) {
      Stop();
      return;
    }
    const auto *type = message.FindString("type");
    if (!type) {
      Stop();
      return;
    }
    if (!history_revocation_.empty()) {
      const auto *id = message.FindString("id");
      if (*type == "response" && id && *id == history_revocation_) {
        Stop();
      }
      return;
    }
    if (!hello_) {
      const auto *id = message.FindString("id");
      const auto *result = message.FindDict("result");
      const auto *pin = PinnedAgentComponent();
      const auto *version =
          result ? result->FindString("sidecar_version") : nullptr;
      if (*type != "response" || !id || *id != "b-1" || !result || !pin ||
          result->FindInt("protocol_version") != pin->protocol || !version ||
          *version != pin->version || result->FindBool("history_v1") != true ||
          !history_context_) {
        Notify("session.error", "The AI component protocol did not match.");
        Stop();
        return;
      }
      hello_ = true;
      for (auto decision : std::exchange(pending_relay_decisions_, {})) {
        OnRelayDecision(decision);
      }
      source_history_ = result->FindBool("source_history_v1").value_or(false);
      preview_diagnostics_ =
          result->FindBool("preview_diagnostics_v1").value_or(false);
      base::ListValue relay;
      relay.Append(component_path_.AsUTF8Unsafe());
      relay.Append("mcp");
      relay.Append("--pipe");
      relay.Append(endpoint_.name);
      relay.Append("--token-file");
      relay.Append(endpoint_.token_file.AsUTF8Unsafe());
      base::ListValue tool_names;
      for (const auto &item : AvailableTools()) {
        if (const auto *name = item.GetDict().FindString("name")) {
          tool_names.Append(*name);
        }
      }
      auto params =
          base::DictValue()
              .Set("session_id", session_id_)
              .Set("provider", options_->provider)
              .Set("model", options_->model)
              .Set("key", std::move(history_context_->session_key))
              .Set("options",
                   base::DictValue().Set("citation_instructions",
                                         "Cite claims with the source IDs from "
                                         "page readings, for example [S3]."))
              .Set("relay", std::move(relay))
              .Set("tools", std::move(tool_names));
      if (!options_->reasoning.empty()) {
        params.Set("reasoning", base::DictValue()
                                    .Set("option", options_->reasoning_option)
                                    .Set("value", options_->reasoning));
      }
      const bool resume = !options_->resume_session_id.empty();
      if (!resume) {
        context_tab_ = std::move(options_->context_tab);
        params.Set("prompt", context_tab_
                                 ? base::StrCat({options_->prompt,
                                                 kContextTabInstruction})
                                 : options_->prompt);
        params.Set("persist", true);
      }
      if (!SendSidecar(resume ? "session.resume" : "session.start",
                       std::move(params))) {
        Stop();
      }
      history_context_.reset();
      return;
    }
    const auto *id = message.FindString("id");
    if (transcript_reply_ && !transcript_message_.empty() &&
        *type == "response" && id && *id == transcript_message_) {
      FinishSavedTranscript(ExtractTranscriptPage(std::move(message)));
      return;
    }
    if (!session_ready_ && *type == "response" && id && *id == "b-2") {
      if (!message.FindDict("result")) {
        Stop();
        return;
      }
      session_ready_ = true;
      handshake_.Stop();
      if (options_->resume_session_id.empty()) {
        AddChatPrompt(options_->prompt);
        if (chat_card_) {
          chat_card_->SetWorking(true);
        }
        Notify("session.state", "running");
      } else {
        Notify("session.state", "idle");
      }
      return;
    }
    if (session_ready_ && *type == "event") {
      const auto *event = message.FindString("event");
      const auto *data = message.FindDict("data");
      if (!event || !data) {
        return;
      }
      const auto *session = data->FindString("session_id");
      if (!session || *session != session_id_) {
        return;
      }
      if (*event == "session.recoverable") {
        RecordHistoryActivity(data->Clone());
        Notify("session.recoverable", "This session can be reopened.");
        return;
      }
      if (*event == "session.done") {
        FailQueuedCalls("The agent turn ended before this tool could run.");
        context_tab_.reset();
        if (active_tool_ == "read_context_tab") {
          AbortCall("turn_finished",
                    "The message using this attachment ended.");
        }
        FinishChatReply();
        RecordHistoryActivity();
        if (chat_card_) {
          chat_card_->SetWorking(false);
        }
      }
      if (*event == "session.item") {
        AddChatItem(*data);
        return;
      }
      if (*event == "session.error") {
        AddChatNotice(Field(*data, "message"));
      }
      if (*event == "session.state" && chat_card_) {
        chat_card_->SetWorking(Field(*data, "state") == "running");
      }
      if (*event == "session.approval_request") {
        const auto *request = data->FindString("request_id");
        if (!request || request->empty() ||
            !SendSidecar("approval.respond", base::DictValue()
                                                 .Set("session_id", session_id_)
                                                 .Set("request_id", *request)
                                                 .Set("decision", "deny"))) {
          Stop();
          return;
        }
        Notify("session.tool_activity", "A provider approval was denied.");
        return;
      }
      if (*event == "session.delta") {
        if (const auto *text = data->FindString("text")) {
          ExtendChatReply(*text);
        }
      }
      if (*event == "session.delta" || *event == "session.state" ||
          *event == "session.error" || *event == "session.done" ||
          *event == "session.tool_activity") {
        events_.Run(std::move(message));
      }
    }
  }

  void Emit(base::DictValue message) {
    if (events_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(events_, std::move(message)));
    }
  }

  void Notify(std::string_view event, std::string_view text) {
    Emit(base::DictValue()
             .Set("v", 1)
             .Set("type", "event")
             .Set("event", event)
             .Set("data", base::DictValue().Set("message", text)));
  }

  void ToolActivity(std::string_view id, std::string_view tool,
                    std::string_view state) {
    Emit(base::DictValue()
             .Set("event", "browser.tool_activity")
             .Set("data", base::DictValue()
                              .Set("id", id)
                              .Set("tool", tool)
                              .Set("state", state)));
  }

  bool CanCall(std::string_view name) const {
    if (name == "krypton_help") {
      return true;
    }
    if (name == "krypton_list_work_tabs" || name == "krypton_close_work_tab") {
      return !options_ || options_->allow_actions || options_->allow_research;
    }
    if (name == "read_context_tab") {
      return options_.has_value();
    }
    if (name == "html_render" || name == "html_preview" ||
        name == "html_guide") {
      return chat_card_ != nullptr;
    }
    if (const auto *operation = Organization::Find(name)) {
      return options_ && options_->allow_tab_list &&
             (operation->name == "krypton_list_tabs" ||
              operation->name == "krypton_list_spaces" ||
              operation->name == "krypton_list_essentials" ||
              operation->name == "krypton_list_folders");
    }
    if (name == "perform_search") {
      return false;
    }
    if (IsResearchTool(name) || name == "open_url") {
      return !options_ || options_->allow_research;
    }
    return !options_ || options_->allow_actions || name == "read_page" ||
           name == "read_element" || name == "request_handoff";
  }

  base::ListValue AvailableTools() const {
    auto tools = ListTools(service_->tool_registry());
    tools.EraseIf([this](const base::Value &item) {
      const auto *name = item.GetDict().FindString("name");
      return !name || !CanCall(*name);
    });
    for (auto &item : tools) {
      auto &tool = item.GetDict();
      const auto name = Field(tool, "name");
      if (!IsPageTool(name) || name == "request_handoff") {
        continue;
      }
      auto &schema = *tool.FindDict("inputSchema");
      auto *properties = schema.FindDict("properties");
      if (!properties) {
        schema.Set("properties", base::DictValue());
        properties = schema.FindDict("properties");
      }
      properties->Set("tab",
                      base::DictValue()
                          .Set("type", "string")
                          .Set("description",
                               "A work-tab ID from krypton_list_work_tabs."));
      if (standalone_) {
        std::string description(Field(tool, "description"));
        if (description.contains("the shared tab")) {
          base::ReplaceSubstringsAfterOffset(&description, 0, "the shared tab",
                                             "the agent's work tab");
        } else {
          description += " This tool uses the agent's work tab.";
        }
        tool.Set("description", std::move(description));
      }
    }
    return tools;
  }

  void ResetIdleTimer() {
    idle_.Start(
        FROM_HERE, base::Minutes(10),
        base::BindOnce(&AgentSession::Stop, weak_factory_.GetWeakPtr()));
  }

  url::Origin CurrentOrigin() const {
    if (auto *contents = PageContents();
        contents && contents->GetPrimaryMainFrame()) {
      return contents->GetPrimaryMainFrame()->GetLastCommittedOrigin();
    }
    return url::Origin();
  }

  bool InScope() const {
    return IsBoundToSharedTab() && history_revocation_.empty();
  }

  content::WebContents *PageContents() const {
    if (active_work_tab_) {
      return active_work_tab_->contents();
    }
    return standalone_ ? nullptr : web_contents();
  }

  tabs::TabHandle PageTab() const {
    return active_work_tab_ ? active_work_tab_->handle : tab_;
  }

  std::string_view PageToolTabId() const {
    if (active_work_tab_) {
      return active_work_tab_->id;
    }
    return standalone_ ? std::string_view() : "shared";
  }

  bool WorkTabInWindow(const WorkTab &work) const {
    const auto *tab = work.handle.Get();
    auto *shared = tab_.Get();
    return tab && shared && work.contents() &&
           tab->GetContents() == work.contents() &&
           tab->GetBrowserWindowInterface() &&
           tab->GetBrowserWindowInterface() ==
               shared->GetBrowserWindowInterface() &&
           tab->GetProfile() == &SharedProfile();
  }

  bool PageInScope() const {
    if (!InScope() || site_revoked_ || !service_->GetTask(task_id_)) {
      return false;
    }
    const auto *contents = PageContents();
    if (!contents || CurrentOrigin() != origin_) {
      return false;
    }
    if (!active_work_tab_) {
      return !standalone_ && !shared_site_revoked_ &&
             ConfirmationGate::IsWebNavigation(contents->GetLastCommittedURL());
    }
    return WorkTabInWindow(*active_work_tab_) &&
           (ConfirmationGate::IsWebNavigation(
                contents->GetLastCommittedURL()) ||
            active_work_tab_->contents()
                ->GetController()
                .IsInitialBlankNavigation());
  }

  void SuggestPrompt(std::string text) {
    if (!InScope() || !options_ || !options_->suggest_prompt ||
        text.size() > 4 * kMaxPromptChars || !PromptFits(text)) {
      return;
    }
    auto cleaned = base::UTF8ToUTF16(text);
    std::erase_if(cleaned, [](char16_t c) {
      return base::IsUnicodeControl(c) || (c >= 0x202a && c <= 0x202e) ||
             (c >= 0x2066 && c <= 0x2069);
    });
    base::TrimWhitespace(cleaned, base::TRIM_ALL, &cleaned);
    if (!cleaned.empty()) {
      options_->suggest_prompt.Run(base::UTF16ToUTF8(cleaned));
    }
  }

  bool IsBoundToSharedTab() const {
    auto *tab = tab_.Get();
    return web_contents() && service_ && tab &&
           tab->GetContents() == web_contents() &&
           tab->GetBrowserWindowInterface() &&
           tab->GetBrowserWindowInterface()->GetSessionID() == window_ &&
           (!standalone_ ||
            web_contents()->GetLastCommittedURL() == GURL(kAgentNewTabUrl));
  }

  void Stop() {
    const bool was_running = !stopped();
    FailQueuedCalls("The browser task ended.");
    AbortCall("cancelled", "The browser task ended.");
    CancelSavedTranscript();
    idle_.Stop();
    pending_handoff_.reset();
    weak_factory_.InvalidateWeakPtrs();
    ReleasePage();
    work_tabs_.clear();
    handshake_.Stop();
    component_.Cancel();
    sidecar_.reset();
    pipe_.reset();
    history_context_.reset();
    sandbox_.reset();
    FinishChatReply();
    ResetCall();
    page_.reset();
    filter_.Clear();
    context_tab_.reset();
    context_filter_.Clear();
    research_cache_.clear();
    research_cache_aliases_.clear();
    research_links_.clear();
    research_sign_in_walls_.clear();
    research_sign_in_grants_.clear();
    research_output_.clear();
    if (!research_partition_.empty() && web_contents()) {
      auto &profile = SharedProfile();
      auto partition = content::StoragePartitionConfig::Create(
          &profile, research_partition_, "research", true);
      if (auto *storage =
              profile.GetStoragePartition(partition, /*can_create=*/false)) {
        storage->ClearData(content::StoragePartition::REMOVE_DATA_MASK_ALL,
                           blink::StorageKey(), base::Time(), base::Time::Max(),
                           base::DoNothing());
        storage->GetNetworkContext()->ClearHttpCache(
            base::Time(), base::Time::Max(), nullptr, base::DoNothing());
      }
      research_partition_.clear();
    }
    Observe(nullptr);
    if (was_running) {
      Notify("session.state", "stopped");
    }
    if (!profile_path_.empty()) {
      RescheduleHistoryMaintenance(profile_path_);
    }
  }

  void StopActorTask() {
    SetActorTabBound(false);
    task_state_observation_ = {};
    const auto task = std::exchange(task_id_, actor::TaskId());
    if (service_ && !task.is_null() && service_->GetTask(task)) {
      service_->StopTask(task, actor::ActorTask::StoppedReason::kStoppedByUser);
    }
  }

  void ReleasePage() {
    permissions_observation_.Reset();
    if (prompt_gate_bound_) {
      if (auto *contents = PageContents()) {
        UnbindBrowserPromptGate(*contents);
      }
      prompt_gate_bound_ = false;
    }
    StopActorTask();
    active_work_tab_ = nullptr;
    page_.reset();
    filter_.ClearPlaceholders();
  }

  void OnWorkTabLost(WorkTab &work) {
    if (closing_work_tab_ == work.id) {
      CompleteWorkTabClose("The work tab left the session before it closed.",
                           /*error=*/true);
    }
    if (active_work_tab_ != &work) {
      work.Revoke();
      return;
    }
    work_tab_lost_ = true;
    pending_handoff_.reset();
    const bool review_pending = handed_off_ || IsPageTool(active_tool_);
    handed_off_ = false;
    constexpr std::string_view reason =
        "The work tab left this session's scope. Open a new work tab to "
        "continue.";
    if (IsPageTool(active_tool_)) {
      AbortCall("out_of_scope", reason);
    }
    ReleasePage();
    work.Revoke();
    FailQueuedCalls(reason);
    if (review_pending) {
      Notify("session.origin_boundary", reason);
    }
  }

  void ReviewChatLink(const GURL &url) {
    if (chat_link_pending_ || !options_ || !options_->review ||
        !IsBoundToSharedTab() || !IsProductionAgentEnabled()) {
      return;
    }
    chat_link_pending_ = true;
    options_->review.Run(base::DictValue()
                             .Set("action", "open_agent_link")
                             .Set("url", url.spec()),
                         base::BindPostTaskToCurrentDefault(
                             base::BindOnce(&AgentSession::OnChatLinkReviewed,
                                            weak_factory_.GetWeakPtr(), url)));
  }

  void OnChatLinkReviewed(const GURL &url, bool allowed) {
    chat_link_pending_ = false;
    if (!allowed || !IsBoundToSharedTab() || !IsProductionAgentEnabled()) {
      return;
    }
    NavigateParams params(tab_.Get()->GetBrowserWindowInterface(), url,
                          ui::PAGE_TRANSITION_LINK);
    params.disposition = WindowOpenDisposition::kNewForegroundTab;
    Navigate(&params);
  }

  void AddChatPrompt(std::string_view text) {
    if (!chat_card_) {
      return;
    }
    FinishChatReply();
    const auto id =
        base::StrCat({"user-", base::NumberToString(++chat_sequence_)});
    chat_card_->Append(id, text);
    chat_card_->Finish(id);
  }

  void ExtendChatReply(std::string_view text) {
    if (!chat_card_) {
      return;
    }
    chat_reply_.append(text);
    if (chat_reply_id_.empty()) {
      chat_reply_id_ =
          base::StrCat({"assistant-", base::NumberToString(++chat_sequence_)});
      chat_card_->Append(chat_reply_id_, chat_reply_);
    } else {
      chat_card_->Update(chat_reply_id_, chat_reply_);
    }
  }

  // Thinking, provider tools, approvals and notices from the sidecar. They
  // belong to the current reply, which starts empty when they come first.
  void AddChatItem(const base::DictValue &data) {
    static constexpr std::string_view kKinds[] = {
        "reasoning",  "command", "file_change", "file_search",
        "web_search", "tool",    "subagent",    "todo",
        "plan",       "notice",  "error",       "approval"};
    static constexpr std::string_view kStatuses[] = {
        "running",     "waiting",  "completed", "failed",
        "interrupted", "declined", "approved"};
    const auto *id = data.FindString("item_id");
    const auto kind = Field(data, "kind");
    const auto status = Field(data, "status");
    const auto offset = data.FindInt("offset").value_or(0);
    if (!chat_card_ || !id || id->size() != 32 ||
        !std::ranges::all_of(*id, base::IsHexDigit<char>) ||
        std::ranges::find(kKinds, kind) == std::end(kKinds) ||
        std::ranges::find(kStatuses, status) == std::end(kStatuses) ||
        offset < 0) {
      return;
    }
    if (chat_reply_id_.empty()) {
      chat_reply_id_ =
          base::StrCat({"assistant-", base::NumberToString(++chat_sequence_)});
      chat_card_->Append(chat_reply_id_, chat_reply_);
    }
    chat_card_->SetItem(
        chat_reply_id_,
        TranscriptEntry{.id = *id,
                        .kind = std::string(kind),
                        .status = std::string(status),
                        .title = std::string(Field(data, "title")),
                        .detail = std::string(Field(data, "detail")),
                        .text = std::string(Field(data, "text")),
                        .output = std::string(Field(data, "output")),
                        .offset = static_cast<uint32_t>(offset)});
  }

  void AddChatNotice(std::string_view text) {
    if (!chat_card_ || text.empty()) {
      return;
    }
    FinishChatReply();
    const auto id =
        base::StrCat({"error-", base::NumberToString(++chat_sequence_)});
    chat_card_->Append(id, text);
    chat_card_->Finish(id);
    chat_card_->SetWorking(false);
  }

  void FinishChatReply() {
    if (chat_card_ && !chat_reply_id_.empty()) {
      auto sources = sources_.ForAnswer(chat_reply_);
      if (source_history_ && !sources.empty()) {
        base::ListValue records;
        for (const auto &source : sources) {
          const auto &source_origin = url::Origin::Create(source.destination);
          records.Append(
              base::DictValue()
                  .Set("id", source.id)
                  .Set("title", filter_.Field(source.title, source_origin))
                  .Set("site", source.site)
                  .Set("display_url",
                       filter_.Url(source.destination, source_origin))
                  .Set("favicon_png", base::Base64Encode(source.favicon_png))
                  .Set("cited", source.cited));
        }
        SendSidecar("history.append",
                    base::DictValue()
                        .Set("session_id", session_id_)
                        .Set("kind", "message.sources")
                        .Set("data", base::DictValue()
                                         .Set("message_id", chat_reply_id_)
                                         .Set("sources", std::move(records))));
      }
      chat_card_->SetSources(chat_reply_id_, std::move(sources));
      chat_card_->Finish(chat_reply_id_);
      chat_reply_id_.clear();
      chat_reply_.clear();
    }
  }

  void RecordCall(std::string_view id, std::string_view call_id,
                  ToolPipe::Reply &reply) {
    in_flight_ = std::string(call_id);
    active_request_ = id;
    active_reply_ = base::MakeRefCounted<base::RefCountedData<ToolPipe::Reply>>(
        std::move(reply));
    reply = base::BindOnce(
        [](scoped_refptr<base::RefCountedData<ToolPipe::Reply>> shared,
           base::DictValue response) {
          if (shared->data) {
            std::move(shared->data).Run(std::move(response));
          }
        },
        active_reply_);
  }

  void ResetCall() {
    research_batch_.reset();
    if (chat_card_ && active_tool_ == "html_preview") {
      chat_card_->CancelPreview();
    }
    capture_activity_.RunAndReset();
    capture_timeout_.Stop();
    work_tab_close_timeout_.Stop();
    closing_work_tab_.clear();
    work_tab_close_reply_.Reset();
    navigation_grant_.reset();
    in_flight_.reset();
    active_request_.clear();
    active_tool_.clear();
    active_reply_.reset();
    ScheduleNextCall();
  }

  void AbortCall(std::string_view code, std::string_view message) {
    if (!active_tool_.empty()) {
      ToolActivity(active_request_, active_tool_, "failed");
    }
    if (active_reply_ && active_reply_->data) {
      auto response = ToolResult(active_request_, std::string(message), true);
      response.FindDict("result")->Set("structuredContent",
                                       base::DictValue().Set("code", code));
      std::move(active_reply_->data).Run(std::move(response));
    }
    ResetCall();
  }

  void BeginHandoff(std::string_view reason, std::string_view id,
                    ToolPipe::Reply reply) {
    handed_off_ = true;
    navigation_grant_.reset();
    if (auto *task = service_ ? service_->GetTask(task_id_) : nullptr;
        task && task->IsUnderActorControl()) {
      task->Pause(/*from_actor=*/true);
    }
    page_.reset();
    filter_.ClearPlaceholders();
    if (!options_ || !options_->handoff) {
      handed_off_ = false;
      if (auto *task = service_ ? service_->GetTask(task_id_) : nullptr;
          task && task->IsUnderUserControl()) {
        task->Resume();
      }
      ResetCall();
      if (reply) {
        std::move(reply).Run(
            ProtocolError(id, "handoff_required", "The user must take over."));
      }
      return;
    }
    auto decision = base::BindPostTaskToCurrentDefault(base::BindOnce(
        &AgentSession::OnHandoffDecision, weak_factory_.GetWeakPtr(),
        std::string(id), std::move(reply)));
    options_->handoff.Run(std::string(reason), std::move(decision));
  }

  void OnHandoffDecision(std::string id, ToolPipe::Reply reply, bool done) {
    if (!InScope() || !handed_off_) {
      return;
    }
    auto *permissions = permissions_observation_.GetSource();
    if (permissions && permissions->IsRequestInProgress()) {
      pending_handoff_ = HandoffDecision{std::move(id), std::move(reply), done};
      return;
    }
    if (auto *task = service_ ? service_->GetTask(task_id_) : nullptr;
        task && task->IsUnderUserControl() && PageInScope()) {
      task->Resume();
    }
    handed_off_ = false;
    ScheduleNextCall();
    Notify("session.handoff_result", done ? "completed" : "declined");
    const std::string_view outcome = done ? "completed" : "declined";
    if (reply) {
      Finish(std::move(reply), id, std::string(outcome), false);
    } else {
      SendSidecar("session.handoff_result", base::DictValue()
                                                .Set("session_id", session_id_)
                                                .Set("outcome", outcome));
    }
  }

  struct QueuedCall {
    std::string id;
    std::string call_id;
    base::DictValue params;
    ToolPipe::Reply reply;
  };

  void FailQueuedCalls(std::string_view reason) {
    auto calls = std::exchange(queued_calls_, {});
    for (auto &call : calls) {
      std::move(call.reply).Run(ToolResult(call.id, std::string(reason), true));
    }
  }

  void ScheduleNextCall() {
    if (call_dispatch_pending_ || in_flight_ || handed_off_ ||
        queued_calls_.empty()) {
      return;
    }
    call_dispatch_pending_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&AgentSession::RunNextCall, weak_factory_.GetWeakPtr()));
  }

  void RunNextCall() {
    call_dispatch_pending_ = false;
    if (in_flight_ || handed_off_ || queued_calls_.empty()) {
      return;
    }
    if (!InScope()) {
      Stop();
      return;
    }
    auto call = std::move(queued_calls_.front());
    queued_calls_.pop_front();
    DispatchCall(std::move(call));
    ScheduleNextCall();
  }

  void CancelCall(std::string_view id, std::string_view call_id,
                  ToolPipe::Reply reply) {
    const auto queued =
        std::ranges::find(queued_calls_, call_id, &QueuedCall::call_id);
    if (queued != queued_calls_.end()) {
      auto cancelled = std::move(*queued);
      queued_calls_.erase(queued);
      std::move(cancelled.reply)
          .Run(ToolResult(cancelled.id,
                          "The queued browser call was cancelled.", true));
    } else if (in_flight_ == call_id) {
      const bool page_call =
          IsPageTool(active_tool_) && active_tool_ != "request_handoff";
      AbortCall("cancelled", "The browser action was cancelled.");
      if (page_call && !handed_off_) {
        FailQueuedCalls(
            "Page access ended when the browser action was cancelled.");
        if (active_work_tab_) {
          OnWorkTabLost(*active_work_tab_);
        } else {
          shared_site_revoked_ = true;
          StopActorTask();
        }
      }
    } else {
      std::move(reply).Run(ProtocolError(
          id, "unknown_call", "No matching browser call is pending."));
      return;
    }
    std::move(reply).Run(Envelope(id).Set("result", base::DictValue()));
  }

  void HandleMessage(base::DictValue message, ToolPipe::Reply reply) {
    const std::string id = *message.FindString("id");
    const auto &method = *message.FindString("method");
    if (!InScope()) {
      std::move(reply).Run(
          ToolResult(id, "The agent session is unavailable.", true));
      if (history_revocation_.empty()) {
        Stop();
      }
      return;
    }
    if (method == "tools.list") {
      std::move(reply).Run(Envelope(id).Set(
          "result", base::DictValue().Set("tools", AvailableTools())));
      return;
    }
    auto *params = message.FindDict("params");
    const auto *call_id = params ? params->FindString("call_id") : nullptr;
    if (!call_id || call_id->empty() || call_id->size() > 128) {
      std::move(reply).Run(
          ToolResult(id, "A valid call ID is required.", true));
      return;
    }
    if (method == "tools.cancel") {
      CancelCall(id, *call_id, std::move(reply));
      return;
    }
    if (method != "tools.call") {
      std::move(reply).Run(
          ProtocolError(id, "unknown_method", "Unknown pipe method."));
      return;
    }
    if (in_flight_ == *call_id ||
        std::ranges::find(queued_calls_, *call_id, &QueuedCall::call_id) !=
            queued_calls_.end()) {
      std::move(reply).Run(
          ToolResult(id, "This call ID is already pending.", true));
      return;
    }
    queued_calls_.push_back(
        {id, *call_id, std::move(*params), std::move(reply)});
    ScheduleNextCall();
  }

  void DispatchCall(QueuedCall queued) {
    const auto &id = queued.id;
    const auto *call_id = &queued.call_id;
    auto reply = std::move(queued.reply);
    const auto *name = queued.params.FindString("name");
    auto *arguments = queued.params.FindDict("arguments");
    if (!name || !CanCall(*name)) {
      std::move(reply).Run(ToolResult(
          id, "This tool is unavailable with the current session permissions.",
          true));
      return;
    }
    if (!arguments) {
      std::move(reply).Run(
          ToolResult(id, "Tool arguments must be an object.", true));
      return;
    }
    std::optional<GURL> link_destination;
    if (IsUrlTool(*name)) {
      const auto *destination = arguments->FindString("url");
      if (*name == "open_url" && destination) {
        const auto link = research_links_.find(*destination);
        if (link != research_links_.end()) {
          link_destination = link->second;
        }
      }
      if (!link_destination &&
          (!destination ||
           !ConfirmationGate::IsWebNavigation(GURL(*destination)))) {
        std::move(reply).Run(ToolResult(
            id,
            *name == "open_url"
                ? "Use an HTTP or HTTPS address without credentials, or "
                  "a link ID issued in this chat."
                : "Only valid HTTP and HTTPS addresses without "
                  "credentials can be opened.",
            true));
        return;
      }
    }
    std::string requested_tab;
    if (IsPageTool(*name) && *name != "request_handoff" &&
        arguments->contains("tab")) {
      const auto *tab = arguments->FindString("tab");
      if (!tab || tab->empty()) {
        std::move(reply).Run(
            ToolResult(id, "A valid work-tab ID is required.", true));
        return;
      }
      requested_tab = *tab;
      arguments->Remove("tab");
    }
    if (!ValidArguments(service_->tool_registry(), *name, *arguments)) {
      std::move(reply).Run(ToolResult(
          id, "The arguments do not match this tool's input schema.", true));
      return;
    }
    if ((IsNavigation(*name) || IsResearchTool(*name)) &&
        GetNavigationMode() == NavigationPermission::kBlock) {
      std::move(reply).Run(
          ToolResult(id, "Agent navigation is blocked in Settings.", true));
      return;
    }
    RecordCall(id, *call_id, reply);
    active_tool_ = *name;
    active_denied_ = false;
    ToolActivity(id, active_tool_, "running");
    if (IsResearchTool(*name)) {
      RunResearch(*name, *arguments, id, std::move(reply));
      return;
    }
    if (*name == "request_handoff") {
      BeginHandoff(*arguments->FindString("reason"), id, std::move(reply));
      return;
    }
    if (*name == "krypton_list_work_tabs") {
      ListWorkTabs(std::move(reply), id);
      return;
    }
    if (*name == "krypton_close_work_tab") {
      CloseWorkTab(*arguments->FindString("tab"), std::move(reply), id);
      return;
    }
    if (*name == "krypton_help") {
      const auto guide = GetKryptonHelp(Field(*arguments, "topic"));
      Finish(std::move(reply), id, std::string(guide), guide.empty());
      return;
    }
    if (*name == "html_guide") {
      auto guide = GetHtmlGuide(Field(*arguments, "topic"));
      if (!guide) {
        Finish(std::move(reply), id, "The HTML guide is unavailable.", true);
      } else {
        Finish(std::move(reply), id, std::move(*guide), false);
      }
      return;
    }
    if (FindKryptonTool(*name) == KryptonToolId::kReadContextTab) {
      ReadContextTab(id, *call_id, std::move(reply));
      return;
    }
    if (*name == "html_render" || *name == "html_preview") {
      RunHtmlTool(*name, *arguments, id, std::move(reply));
      return;
    }
    if (Organization::Find(*name)) {
      RunOrganization(Organization::Find(*name), id, *call_id,
                      arguments->Clone(), std::move(reply), false);
      return;
    }
    PendingCall call{.request_id = id,
                     .call_id = *call_id,
                     .name = *name,
                     .arguments = arguments->Clone(),
                     .reply = std::move(reply)};
    if (link_destination) {
      call.arguments.Set("url", link_destination->spec());
      call.link_destination = true;
    }
    call.open_new_work_tab =
        *name == "open_url" && arguments->FindBool("new_tab").value_or(false);
    SelectPageForCall(std::move(call), requested_tab);
  }

  struct ResearchResult {
    url::Origin origin;
    std::string source;
    std::vector<std::string> rows;
  };

  struct ResearchItem {
    GURL url;
    bool from_link = false;
    bool signed_in = false;
    std::optional<ResearchResult> result;
    std::unique_ptr<ResearchLoad> load;
    bool reading_pdf = false;
  };

  struct ResearchBatch {
    std::string id;
    std::string call_id;
    ToolPipe::Reply reply;
    bool search = false;
    bool use_sign_in = false;
    size_t next = 0;
    size_t active = 0;
    bool awaiting_review = false;
    std::vector<ResearchItem> items;
  };

  using ResearchKey = std::pair<bool, GURL>;

  void RunResearch(std::string_view name, const base::DictValue &arguments,
                   std::string_view id, ToolPipe::Reply reply) {
    if (const auto cursor = arguments.FindInt("continuation")) {
      if (research_continuation_ != name ||
          static_cast<size_t>(*cursor) != research_offset_ ||
          research_offset_ >= research_output_.size()) {
        Finish(std::move(reply), id, "Research continuation is unavailable.",
               true);
        return;
      }
      SendResearchRows(std::move(reply), id);
      return;
    }
    research_output_.clear();
    research_offset_ = 0;
    research_continuation_ = name;
    auto batch = std::make_unique<ResearchBatch>();
    batch->id = id;
    batch->call_id = *in_flight_;
    batch->reply = std::move(reply);
    batch->search = name == "web_search";
    batch->use_sign_in = arguments.FindBool("use_sign_in").value_or(false);
    auto *engine =
        batch->search
            ? TemplateURLServiceFactory::GetForProfile(&SharedProfile())
            : nullptr;
    const auto *provider =
        engine ? engine->GetDefaultSearchProvider() : nullptr;
    for (const auto &input :
         *arguments.FindList(batch->search ? "queries" : "targets")) {
      ResearchItem item;
      const auto &text = input.GetString();
      if (batch->search) {
        if (provider && provider->url_ref().SupportsReplacement(
                            engine->search_terms_data())) {
          item.url = provider->GenerateSearchURL(engine->search_terms_data(),
                                                 base::UTF8ToUTF16(text));
        }
      } else if (auto found = research_links_.find(text);
                 found != research_links_.end()) {
        item.url = found->second;
        item.from_link = true;
      } else {
        item.url = GURL(text);
      }
      batch->items.push_back(std::move(item));
    }
    if (research_partition_.empty()) {
      research_partition_ =
          base::StrCat({"krypton-agent-research-",
                        base::UnguessableToken::Create().ToString()});
    }
    research_batch_ = std::move(batch);
    DispatchResearch();
  }

  bool IsCurrentResearch(std::string_view call_id) const {
    return research_batch_ && in_flight_ == call_id && InScope() &&
           !handed_off_;
  }

  void ResearchFailure(ResearchItem &item, std::string_view reason) {
    item.result.emplace();
    item.result->rows.emplace_back(reason);
  }

  void DispatchResearch() {
    auto &batch = *research_batch_;
    // Keep simultaneous background renderers small. All items in the request
    // use this pool and retain their original order in the response.
    constexpr size_t kParallelLoads = 3;
    while (!batch.awaiting_review && batch.active < kParallelLoads &&
           batch.next < batch.items.size()) {
      const size_t index = batch.next++;
      auto &item = batch.items[index];
      if (!ConfirmationGate::IsWebNavigation(item.url)) {
        ResearchFailure(item, batch.search
                                  ? "The default search engine is unavailable."
                                  : "Only valid HTTP and HTTPS addresses or "
                                    "link IDs from this chat can be read.");
        continue;
      }
      const auto mode = GetNavigationMode();
      if (mode == NavigationPermission::kBlock) {
        ResearchFailure(item, "Agent navigation is blocked in Settings.");
        continue;
      }
      PendingCall call;
      call.name = batch.search ? "web_search" : "read_url";
      call.arguments.Set("url", item.url.spec());
      call.link_destination = item.from_link;
      auto details = DataFlowReview(call, item.url);
      const bool data_flow = details.has_value();
      const bool review_navigation = mode == NavigationPermission::kAsk &&
                                     !IsAllowlisted(SharedProfile(), item.url);
      if (data_flow || review_navigation || gate_.requires_confirmation()) {
        if (!options_ || !options_->review) {
          ResearchFailure(item, "Background navigation requires approval.");
          continue;
        }
        if (!details) {
          details = base::DictValue()
                        .Set("tool", call.name)
                        .Set("arguments", std::move(call.arguments));
        }
        details->Set("navigation", true);
        details->Set("can_always_allow", review_navigation && !data_flow &&
                                             !gate_.requires_confirmation());
        batch.awaiting_review = true;
        options_->review.Run(
            std::move(*details),
            base::BindPostTaskToCurrentDefault(base::BindOnce(
                &AgentSession::OnResearchNavigationReview,
                weak_factory_.GetWeakPtr(), batch.call_id, index)));
        return;
      }
      PrepareResearchLoad(index);
    }
    if (!batch.awaiting_review && batch.active == 0 &&
        batch.next == batch.items.size()) {
      CompleteResearch();
    }
  }

  void OnResearchNavigationReview(std::string call_id, size_t index,
                                  bool approved) {
    if (!IsCurrentResearch(call_id)) {
      return;
    }
    research_batch_->awaiting_review = false;
    if (approved && GetNavigationMode() != NavigationPermission::kBlock) {
      PrepareResearchLoad(index);
    } else {
      ResearchFailure(research_batch_->items[index],
                      "Background navigation was denied.");
    }
    DispatchResearch();
  }

  void PrepareResearchLoad(size_t index) {
    auto &batch = *research_batch_;
    auto &item = batch.items[index];
    if (batch.use_sign_in) {
      if (!item.url.SchemeIs("https") ||
          !research_sign_in_walls_.contains(item.url)) {
        ResearchFailure(item,
                        "Read this HTTPS address without sign-in first. "
                        "The browser only asks after it finds a sign-in wall.");
        return;
      }
      const auto site = ResearchSite(item.url);
      if (!research_sign_in_grants_.contains(site)) {
        if (!options_ || !options_->review_sign_in) {
          ResearchFailure(item, "Reading with sign-in requires native review.");
          return;
        }
        batch.awaiting_review = true;
        options_->review_sign_in.Run(
            site, base::BindPostTaskToCurrentDefault(base::BindOnce(
                      &AgentSession::OnResearchSignInReview,
                      weak_factory_.GetWeakPtr(), batch.call_id, index)));
        return;
      }
      item.signed_in = true;
    } else {
      const ResearchKey address(batch.search, item.url);
      const auto alias = research_cache_aliases_.find(address);
      if (alias != research_cache_aliases_.end()) {
        if (auto entry = research_cache_.find({batch.search, alias->second});
            entry != research_cache_.end()) {
          item.result = entry->second;
          sources_.Reuse(item.result->source);
          return;
        }
      }
    }
    StartResearchLoad(index);
  }

  void OnResearchSignInReview(std::string call_id, size_t index,
                              SignInDecision decision) {
    if (!IsCurrentResearch(call_id)) {
      return;
    }
    research_batch_->awaiting_review = false;
    auto &item = research_batch_->items[index];
    if (decision == SignInDecision::kDeny ||
        GetNavigationMode() == NavigationPermission::kBlock) {
      ResearchFailure(item, "Reading with sign-in was denied.");
    } else {
      if (decision == SignInDecision::kAllowInChat) {
        research_sign_in_grants_.insert(ResearchSite(item.url));
      }
      item.signed_in = true;
      StartResearchLoad(index);
    }
    DispatchResearch();
  }

  bool CheckResearchRedirect(const GURL &destination) {
    if (!research_batch_ || !InScope() || handed_off_ ||
        GetNavigationMode() == NavigationPermission::kBlock ||
        gate_.requires_confirmation()) {
      return false;
    }
    PendingCall call;
    call.name = active_tool_;
    call.arguments.Set("url", destination.spec());
    return !DataFlowReview(call, destination);
  }

  void StartResearchLoad(size_t index) {
    auto &batch = *research_batch_;
    auto &item = batch.items[index];
    ++batch.active;
    item.load = std::make_unique<ResearchLoad>(
        SharedProfile(), *service_, item.url, research_partition_,
        item.signed_in,
        base::BindRepeating(
            [](base::WeakPtr<AgentSession> session, const GURL &destination) {
              return session && session->CheckResearchRedirect(destination);
            },
            weak_factory_.GetWeakPtr()),
        base::BindOnce(&AgentSession::OnResearchLoaded,
                       weak_factory_.GetWeakPtr(), batch.call_id, index));
    item.load->Start();
  }

  void FinishResearchItem(size_t index) {
    auto &batch = *research_batch_;
    batch.items[index].load.reset();
    --batch.active;
    DispatchResearch();
  }

  void OnResearchLoaded(std::string call_id, size_t index,
                        ResearchLoad::Result result) {
    if (!IsCurrentResearch(call_id)) {
      return;
    }
    auto &item = research_batch_->items[index];
    auto *contents = item.load->web_contents();
    using Result = ResearchLoad::Result;
    if (result == Result::kLoaded && contents) {
      if (contents->GetContentsMimeType() == "application/pdf") {
        ReadResearchPdf(std::move(call_id), index, *contents);
        return;
      }
      ReadPage(*contents,
               base::BindPostTaskToCurrentDefault(base::BindOnce(
                   &AgentSession::OnResearchPageRead,
                   weak_factory_.GetWeakPtr(), std::move(call_id), index)));
      return;
    }
    switch (result) {
    case Result::kSignIn:
      research_sign_in_walls_.insert(item.url);
      ResearchFailure(item, "The site requires sign-in.");
      break;
    case Result::kBlocked:
      ResearchFailure(item, "The background redirect could not be approved. "
                            "Open the address in a work tab to continue.");
      break;
    case Result::kTimeout:
      ResearchFailure(item, "The background page did not finish loading.");
      break;
    case Result::kLoaded:
    case Result::kFailed:
      ResearchFailure(item, "The background page could not be loaded. "
                            "The site may be blocked or unavailable.");
      break;
    }
    FinishResearchItem(index);
  }

  void
  OnResearchPageRead(std::string call_id, size_t index,
                     optimization_guide::AIPageContentResultOrError result) {
    if (!IsCurrentResearch(call_id)) {
      return;
    }
    auto &batch = *research_batch_;
    auto &item = batch.items[index];
    auto *contents = item.load->web_contents();
    if (result && contents) {
      const auto address = contents->GetLastCommittedURL();
      const auto source_origin = url::Origin::Create(address);
      auto page =
          ExtractOutline(result->proto, filter_, source_origin,
                         SiteHasUserContent(address), PageReadMode::kResearch);
      if (page.instruction_like) {
        gate_.FlagPageInstructions();
      }
      if (page.user_content && options_ &&
          warned_user_content_.insert(address).second) {
        Notify("session.user_content",
               "This page has content written by other people. It may "
               "contain hidden instructions.");
      }
      if (page.handoff == HandoffReason::kSignIn) {
        research_sign_in_walls_.insert(item.url);
        ResearchFailure(item, "The site requires sign-in.");
      } else if (page.sensitive_page || page.handoff != HandoffReason::kNone) {
        ResearchFailure(item, "This page contains a sensitive prompt or a "
                              "verification check. Open it in a work tab "
                              "if you want to handle the prompt.");
      } else {
        ResearchResult output;
        output.origin = source_origin;
        if (!batch.search) {
          output.source = sources_.Record(*contents);
        }
        FormatResearchPage(batch.search, address, page, output);
        SaveResearchResult(index, address, std::move(output));
      }
    } else {
      ResearchFailure(item, "No readable content was available.");
    }
    FinishResearchItem(index);
  }

  void ReadResearchPdf(std::string call_id, size_t index,
                       content::WebContents &contents) {
    auto &item = research_batch_->items[index];
    if (research_batch_->search) {
      ResearchFailure(item, "The search engine returned a PDF.");
      FinishResearchItem(index);
      return;
    }
#if BUILDFLAG(ENABLE_PDF)
    auto *helper = pdf::PDFDocumentHelper::MaybeGetForFullPagePdf(contents);
    if (helper) {
      item.reading_pdf = true;
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&AgentSession::OnResearchPdfTimeout,
                         weak_factory_.GetWeakPtr(), call_id, index),
          base::Seconds(30));
      helper->RegisterForDocumentLoadComplete(
          base::BindPostTaskToCurrentDefault(base::BindOnce(
              &AgentSession::OnResearchPdfReady, weak_factory_.GetWeakPtr(),
              std::move(call_id), index)));
      return;
    }
#endif
    ResearchFailure(item, "The PDF reader is unavailable in this background "
                          "page. Open the PDF in a work tab to view it.");
    FinishResearchItem(index);
  }

#if BUILDFLAG(ENABLE_PDF)
  ResearchItem *FindResearchPdf(std::string_view call_id, size_t index) {
    if (!IsCurrentResearch(call_id)) {
      return nullptr;
    }
    auto &item = research_batch_->items[index];
    return item.reading_pdf ? &item : nullptr;
  }

  void OnResearchPdfTimeout(std::string call_id, size_t index) {
    if (auto *item = FindResearchPdf(call_id, index)) {
      item->reading_pdf = false;
      ResearchFailure(*item, "The background PDF reader did not finish.");
      FinishResearchItem(index);
    }
  }

  pdf::PDFDocumentHelper *ResearchPdfHelper(ResearchItem &item) {
    auto *contents = item.load->web_contents();
    return contents ? pdf::PDFDocumentHelper::MaybeGetForFullPagePdf(*contents)
                    : nullptr;
  }

  void OnResearchPdfReady(std::string call_id, size_t index) {
    if (auto *item = FindResearchPdf(call_id, index)) {
      if (auto *helper = ResearchPdfHelper(*item)) {
        helper->HasMeaningfulText(base::BindPostTaskToCurrentDefault(
            base::BindOnce(&AgentSession::OnResearchPdfHasText,
                           weak_factory_.GetWeakPtr(), std::move(call_id),
                           index)));
      }
    }
  }

  void OnResearchPdfHasText(std::string call_id, size_t index, bool has_text) {
    auto *item = FindResearchPdf(call_id, index);
    if (!item) {
      return;
    }
    if (!has_text) {
      item->reading_pdf = false;
      ResearchFailure(*item, "This PDF has no text layer.");
      FinishResearchItem(index);
      return;
    }
    if (auto *helper = ResearchPdfHelper(*item)) {
      helper->GetPdfBytes(
          /*size_limit=*/0,
          base::BindPostTaskToCurrentDefault(base::BindOnce(
              &AgentSession::OnResearchPdfPageCount, weak_factory_.GetWeakPtr(),
              std::move(call_id), index)));
    }
  }

  void OnResearchPdfPageCount(std::string call_id, size_t index,
                              pdf::mojom::PdfListener::GetPdfBytesStatus status,
                              const std::vector<uint8_t> & /*bytes*/,
                              uint32_t count) {
    auto *item = FindResearchPdf(call_id, index);
    if (!item) {
      return;
    }
    auto *contents = item->load->web_contents();
    if (!contents || !count || count > INT32_MAX ||
        status == pdf::mojom::PdfListener::GetPdfBytesStatus::kFailed) {
      item->reading_pdf = false;
      ResearchFailure(*item, "The PDF page count could not be read.");
      FinishResearchItem(index);
      return;
    }
    ResearchResult output;
    output.origin = url::Origin::Create(contents->GetLastCommittedURL());
    output.source = sources_.Record(*contents);
    ReadNextResearchPdfPage(std::move(call_id), index, 0,
                            static_cast<int32_t>(count), std::move(output));
  }

  void ReadNextResearchPdfPage(std::string call_id, size_t index, int32_t page,
                               int32_t count, ResearchResult output) {
    auto *item = FindResearchPdf(call_id, index);
    if (!item) {
      return;
    }
    if (page == count && item->load->web_contents()) {
      item->reading_pdf = false;
      SaveResearchResult(index,
                         item->load->web_contents()->GetLastCommittedURL(),
                         std::move(output));
      FinishResearchItem(index);
      return;
    }
    if (auto *helper = ResearchPdfHelper(*item)) {
      helper->GetPageText(
          page,
          base::BindPostTaskToCurrentDefault(base::BindOnce(
              &AgentSession::OnResearchPdfText, weak_factory_.GetWeakPtr(),
              std::move(call_id), index, page, count, std::move(output))));
    }
  }

  void OnResearchPdfText(std::string call_id, size_t index, int32_t page,
                         int32_t count, ResearchResult output,
                         const std::u16string &content) {
    if (!FindResearchPdf(call_id, index)) {
      return;
    }
    output.rows.push_back(
        base::StrCat({"## Page ", base::NumberToString(page + 1)}));
    std::u16string_view remaining(content);
    constexpr size_t kTextChunk = 2048;
    while (!remaining.empty()) {
      size_t length = std::min(remaining.size(), kTextChunk);
      if (length < remaining.size()) {
        const size_t newline = remaining.rfind(u'\n', length);
        if (newline != std::u16string_view::npos && newline >= length / 2) {
          length = newline + 1;
        } else if (remaining[length] >= 0xDC00 && remaining[length] <= 0xDFFF) {
          --length;
        }
      }
      const auto text = base::UTF16ToUTF8(remaining.substr(0, length));
      remaining.remove_prefix(length);
      if (PrivacyFilter::LooksLikeInstruction(text)) {
        gate_.FlagPageInstructions();
      }
      output.rows.push_back(MarkdownText(filter_.Text(text, output.origin)));
    }
    ReadNextResearchPdfPage(std::move(call_id), index, page + 1, count,
                            std::move(output));
  }
#endif

  void FormatResearchPage(bool search, const GURL &address,
                          const PageOutline &page, ResearchResult &output) {
    std::set<GURL> seen;
    bool after_result = false;
    bool in_table = false;
    bool table_heading = false;
    std::vector<std::string> cells;
    auto flush_cells = [&] {
      if (cells.empty()) {
        return;
      }
      std::string line = "|";
      for (const auto &cell : cells) {
        base::StrAppend(&line, {" ", cell, " |"});
      }
      output.rows.push_back(std::move(line));
      if (!table_heading) {
        std::string separator = "|";
        for (size_t column = 0; column < cells.size(); ++column) {
          separator += " --- |";
        }
        output.rows.push_back(std::move(separator));
        table_heading = true;
      }
      cells.clear();
    };
    for (std::string_view row : page.rows) {
      const bool user_content =
          row.starts_with("<user_content>") && row.ends_with("</user_content>");
      if (user_content) {
        row.remove_prefix(std::string_view("<user_content>").size());
        row.remove_suffix(std::string_view("</user_content>").size());
      }
      auto fields = base::JSONReader::ReadDict(row, base::JSON_PARSE_RFC);
      if (!fields) {
        continue;
      }
      const auto role = Field(*fields, "role");
      if (!search &&
          (role == "CONTENT_ATTRIBUTE_TABLE" || role == "table_end")) {
        flush_cells();
        in_table = role == "CONTENT_ATTRIBUTE_TABLE";
        table_heading = false;
        continue;
      }
      if (in_table && role == "CONTENT_ATTRIBUTE_TABLE_ROW") {
        flush_cells();
        continue;
      }
      if (in_table && role == "CONTENT_ATTRIBUTE_TABLE_CELL") {
        cells.emplace_back();
      }
      std::string line;
      if (auto link_index = fields->FindInt("research_link")) {
        after_result = false;
        const auto &link = page.links.at(*link_index);
        const GURL destination =
            search ? SearchResultDestination(address, link.url) : link.url;
        if (!ConfirmationGate::IsWebNavigation(destination) ||
            (search && (!link.heading || !seen.insert(destination).second))) {
          continue;
        }
        auto link_id = base::StrCat(
            {"L", base::NumberToString(research_links_.size() + 1)});
        research_links_.emplace(link_id, destination);
        line = base::StrCat({"- [", link_id, "] ", MarkdownText(link.title),
                             " — ", filter_.Url(destination, output.origin)});
        after_result = true;
      } else {
        auto text = Field(*fields, "text");
        if (text.empty()) {
          text = Field(*fields, "label");
        }
        if (text.empty() || (search && !after_result)) {
          continue;
        }
        if (role == "CONTENT_ATTRIBUTE_HEADING") {
          if (search) {
            after_result = false;
            continue;
          }
          line = "## ";
        } else if (role == "CONTENT_ATTRIBUTE_LIST_ITEM") {
          line = "- ";
        } else if (!search && fields->FindBool("code").value_or(false)) {
          line = "    ";
        }
        line += MarkdownText(text);
      }
      if (user_content) {
        line = base::StrCat({"<user_content>", line, "</user_content>"});
      }
      if (in_table && !cells.empty()) {
        if (!cells.back().empty()) {
          cells.back().push_back(' ');
        }
        cells.back() += line;
      } else {
        output.rows.push_back(std::move(line));
      }
    }
    flush_cells();
    if (output.rows.empty()) {
      output.rows.emplace_back(
          search ? "No readable organic results were found. The engine may "
                   "require consent or verification in a visible tab."
                 : "The page did not expose readable article text.");
    }
    if (page.truncated) {
      output.rows.emplace_back(
          "The page exceeded the extraction safety limit.");
    }
  }

  void SaveResearchResult(size_t index, const GURL &address,
                          ResearchResult output) {
    auto &batch = *research_batch_;
    auto &item = batch.items[index];
    if (!item.signed_in) {
      research_cache_aliases_.insert_or_assign({batch.search, item.url},
                                               address);
      research_cache_.insert_or_assign({batch.search, address}, output);
    }
    item.result = std::move(output);
  }

  void CompleteResearch() {
    auto batch = std::move(research_batch_);
    for (size_t index = 0; index < batch->items.size(); ++index) {
      auto &result = *batch->items[index].result;
      research_output_.push_back(
          base::StrCat({"Result ", base::NumberToString(index + 1), "\n"}));
      std::string part;
      auto flush = [&] {
        research_output_.push_back(
            WrapContent(std::move(part), result.origin, {}, result.source));
        part.clear();
      };
      for (std::string_view row : result.rows) {
        constexpr size_t kChunkSize = kResponseBytes / 2;
        if (!part.empty() && part.size() + row.size() >= kChunkSize) {
          flush();
        }
        while (row.size() >= kChunkSize) {
          size_t length = kChunkSize - 1;
          while ((static_cast<unsigned char>(row[length]) & 0xC0) == 0x80) {
            --length;
          }
          part.append(row.substr(0, length));
          row.remove_prefix(length);
          flush();
        }
        base::StrAppend(&part, {row, "\n"});
      }
      if (!part.empty()) {
        flush();
      }
    }
    SendResearchRows(std::move(batch->reply), batch->id);
  }

  void SendResearchRows(ToolPipe::Reply reply, std::string_view id) {
    std::string output;
    while (research_offset_ < research_output_.size()) {
      const auto &part = research_output_[research_offset_];
      if (!output.empty() && output.size() + part.size() > kResponseBytes) {
        break;
      }
      base::StrAppend(&output, {part, "\n"});
      ++research_offset_;
    }
    const int continuation = research_offset_ < research_output_.size()
                                 ? static_cast<int>(research_offset_)
                                 : 0;
    if (!continuation) {
      research_output_.clear();
      research_offset_ = 0;
    }
    Finish(std::move(reply), id, std::move(output), false, continuation);
  }

  void RunOrganization(const Organization::Operation *operation, std::string id,
                       std::string call_id, base::DictValue arguments,
                       ToolPipe::Reply reply, bool approved) {
    if (in_flight_ != call_id || !InScope()) {
      Finish(std::move(reply), id, "Organization scope changed.", true);
      return;
    }
    auto *browser = tab_.Get()->GetBrowserWindowInterface();
    auto result =
        organization_.Run(*operation, arguments, *browser, filter_, origin_,
                          gate_.requires_confirmation(), approved);
    if (result.needs_confirmation && !approved && options_ &&
        options_->review) {
      auto details = base::DictValue()
                         .Set("tool", operation->name)
                         .Set("arguments", arguments.Clone());
      auto decision = base::BindPostTaskToCurrentDefault(base::BindOnce(
          &AgentSession::OnOrganizationReview, weak_factory_.GetWeakPtr(),
          operation, std::move(id), std::move(call_id), std::move(arguments),
          std::move(reply)));
      options_->review.Run(std::move(details), std::move(decision));
      return;
    }
    if (result.needs_confirmation) {
      ConfirmationGate::LogDenial(operation->name);
    }
    if (result.page_derived) {
      if (PrivacyFilter::LooksLikeInstruction(result.text)) {
        gate_.FlagPageInstructions();
      }
      result.text = WrapContent(std::move(result.text), origin_);
    }
    LOG(INFO) << "Krypton Agent organization call " << operation->name
              << (result.error ? " refused" : " completed");
    Finish(std::move(reply), id, std::move(result.text), result.error);
  }

  void OnOrganizationReview(const Organization::Operation *operation,
                            std::string id, std::string call_id,
                            base::DictValue arguments, ToolPipe::Reply reply,
                            bool accepted) {
    if (in_flight_ != call_id) {
      return;
    }
    if (!accepted || !IsProductionAgentEnabled()) {
      active_denied_ = true;
      ConfirmationGate::LogDenial(operation->name);
      Finish(std::move(reply), id,
             "Action requires user confirmation and was denied.", true);
      return;
    }
    RunOrganization(operation, std::move(id), std::move(call_id),
                    std::move(arguments), std::move(reply), true);
  }

  struct PendingCall {
    std::string request_id;
    std::string call_id;
    std::string name;
    base::DictValue arguments;
    ToolPipe::Reply reply;
    std::optional<actor::PinnedHistoryEntry> history = std::nullopt;
    bool open_new_work_tab = false;
    bool opening_reviewed = false;
    bool review_without_page = false;
    bool link_destination = false;
  };

  std::optional<actor::PinnedHistoryEntry>
  ResolveHistoryDestination(const PendingCall &call) const {
    auto &controller = PageContents()->GetController();
    const auto *current = controller.GetLastCommittedEntry();
    const int offset = call.name == "go_back" ? -1 : 1;
    const auto *target = controller.GetEntryAtIndex(
        controller.GetLastCommittedEntryIndex() + offset);
    if (controller.GetPendingEntry() || !current || !target ||
        target->IsInitialEntry() || target->GetHasPostData() ||
        target->GetURL() != target->GetVirtualURL() ||
        !ConfirmationGate::IsWebNavigation(target->GetURL()) ||
        current->GetMainFrameDocumentSequenceNumber() ==
            target->GetMainFrameDocumentSequenceNumber()) {
      return std::nullopt;
    }
    return actor::PinnedHistoryEntry{current->GetUniqueID(),
                                     target->GetUniqueID(), target->GetURL()};
  }

  std::optional<GURL> CallDestination(const PendingCall &call) const {
    if (call.history) {
      return call.history->url;
    }
    if (IsUrlTool(call.name)) {
      return GURL(*call.arguments.FindString("url"));
    }
    return std::nullopt;
  }

  WorkTab *FindWorkTab(std::string_view id) const {
    for (const auto &tab : work_tabs_) {
      if (tab->id == id && WorkTabInWindow(*tab)) {
        return tab.get();
      }
    }
    return nullptr;
  }

  void ListWorkTabs(ToolPipe::Reply reply, std::string_view id) {
    base::ListValue tabs;
    for (const auto &work : work_tabs_) {
      if (!WorkTabInWindow(*work)) {
        continue;
      }
      const auto &url = work->contents()->GetLastCommittedURL();
      const auto origin = url::Origin::Create(url);
      tabs.Append(
          base::DictValue()
              .Set("tab", work->id)
              .Set("current", active_work_tab_ == work.get())
              .Set("title",
                   filter_.Text(base::UTF16ToUTF8(work->contents()->GetTitle()),
                                origin))
              .Set("url", filter_.Url(url, origin)));
    }
    Finish(std::move(reply), id,
           WrapContent(base::WriteJson(tabs).value_or("[]"), url::Origin()),
           false);
  }

  bool SharedPageAvailable() const {
    return !standalone_ && !shared_site_revoked_ && InScope() &&
           ConfirmationGate::IsWebNavigation(
               web_contents()->GetLastCommittedURL()) &&
           web_contents()->GetPrimaryMainFrame()->GetLastCommittedOrigin() ==
               shared_origin_;
  }

  void CloseWorkTab(std::string_view tab_id, ToolPipe::Reply reply,
                    std::string_view id) {
    auto *work = FindWorkTab(tab_id);
    if (!work) {
      Finish(std::move(reply), id, "This work tab is no longer available.",
             true);
      return;
    }
    auto *strip = tab_.Get()->GetBrowserWindowInterface()->GetTabStripModel();
    const int index = strip->GetIndexOfWebContents(work->contents());
    if (index == TabStripModel::kNoTab) {
      Finish(std::move(reply), id, "This work tab is no longer available.",
             true);
      return;
    }
    closing_work_tab_ = work->id;
    work_tab_close_reply_ = base::BindOnce(
        [](base::WeakPtr<AgentSession> session, std::string call_id,
           ToolPipe::Reply reply, std::string id, std::string text,
           bool error) {
          if (session && session->in_flight_ == call_id) {
            session->Finish(std::move(reply), id, std::move(text), error);
          }
        },
        weak_factory_.GetWeakPtr(), *in_flight_, std::move(reply),
        std::string(id));
    work_tab_close_timeout_.Start(
        FROM_HERE, base::Seconds(8),
        base::BindOnce(&AgentSession::OnWorkTabCloseTimeout,
                       weak_factory_.GetWeakPtr()));
    strip->CloseWebContentsAt(index,
                              TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
  }

  void OnWorkTabClosed(const WorkTab &work) {
    if (closing_work_tab_ == work.id) {
      CompleteWorkTabClose("The work tab closed.", /*error=*/false);
    }
  }

  void OnWorkTabCloseTimeout() {
    const auto *work = FindWorkTab(closing_work_tab_);
    const auto *dialog =
        javascript_dialogs::AppModalDialogQueue::GetInstance()->active_dialog();
    const bool waiting_for_user = work && dialog && dialog->view() &&
                                  dialog->is_before_unload_dialog() &&
                                  dialog->web_contents() == work->contents();
    CompleteWorkTabClose(
        waiting_for_user
            ? "The work tab has not closed. Respond to the leave-page prompt."
            : "The browser did not finish closing the work tab in time.",
        /*error=*/true);
  }

  void CompleteWorkTabClose(std::string text, bool error) {
    work_tab_close_timeout_.Stop();
    closing_work_tab_.clear();
    // A detach or unload notification can run inside the tab strip mutation.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(work_tab_close_reply_),
                                  std::move(text), error));
  }

  void SelectPageForCall(PendingCall call, std::string_view requested_tab) {
    if (!requested_tab.empty()) {
      if (call.open_new_work_tab) {
        Finish(std::move(call.reply), call.request_id,
               "Choose a current work tab or open a new one.", true);
        return;
      }
      if (requested_tab == "shared" && SharedPageAvailable()) {
        if (!active_work_tab_ && PageInScope()) {
          RunPageCall(std::move(call));
        } else {
          ActivateSharedPage(std::move(call));
        }
        return;
      }
      auto *work = FindWorkTab(requested_tab);
      if (!work) {
        Finish(std::move(call.reply), call.request_id,
               "This work tab is no longer within the session.", true);
        return;
      }
      if (active_work_tab_ == work && PageInScope()) {
        RunPageCall(std::move(call));
      } else {
        ActivateWorkTab(*work, std::move(call));
      }
      return;
    }
    if (PageInScope()) {
      RunPageCall(std::move(call));
      return;
    }
    if (!call.open_new_work_tab) {
      for (const auto &work : base::Reversed(work_tabs_)) {
        if (WorkTabInWindow(*work)) {
          ActivateWorkTab(*work, std::move(call));
          return;
        }
      }
      if (SharedPageAvailable()) {
        ActivateSharedPage(std::move(call));
        return;
      }
    }
    if (standalone_ && (call.open_new_work_tab || !work_tab_lost_)) {
      call.open_new_work_tab = false;
      OpenWorkTab(std::move(call));
      return;
    }
    if (call.open_new_work_tab) {
      call.review_without_page = true;
      ReviewPageAction(std::move(call), content::WeakDocumentPtr(),
                       PageOutline(), 0);
      return;
    }
    Finish(
        std::move(call.reply), call.request_id,
        "The page tab is unavailable. Use open_url with new_tab to continue.",
        true);
  }

  void OpenWorkTab(PendingCall call) {
    auto *chat_tab = tab_.Get();
    auto *window = chat_tab->GetBrowserWindowInterface();
    auto *strip = window->GetTabStripModel();
    const int index = strip->GetIndexOfWebContents(web_contents());
    if (index == TabStripModel::kNoTab || strip->closing_all()) {
      Finish(std::move(call.reply), call.request_id,
             "The browser could not open the work tab.", true);
      return;
    }
    gfx::Size viewport = web_contents()->GetContainerBounds().size();
    if (viewport.IsEmpty()) {
      if (auto *view = BrowserView::GetBrowserViewForBrowser(window)) {
        viewport = view->GetContentsSize();
      }
    }
    if (viewport.IsEmpty()) {
      Finish(std::move(call.reply), call.request_id,
             "The browser has no viewport for a work tab.", true);
      return;
    }
    // The initial document is empty. The actor loads the site after its
    // navigation checks are bound.
    content::WebContents::CreateParams params(window->GetProfile());
    params.initially_hidden = true;
    auto contents = content::WebContents::Create(params);
    auto weak_contents = contents->GetWeakPtr();
    strip->InsertWebContentsAt(index + 1, std::move(contents), ADD_NONE,
                               chat_tab->GetGroup());
    auto *tab =
        weak_contents
            ? tabs::TabInterface::MaybeGetFromContents(weak_contents.get())
            : nullptr;
    if (!InScope() || !tab || tab->GetBrowserWindowInterface() != window) {
      if (in_flight_ == call.call_id) {
        Finish(std::move(call.reply), call.request_id,
               "The browser could not open the work tab.", true);
      }
      return;
    }
    // InsertWebContentsAt does not size a background tab as AddTab does.
    weak_contents->Resize(gfx::Rect(viewport));
    auto &work =
        *work_tabs_.emplace_back(std::make_unique<WorkTab>(*this, *tab));
    ActivateWorkTab(work, std::move(call));
  }

  void ActivateWorkTab(WorkTab &work, PendingCall call) {
    ReleasePage();
    active_work_tab_ = &work;
    origin_ = CurrentOrigin();
    site_revoked_ = false;
    if (!BindActorTask(work.handle,
                       base::BindOnce(&AgentSession::OnWorkTabBound,
                                      weak_factory_.GetWeakPtr(),
                                      std::move(call)))) {
      OnWorkTabLost(work);
    }
  }

  void ActivateSharedPage(PendingCall call) {
    ReleasePage();
    origin_ = shared_origin_;
    site_revoked_ = false;
    if (!BindActorTask(tab_, base::BindOnce(&AgentSession::OnSharedPageRebound,
                                            weak_factory_.GetWeakPtr(),
                                            std::move(call)))) {
      AbortCall("out_of_scope", "The shared tab could not be bound.");
    }
  }

  void OnSharedPageRebound(PendingCall call,
                           actor::mojom::ActionResultPtr result) {
    if (in_flight_ != call.call_id) {
      return;
    }
    if (!result || !actor::IsOk(*result) || !PageInScope() ||
        !BindPageGates(*PageContents())) {
      ReleasePage();
      Finish(std::move(call.reply), call.request_id,
             "The shared tab could not be bound.", true);
      return;
    }
    SetActorTabBound(true);
    if (in_flight_ == call.call_id && !handed_off_) {
      RunPageCall(std::move(call));
    }
  }

  void OnWorkTabBound(PendingCall call, actor::mojom::ActionResultPtr result) {
    if (in_flight_ != call.call_id || !active_work_tab_) {
      return;
    }
    if (!result || !actor::IsOk(*result) || !PageInScope() ||
        !BindPageGates(*PageContents())) {
      OnWorkTabLost(*active_work_tab_);
      return;
    }
    SetActorTabBound(true);
    if (in_flight_ != call.call_id || handed_off_) {
      return;
    }
    if (call.opening_reviewed) {
      PerformAction(std::move(call), PageOutline(),
                    /*reviewed_navigation=*/true);
    } else {
      RunPageCall(std::move(call));
    }
  }

  void RunPageCall(PendingCall call) {
    if (!PageInScope()) {
      Finish(std::move(call.reply), call.request_id,
             "The tab is outside this session's scope.", true);
      return;
    }
    if (IsHistoryTool(call.name)) {
      call.history = ResolveHistoryDestination(call);
      if (!call.history) {
        Finish(std::move(call.reply), call.request_id,
               "There is no eligible history entry in that direction.", true);
        return;
      }
    }
    auto &contents = *PageContents();
    auto document = contents.GetPrimaryMainFrame()->GetWeakDocumentPtr();
    if (active_work_tab_ &&
        contents.GetController().IsInitialBlankNavigation()) {
      if (IsUrlTool(call.name) || call.name == "perform_search") {
        ReviewPageAction(std::move(call), std::move(document), PageOutline(),
                         /*node_id=*/0);
      } else {
        Finish(std::move(call.reply), call.request_id,
               "The work tab is empty. Open or search for a page with the "
               "enabled navigation tools, or attach a tab using the composer.",
               true);
      }
      return;
    }
    const int node = call.arguments.FindInt("dom_node_id").value_or(0);
    if (call.name != "read_element" && node &&
        (!page_ || !page_->targets.contains(node))) {
      Finish(std::move(call.reply), call.request_id,
             "Read the page before targeting a node.", true);
      return;
    }
    ReadPage(contents, base::BindOnce(&AgentSession::OnPageRead,
                                      weak_factory_.GetWeakPtr(),
                                      std::move(call), std::move(document)));
  }

  void ReadContextTab(const std::string &id, const std::string &call_id,
                      ToolPipe::Reply reply) {
    auto *contents = context_tab_
                         ? context_tab_->GetContents(
                               *tab_.Get()->GetBrowserWindowInterface())
                         : nullptr;
    if (!contents) {
      Finish(std::move(reply), id,
             "No unchanged tab is attached to this message.", true);
      return;
    }
    ReadPage(*contents, base::BindOnce(&AgentSession::OnContextTabRead,
                                       weak_factory_.GetWeakPtr(), id, call_id,
                                       *context_tab_, std::move(reply)));
  }

  void OnContextTabRead(std::string id, std::string call_id, ContextTab context,
                        ToolPipe::Reply reply,
                        optimization_guide::AIPageContentResultOrError result) {
    if (in_flight_ != call_id) {
      return;
    }
    auto *contents =
        InScope() && context_tab_
            ? context.GetContents(*tab_.Get()->GetBrowserWindowInterface())
            : nullptr;
    if (!contents || contents->GetLastCommittedURL() != context_tab_->url ||
        contents->GetPrimaryMainFrame() !=
            context_tab_->document.AsRenderFrameHostIfValid() ||
        !result) {
      Finish(std::move(reply), id,
             "The attached tab changed or its text could not be read.", true);
      return;
    }
    const auto origin = url::Origin::Create(context.url);
    const auto outline = ExtractOutline(result->proto, context_filter_, origin,
                                        SiteHasUserContent(context.url),
                                        PageReadMode::kTextOnly);
    if (outline.handoff != HandoffReason::kNone || outline.sensitive_page) {
      Finish(std::move(reply), id,
             "The attached tab contains a sensitive form or sign-in step.",
             true);
      return;
    }
    if (outline.instruction_like) {
      gate_.FlagPageInstructions();
    }
    if (outline.user_content &&
        warned_user_content_.insert(context.url).second) {
      Notify("session.user_content",
             "This page has content written by other people. It may contain "
             "hidden instructions.");
    }
    std::string text;
    bool truncated = outline.truncated;
    for (const auto &row : outline.rows) {
      if (text.size() + row.size() + 1 > kResponseBytes) {
        truncated = true;
        break;
      }
      text += row;
      text.push_back('\n');
    }
    if (truncated) {
      text += "truncated=true\n";
    }
    Finish(std::move(reply), id,
           WrapContent(std::move(text), origin, {}, sources_.Record(*contents)),
           false);
  }

  void OnPageRead(PendingCall call, content::WeakDocumentPtr document,
                  optimization_guide::AIPageContentResultOrError result) {
    if (in_flight_ != call.call_id) {
      return;
    }
    if (!PageInScope() || document.AsRenderFrameHostIfValid() !=
                              PageContents()->GetPrimaryMainFrame()) {
      Finish(std::move(call.reply), call.request_id, "The shared page changed.",
             true);
      return;
    }
    if (!result) {
      Finish(std::move(call.reply), call.request_id, "Page extraction failed.",
             true);
      return;
    }
    PageOutline fresh = ReadOutline(result->proto);
    if (fresh.instruction_like) {
      gate_.FlagPageInstructions();
    }
    if (fresh.handoff != HandoffReason::kNone) {
      BeginHandoff(HandoffName(fresh.handoff), call.request_id,
                   std::move(call.reply));
      return;
    }
    if (const auto *operation = Organization::Find(call.name)) {
      RunOrganization(operation, call.request_id, call.call_id,
                      std::move(call.arguments), std::move(call.reply), false);
      return;
    }
    if (auto *tab_data = actor::ActorTabData::From(PageTab().Get())) {
      tab_data->DidObserveContent(result->proto, actor::ApcSource::kActor);
    }
    if (FindKryptonTool(call.name) == KryptonToolId::kReadPage) {
      if (const auto continuation = call.arguments.FindInt("continuation")) {
        SendOutline(std::move(call.reply), call.request_id, *continuation);
        return;
      }
      page_ = std::move(fresh);
      SendOutline(std::move(call.reply), call.request_id, 0);
      return;
    }
    const int node_id = call.arguments.FindInt("dom_node_id").value_or(0);
    if (call.name == "read_element") {
      const auto row = fresh.element_rows.find(node_id);
      if (!page_ || !page_->element_rows.contains(node_id) ||
          row == fresh.element_rows.end() ||
          page_->document != fresh.document) {
        Finish(std::move(call.reply), call.request_id,
               "The element is unavailable. Read the page again.", true);
        return;
      }
      Finish(std::move(call.reply), call.request_id,
             WrapContent(fresh.rows[row->second], origin_, PageToolTabId(),
                         options_ ? sources_.Record(*PageContents())
                                  : std::string()),
             false);
      return;
    }
    if (!MatchesObservedTargets(call, fresh)) {
      Finish(std::move(call.reply), call.request_id,
             "A target changed or is unavailable. Read the page again.", true);
      return;
    }
    ReviewPageAction(std::move(call), std::move(document), std::move(fresh),
                     node_id);
  }

  bool MatchesObservedTargets(const PendingCall &call,
                              const PageOutline &current) const {
    for (const auto *key :
         {"dom_node_id", "from_dom_node_id", "to_dom_node_id"}) {
      const int node = call.arguments.FindInt(key).value_or(0);
      if (!node) {
        continue;
      }
      const auto target = current.targets.find(node);
      if (!page_ || target == current.targets.end() ||
          !page_->targets.contains(node) ||
          page_->targets.at(node) != target->second) {
        return false;
      }
    }
    return true;
  }

  void ReviewPageAction(PendingCall call, content::WeakDocumentPtr document,
                        PageOutline fresh, int node_id) {
    const PageTarget *target = node_id ? &fresh.targets.at(node_id) : nullptr;
    if (call.name == "capture_page" && !CanCapturePage(fresh)) {
      Finish(std::move(call.reply), call.request_id,
             "This page cannot be captured. Use read_page for filtered text.",
             true);
      return;
    }
    const bool review_action =
        (call.open_new_work_tab && !standalone_) ||
        gate_.NeedsConfirmation(call.name, call.arguments, fresh, target);
    const auto destination = CallDestination(call);
    const bool review_navigation =
        destination && GetNavigationMode() == NavigationPermission::kAsk &&
        !IsAllowlisted(SharedProfile(), *destination);
    auto details = DataFlowReview(call);
    const bool data_flow_review = details.has_value();
    if (review_action || review_navigation || data_flow_review) {
      if (!options_ || !options_->review) {
        Deny(std::move(call));
        return;
      }
      if (!details) {
        details = base::DictValue()
                      .Set("tool", call.name)
                      .Set("arguments", call.arguments.Clone());
      }
      details->Set("navigation", destination.has_value());
      if (call.name == "capture_page") {
        details->Set("title", "Share a page screenshot");
        details->Set("detail", "Share the visible page as an image with the "
                               "selected agent provider.");
        details->Set(
            "target",
            filter_.Url(PageContents()->GetLastCommittedURL(), origin_));
      }
      if (destination) {
        details->Set("url", destination->spec());
        if (call.history) {
          auto review_arguments = call.arguments.Clone();
          review_arguments.Set("url", destination->spec());
          details->Set("arguments", std::move(review_arguments));
        }
      }
      details->Set("can_always_allow",
                   review_navigation && !review_action && !data_flow_review);
      auto decision = base::BindPostTaskToCurrentDefault(base::BindOnce(
          &AgentSession::OnActionReview, weak_factory_.GetWeakPtr(),
          std::move(call), document, gate_.requires_confirmation(),
          fresh.sensitive_page, fresh.truncated, data_flow_review));
      options_->review.Run(std::move(*details), std::move(decision));
      return;
    }
    PerformAction(std::move(call), std::move(fresh),
                  /*reviewed_navigation=*/false);
  }

  std::optional<base::DictValue>
  DataFlowReview(const PendingCall &call,
                 std::optional<GURL> explicit_destination = std::nullopt) {
    bool requires_confirmation = false;
    std::optional<url::Origin> source;
    if (const auto destination = explicit_destination ? explicit_destination
                                                      : CallDestination(call)) {
      const GURL &url = *destination;
      const std::string decoded = base::UnescapeURLComponent(
          url.spec(),
          base::UnescapeRule::SPACES | base::UnescapeRule::PATH_SEPARATORS |
              base::UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS |
              base::UnescapeRule::REPLACE_PLUS_WITH_SPACE);
      source = filter_.FindTaint(decoded, url::Origin::Create(url), false);
      if (!source) {
        source =
            context_filter_.FindTaint(decoded, url::Origin::Create(url), false);
      }
      requires_confirmation =
          (!call.link_destination && HasHighEntropySegment(decoded)) ||
          filter_.taint_saturated() || context_filter_.taint_saturated();
    } else if (call.name == "perform_search" || call.name == "type" ||
               call.name == "select_option") {
      for (const auto [key, value] : call.arguments) {
        if (key != "query" && key != "text" && key != "value") {
          continue;
        }
        source = TaintInValue(filter_, value, origin_, call.name == "type");
        if (!source) {
          source = TaintInValue(context_filter_, value, origin_, false);
        }
        if (source) {
          break;
        }
      }
      requires_confirmation =
          filter_.taint_saturated() || context_filter_.taint_saturated();
    }
    if (!source && !requires_confirmation) {
      return std::nullopt;
    }
    auto details =
        base::DictValue()
            .Set("tool", call.name)
            .Set("reason", source ? "read_data" : "sensitive_destination")
            .Set("arguments", call.arguments.Clone());
    if (source) {
      details.Set("source", source->Serialize());
    }
    return details;
  }

  void OnActionReview(PendingCall call, content::WeakDocumentPtr document,
                      bool reviewed_sticky, bool reviewed_sensitive,
                      bool reviewed_truncated, bool reviewed_data_flow,
                      bool approved) {
    if (in_flight_ != call.call_id) {
      return;
    }
    // Re-extract the page after the human decision, including current form
    // state.
    if (approved && call.open_new_work_tab && call.review_without_page &&
        InScope() && GetNavigationMode() != NavigationPermission::kBlock) {
      call.open_new_work_tab = false;
      call.opening_reviewed = true;
      OpenWorkTab(std::move(call));
      return;
    }
    if (!approved || !PageInScope() ||
        document.AsRenderFrameHostIfValid() !=
            PageContents()->GetPrimaryMainFrame()) {
      Deny(std::move(call));
      return;
    }
    if (active_work_tab_ &&
        PageContents()->GetController().IsInitialBlankNavigation()) {
      CompleteReviewedAction(std::move(call), PageOutline(), reviewed_sticky,
                             reviewed_sensitive, reviewed_truncated,
                             reviewed_data_flow);
      return;
    }
    ReadPage(*PageContents(),
             base::BindOnce(
                 &AgentSession::OnReviewedPageRead, weak_factory_.GetWeakPtr(),
                 std::move(call), std::move(document), reviewed_sticky,
                 reviewed_sensitive, reviewed_truncated, reviewed_data_flow));
  }

  void
  OnReviewedPageRead(PendingCall call, content::WeakDocumentPtr document,
                     bool reviewed_sticky, bool reviewed_sensitive,
                     bool reviewed_truncated, bool reviewed_data_flow,
                     optimization_guide::AIPageContentResultOrError result) {
    if (in_flight_ != call.call_id) {
      return;
    }
    if (!PageInScope() || !result ||
        document.AsRenderFrameHostIfValid() !=
            PageContents()->GetPrimaryMainFrame()) {
      Deny(std::move(call));
      return;
    }
    CompleteReviewedAction(std::move(call), ReadOutline(result->proto),
                           reviewed_sticky, reviewed_sensitive,
                           reviewed_truncated, reviewed_data_flow);
  }

  void CompleteReviewedAction(PendingCall call, PageOutline fresh,
                              bool reviewed_sticky, bool reviewed_sensitive,
                              bool reviewed_truncated,
                              bool reviewed_data_flow) {
    if (fresh.instruction_like) {
      gate_.FlagPageInstructions();
    }
    if (fresh.handoff != HandoffReason::kNone) {
      BeginHandoff(HandoffName(fresh.handoff), call.request_id,
                   std::move(call.reply));
      return;
    }
    if (!MatchesObservedTargets(call, fresh)) {
      Deny(std::move(call));
      return;
    }
    if ((gate_.requires_confirmation() && !reviewed_sticky) ||
        (fresh.sensitive_page && !reviewed_sensitive) ||
        (fresh.truncated && !reviewed_truncated) ||
        (DataFlowReview(call) && !reviewed_data_flow)) {
      Deny(std::move(call));
      return;
    }
    PerformAction(std::move(call), std::move(fresh),
                  /*reviewed_navigation=*/true);
  }

  void PerformAction(PendingCall call, PageOutline fresh,
                     bool reviewed_navigation) {
    if (!PageInScope() || handed_off_) {
      Finish(std::move(call.reply), call.request_id,
             "The tab is outside this session's scope.", true);
      return;
    }
    if (call.name == "capture_page") {
      CapturePage(std::move(call), fresh);
      return;
    }
    if (IsNavigation(call.name) &&
        GetNavigationMode() == NavigationPermission::kBlock) {
      Finish(std::move(call.reply), call.request_id,
             "Agent navigation is blocked in Settings.", true);
      return;
    }
    if (const auto destination = CallDestination(call)) {
      const GURL &url = *destination;
      if (!reviewed_navigation &&
          GetNavigationMode() == NavigationPermission::kAsk &&
          !IsAllowlisted(SharedProfile(), url)) {
        Deny(std::move(call));
        return;
      }
      if (call.open_new_work_tab) {
        call.open_new_work_tab = false;
        call.opening_reviewed = reviewed_navigation;
        OpenWorkTab(std::move(call));
        return;
      }
      navigation_grant_.emplace(
          url, url::Origin::Create(url), std::nullopt,
          call.history ? std::make_optional(call.history->destination_id)
                       : std::nullopt);
    }
    const std::string_view field = call.name == "type" ? "text" : "value";
    if (const auto *value = call.arguments.FindString(field)) {
      auto resolved = filter_.Resolve(*value, origin_);
      if (!resolved) {
        Deny(std::move(call));
        return;
      }
      call.arguments.Set(field, std::move(*resolved));
    }
    if (call.name == "wait") {
      if (const auto *condition = call.arguments.Find("until");
          condition && fresh.ContainsVisible(*condition)) {
        page_ = std::move(fresh);
        SendOutline(std::move(call.reply), call.request_id, 0);
        return;
      }
    }
    if (call.name == "set_checked") {
      const auto &target =
          fresh.targets.at(*call.arguments.FindInt("dom_node_id"));
      if (target.control_type !=
              optimization_guide::proto::FORM_CONTROL_TYPE_INPUT_CHECKBOX ||
          !target.checked || target.disabled || target.sensitive_field) {
        Finish(std::move(call.reply), call.request_id,
               "The target must be an enabled checkbox with a known state.",
               true);
        return;
      }
      if (target.checked == call.arguments.FindBool("checked")) {
        Finish(std::move(call.reply), call.request_id,
               "The checkbox already has the requested state.", false);
        return;
      }
    }
    const auto surface = actor::ActorSurfaceHandle::From(PageTab());
    std::unique_ptr<actor::ToolRequest> request;
    if (call.name == "go_back" && call.history) {
      request = std::make_unique<actor::HistoryBackToolRequest>(
          surface, std::move(call.history));
    } else if (call.name == "go_forward" && call.history) {
      request = std::make_unique<actor::HistoryForwardToolRequest>(
          surface, std::move(call.history));
    } else {
      request = BuildRequest(call.name, call.arguments, surface, fresh);
    }
    if (!request) {
      Finish(std::move(call.reply), call.request_id,
             "Invalid action target or argument.", true);
      return;
    }
    if (call.name == "perform_search") {
      const auto origin = request->AssociatedOriginGrant();
      if (!origin || !ConfirmationGate::IsWebNavigation(origin->GetURL())) {
        Finish(std::move(call.reply), call.request_id,
               "The default search destination is unavailable.", true);
        return;
      }
      navigation_grant_.emplace(GURL(), *origin, std::nullopt);
    }
    std::vector<std::unique_ptr<actor::ToolRequest>> actions;
    actions.push_back(std::move(request));
    service_->PerformActions(
        task_id_, std::move(actions), actor::ActorTaskMetadata(),
        base::BindOnce(&AgentSession::OnActionComplete,
                       weak_factory_.GetWeakPtr(), std::move(call)));
  }

  PageOutline
  ReadOutline(const optimization_guide::proto::AnnotatedPageContent &proto) {
    const auto &url = PageContents()->GetLastCommittedURL();
    auto outline =
        ExtractOutline(proto, filter_, origin_, SiteHasUserContent(url));
    if (outline.user_content && options_ &&
        warned_user_content_.insert(url).second) {
      auto *profile = tab_.Get()->GetBrowserWindowInterface()->GetProfile();
      MarkSiteWithUserContent(*profile, url);
      Notify("session.user_content",
             "This page has content written by other people. It may contain "
             "hidden instructions.");
    }
    return outline;
  }

  void Deny(PendingCall call) {
    active_denied_ = true;
    gate_.LogDenial(call.name);
    Finish(std::move(call.reply), call.request_id,
           "Action requires user confirmation and was denied.", true);
  }

  void OnActionComplete(PendingCall call,
                        std::vector<actor::ActionResultWithLatencyInfo> results,
                        actor::TabObservationStrategy observation) {
    if (in_flight_ != call.call_id) {
      return;
    }
    const auto failure = std::ranges::find_if(results, [](const auto &action) {
      return !action.result || !actor::IsOk(*action.result);
    });
    if (results.empty() || failure != results.end()) {
      const std::string reason =
          failure != results.end() && failure->result
              ? DescribeActionFailure(failure->result->code)
              : "The browser returned no action result.";
      Finish(std::move(call.reply), call.request_id, reason, true);
      return;
    }
    if (!PageInScope()) {
      Finish(std::move(call.reply), call.request_id,
             "The shared tab is no longer in scope.", true);
      return;
    }
    ReadPage(*PageContents(),
             base::BindOnce(
                 &AgentSession::OnObservation, weak_factory_.GetWeakPtr(),
                 std::move(call),
                 PageContents()->GetPrimaryMainFrame()->GetWeakDocumentPtr()));
  }

  void OnObservation(PendingCall call, content::WeakDocumentPtr document,
                     optimization_guide::AIPageContentResultOrError result) {
    if (in_flight_ != call.call_id) {
      return;
    }
    if (!PageInScope() ||
        document.AsRenderFrameHostIfValid() !=
            PageContents()->GetPrimaryMainFrame() ||
        !result) {
      Finish(std::move(call.reply), call.request_id,
             "Action completed. No page observation is available.",
             call.name == "set_checked" || call.arguments.contains("until"));
      return;
    }
    page_ = ReadOutline(result->proto);
    if (page_->instruction_like) {
      gate_.FlagPageInstructions();
    }
    if (page_->handoff != HandoffReason::kNone) {
      const auto reason = page_->handoff;
      BeginHandoff(HandoffName(reason), call.request_id, std::move(call.reply));
      return;
    }
    if (const auto *condition = call.arguments.Find("until");
        condition && !page_->ContainsVisible(*condition)) {
      Finish(std::move(call.reply), call.request_id,
             "The condition was not visible after the wait.", true);
      return;
    }
    if (call.name == "set_checked") {
      const auto target =
          page_->targets.find(*call.arguments.FindInt("dom_node_id"));
      if (target == page_->targets.end() ||
          target->second.checked != call.arguments.FindBool("checked")) {
        Finish(std::move(call.reply), call.request_id,
               "The requested checkbox state could not be verified.", true);
        return;
      }
    }
    SendOutline(std::move(call.reply), call.request_id, 0);
  }

  void SendOutline(ToolPipe::Reply reply, std::string_view id, size_t start) {
    if (!page_ || (start && start >= page_->rows.size())) {
      Finish(std::move(reply), id, "Continuation is no longer available.",
             true);
      return;
    }
    if (start == 0 && page_->source_id.empty() && options_) {
      page_->source_id = sources_.Record(*PageContents());
    }
    std::string content =
        base::StrCat({"hidden=", base::NumberToString(page_->hidden),
                      page_->truncated ? "\ntruncated=true\n" : "\n"});
    size_t row = start;
    for (; row < page_->rows.size() && content.size() < kResponseBytes; ++row) {
      content += page_->rows[row];
      content.push_back('\n');
    }
    const int continuation =
        row < page_->rows.size() ? static_cast<int>(row) : 0;
    Finish(std::move(reply), id,
           WrapContent(std::move(content), origin_, PageToolTabId(),
                       page_->source_id),
           false, continuation);
  }

  void RunHtmlTool(std::string_view name, const base::DictValue &arguments,
                   const std::string &id, ToolPipe::Reply reply) {
    const auto &html = *arguments.FindString("html");
    if (name == "html_preview") {
      const auto *steps = arguments.FindList("steps");
      chat_card_->PreviewHtml(
          html, arguments.FindInt("width").value_or(kDefaultHtmlWidth),
          Field(arguments, "appearance") != "light",
          steps ? steps->Clone() : base::ListValue(),
          base::BindOnce(&AgentSession::OnHtmlPreview,
                         weak_factory_.GetWeakPtr(), id, std::move(reply)));
      return;
    }
    if (chat_reply_id_.empty()) {
      ExtendChatReply("");
    }
    const auto &title = *arguments.FindString("title");
    const int height = arguments.FindInt("height").value_or(480);
    auto render_id = chat_card_->ShowHtml(chat_reply_id_, html, title,
                                          kDefaultHtmlWidth, height);
    if (render_id.empty()) {
      Finish(std::move(reply), id, "The browser could not attach the render.",
             true);
      return;
    }
    if (source_history_) {
      SendSidecar("history.append",
                  base::DictValue()
                      .Set("session_id", session_id_)
                      .Set("kind", "message.html")
                      .Set("data", base::DictValue()
                                       .Set("html", html)
                                       .Set("title", title)
                                       .Set("height", height)));
    }
    Finish(std::move(reply), id, base::StrCat({"Rendered HTML: ", render_id}),
           false);
  }

  bool CanCapturePage(const PageOutline &outline) const {
    if (!PageInScope() || outline.document.empty() || outline.truncated ||
        outline.sensitive_page || outline.contains_redactions ||
        outline.handoff != HandoffReason::kNone) {
      return false;
    }
    auto *main_frame = PageContents()->GetPrimaryMainFrame();
    bool has_subframes = false;
    main_frame->ForEachRenderFrameHost([&](content::RenderFrameHost *frame) {
      has_subframes |= frame != main_frame;
    });
    return !has_subframes;
  }

  void CapturePage(PendingCall call, const PageOutline &outline) {
    if (!CanCapturePage(outline)) {
      Finish(std::move(call.reply), call.request_id,
             "The page changed and can no longer be captured.", true);
      return;
    }
    auto &contents = *PageContents();
    auto *view = contents.GetRenderWidgetHostView();
    const gfx::Rect viewport(view ? view->GetViewBounds().size() : gfx::Size());
    if (viewport.IsEmpty()) {
      Finish(std::move(call.reply), call.request_id,
             "The page has no view to capture.", true);
      return;
    }
    auto *service = page_content_annotations::
        PageContentScreenshotServiceFactory::GetForProfile(&SharedProfile());
    if (!service) {
      Finish(std::move(call.reply), call.request_id,
             "The browser screenshot service is unavailable.", true);
      return;
    }
    capture_activity_ = contents.IncrementCapturerCount(
        gfx::Size(), /*stay_hidden=*/false, /*stay_awake=*/false,
        /*is_activity=*/false);
    capture_timeout_.Start(
        FROM_HERE, base::Seconds(8),
        base::BindOnce(&AgentSession::AbortCall, weak_factory_.GetWeakPtr(),
                       "capture_timeout", "The page capture timed out."));
    constexpr auto kScrollOffset =
        paint_preview::mojom::ClipCoordOverride::kScrollOffset;
    service->RequestScreenshot(
        &contents,
        {.clip_rect = viewport,
         .scale_factor = 1.0,
         .clip_x_coord_override = kScrollOffset,
         .clip_y_coord_override = kScrollOffset,
         .redaction_params = {},
         .max_per_capture_bytes = kMaxPaintPreviewRecordingBytes},
        base::BindOnce(&AgentSession::OnPagePixels, weak_factory_.GetWeakPtr(),
                       std::move(call),
                       contents.GetPrimaryMainFrame()->GetWeakDocumentPtr()));
  }

  void OnPagePixels(PendingCall call, content::WeakDocumentPtr document,
                    base::expected<const SkBitmap *, std::string> pixels) {
    if (in_flight_ != call.call_id) {
      return;
    }
    capture_activity_.RunAndReset();
    if (!PageInScope() ||
        document.AsRenderFrameHostIfValid() !=
            PageContents()->GetPrimaryMainFrame() ||
        !pixels.has_value() || (*pixels)->drawsNothing()) {
      Finish(std::move(call.reply), call.request_id,
             "A screenshot of the approved page was unavailable.", true);
      return;
    }
    ReadPage(*PageContents(),
             base::BindOnce(&AgentSession::OnCaptureRechecked,
                            weak_factory_.GetWeakPtr(), std::move(call),
                            std::move(document), **pixels));
  }

  void
  OnCaptureRechecked(PendingCall call, content::WeakDocumentPtr document,
                     SkBitmap bitmap,
                     optimization_guide::AIPageContentResultOrError result) {
    if (in_flight_ != call.call_id) {
      return;
    }
    if (!PageInScope() ||
        document.AsRenderFrameHostIfValid() !=
            PageContents()->GetPrimaryMainFrame() ||
        !result || !CanCapturePage(ReadOutline(result->proto))) {
      Finish(std::move(call.reply), call.request_id,
             "The page changed and can no longer be shared as an image.", true);
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](SkBitmap bitmap) -> std::string {
              const auto png =
                  gfx::PNGCodec::FastEncodeBGRASkBitmap(bitmap, false);
              return png && png->size() <= 2 * 1024 * 1024
                         ? base::Base64Encode(*png)
                         : std::string();
            },
            std::move(bitmap)),
        base::BindOnce(&AgentSession::OnPageImageEncoded,
                       weak_factory_.GetWeakPtr(), std::move(call),
                       std::move(document)));
  }

  void OnPageImageEncoded(PendingCall call, content::WeakDocumentPtr document,
                          std::string data) {
    if (in_flight_ != call.call_id) {
      return;
    }
    if (!PageInScope() ||
        document.AsRenderFrameHostIfValid() !=
            PageContents()->GetPrimaryMainFrame() ||
        data.empty()) {
      Finish(std::move(call.reply), call.request_id,
             "The browser could not encode the approved page screenshot.",
             true);
      return;
    }
    base::ListValue content;
    content.Append(
        base::DictValue()
            .Set("type", "text")
            .Set("text", WrapContent("Page screenshot of untrusted "
                                     "web content.",
                                     origin_, PageToolTabId(),
                                     sources_.Record(*PageContents()))));
    FinishImage(std::move(call.reply), call.request_id,
                base::Value(std::move(data)), std::move(content), {});
  }

  void OnHtmlPreview(std::string id, ToolPipe::Reply reply,
                     base::DictValue result) {
    if (!InScope() || active_request_ != id || active_tool_ != "html_preview") {
      return;
    }
    auto *screenshot = result.FindDict("screenshot");
    auto image_data = screenshot ? screenshot->Extract("data") : std::nullopt;
    auto *messages = result.FindList("consoleMessages");
    if (messages) {
      for (auto &message : *messages) {
        auto &record = message.GetDict();
        const url::Origin render_origin;
        record.Set("text", WrapContent(filter_.Field(Field(record, "text"),
                                                     render_origin),
                                       render_origin));
      }
    }
    if (!image_data) {
      const auto reason = Field(result, "failure");
      const auto *pin = PinnedAgentComponent();
      static constexpr std::string_view kPreviewDiagnostics[] = {
          "timeout",      "no_view",       "copy_failed",
          "empty_bitmap", "encode_failed", "paint_preview_error"};
      if (preview_diagnostics_ && pin && pin->unsigned_development &&
          sidecar_ && std::ranges::contains(kPreviewDiagnostics, reason)) {
        sidecar_->Send(
            base::DictValue()
                .Set("v", 1)
                .Set("type", "event")
                .Set("event", "browser.preview_diagnostic")
                .Set("data", base::DictValue().Set("reason", reason)));
      }
      std::string error =
          base::StrCat({"The HTML preview could not be captured (",
                        reason.empty() ? "no_image" : reason, ")."});
      if (messages && !messages->empty()) {
        if (auto console = base::WriteJson(*messages)) {
          base::StrAppend(&error, {"\n", *console});
        }
      }
      Finish(std::move(reply), id, std::move(error), true);
      return;
    }
    FinishImage(std::move(reply), id, std::move(*image_data), {},
                std::move(result));
  }

  void FinishImage(ToolPipe::Reply reply, std::string_view id,
                   base::Value image, base::ListValue content,
                   base::DictValue details) {
    content.Append(base::DictValue()
                       .Set("type", "image")
                       .Set("mimeType", "image/png")
                       .Set("data", std::move(image)));
    auto result = base::DictValue()
                      .Set("content", std::move(content))
                      .Set("structuredContent", std::move(details))
                      .Set("isError", false);
    ToolActivity(active_request_, active_tool_, "done");
    ResetCall();
    std::move(reply).Run(Envelope(id).Set("result", std::move(result)));
  }

  void Finish(ToolPipe::Reply reply, std::string_view id, std::string text,
              bool error, int continuation = 0) {
    if (!error && options_ && gate_.requires_confirmation() &&
        !history_flag_saved_) {
      auto completion = base::BindOnce(
          [](base::WeakPtr<AgentSession> self, ToolPipe::Reply reply,
             std::string id, std::string text, int continuation, bool saved) {
            if (!self || !self->InScope() || self->handed_off_ ||
                self->active_request_ != id) {
              return;
            }
            self->history_flag_saved_ = saved;
            if (!saved) {
              text = "The browser could not retain this session's review "
                     "requirements.";
            }
            self->Finish(std::move(reply), id, std::move(text), !saved,
                         saved ? continuation : 0);
          },
          weak_factory_.GetWeakPtr(), std::move(reply), std::string(id),
          std::move(text), continuation);
      if (!HistoryTaskRunner()->PostTaskAndReplyWithResult(
              FROM_HERE,
              base::BindOnce(&UpdateHistorySession, profile_path_, session_id_,
                             true, std::optional<base::DictValue>()),
              std::move(completion))) {
        Stop();
      }
      return;
    }
    if (!active_tool_.empty()) {
      ToolActivity(active_request_, active_tool_,
                   error ? (active_denied_ ? "denied" : "failed") : "done");
    }
    ResetCall();
    std::move(reply).Run(ToolResult(id, std::move(text), error, continuation));
  }

  struct HandoffDecision {
    std::string request_id;
    ToolPipe::Reply reply;
    bool done;
  };

  const tabs::TabHandle tab_;
  const SessionID window_;
  const bool standalone_;
  std::vector<std::unique_ptr<WorkTab>> work_tabs_;
  raw_ptr<WorkTab> active_work_tab_ = nullptr;
  bool work_tab_lost_ = false;
  bool shared_site_revoked_ = false;
  url::Origin origin_;
  url::Origin shared_origin_;
  const base::WeakPtr<actor::ActorKeyedService> service_;
  UiStateManager ui_;
  ConfirmationGate gate_;
  PrivacyFilter filter_;
  PrivacyFilter context_filter_{PrivacyFilter::PlaceholderMode::kRedact};
  std::optional<ContextTab> context_tab_;
  Organization organization_;
  AgentComponent component_;
  std::optional<SessionOptions> options_;
  std::optional<ProviderSandbox> sandbox_;
  std::optional<HistoryContext> history_context_;
  SessionEvent events_;
  base::FilePath profile_path_;
  std::string space_id_;
  std::unique_ptr<ChatCard> chat_card_;
  SourceRecords sources_;
  std::unique_ptr<ResearchBatch> research_batch_;
  std::map<ResearchKey, ResearchResult> research_cache_;
  std::map<ResearchKey, GURL> research_cache_aliases_;
  std::map<std::string, GURL, std::less<>> research_links_;
  std::set<GURL> research_sign_in_walls_;
  std::set<std::string, std::less<>> research_sign_in_grants_;
  std::string research_partition_;
  std::string research_continuation_;
  size_t research_offset_ = 0;
  std::vector<std::string> research_output_;
  std::set<GURL> warned_user_content_;
  bool source_history_ = false;
  bool preview_diagnostics_ = false;
  std::string chat_reply_;
  std::string chat_reply_id_;
  int chat_sequence_ = 0;
  bool chat_link_pending_ = false;
  base::FilePath component_path_;
  ToolPipe::Endpoint endpoint_;
  std::unique_ptr<SidecarProcess> sidecar_;
  base::OneShotTimer handshake_;
  base::OneShotTimer transcript_timeout_;
  SavedTranscriptReply transcript_reply_;
  std::string transcript_session_;
  std::string transcript_message_;
  int transcript_sequence_ = 0;
  std::string session_id_;
  std::string history_revocation_;
  int message_id_ = 0;
  bool hello_ = false;
  bool history_flag_saved_ = false;
  bool session_ready_ = false;
  actor::TaskId task_id_;
  base::CallbackListSubscription task_state_observation_;
  bool actor_tab_bound_ = false;
  std::unique_ptr<ToolPipe> pipe_;
  base::ScopedObservation<permissions::PermissionRequestManager,
                          permissions::PermissionRequestManager::Observer>
      permissions_observation_{this};
  base::OneShotTimer idle_;
  std::optional<PageOutline> page_;
  std::optional<std::string> in_flight_;
  std::deque<QueuedCall> queued_calls_;
  bool call_dispatch_pending_ = false;
  std::string active_request_;
  std::string active_tool_;
  bool active_denied_ = false;
  base::ScopedClosureRunner capture_activity_;
  base::OneShotTimer capture_timeout_;
  base::OneShotTimer work_tab_close_timeout_;
  std::string closing_work_tab_;
  base::OnceCallback<void(std::string, bool)> work_tab_close_reply_;
  scoped_refptr<base::RefCountedData<ToolPipe::Reply>> active_reply_;
  bool site_revoked_ = false;
  bool handed_off_ = false;
  bool prompt_gate_bound_ = false;
  std::optional<HandoffDecision> pending_handoff_;
  std::optional<NavigationGrant> navigation_grant_;
  std::vector<ToolPipe::ClientDecision> pending_relay_decisions_;
  base::WeakPtrFactory<AgentSession> weak_factory_{this};
};

std::unique_ptr<AgentSession> &DevelopmentSession() {
  static base::NoDestructor<std::unique_ptr<AgentSession>> session;
  return *session;
}

std::unique_ptr<AgentSession> &ProductionSession() {
  static base::NoDestructor<std::unique_ptr<AgentSession>> session;
  return *session;
}

SavedTranscriptReader &TranscriptReader() {
  static base::NoDestructor<SavedTranscriptReader> reader;
  return *reader;
}

base::CallbackListSubscription &OptInObserver() {
  static base::NoDestructor<base::CallbackListSubscription> observer;
  return *observer;
}
} // namespace

std::optional<ContextTab> ContextTab::Capture(tabs::TabInterface &tab,
                                              BrowserWindowInterface &window) {
  auto *contents = tab.GetContents();
  auto *profile = window.GetProfile();
  if (tab.GetBrowserWindowInterface() != &window || !profile ||
      profile != tab.GetProfile() || !profile->IsRegularProfile() ||
      !contents || !contents->GetPrimaryMainFrame() ||
      !ConfirmationGate::IsWebNavigation(contents->GetLastCommittedURL()) ||
      contents->GetVisibleURL() != contents->GetLastCommittedURL()) {
    return std::nullopt;
  }
  return ContextTab{tab.GetHandle(),
                    contents->GetPrimaryMainFrame()->GetWeakDocumentPtr(),
                    contents->GetLastCommittedURL()};
}

content::WebContents *
ContextTab::GetContents(BrowserWindowInterface &window) const {
  auto *current_tab = tab.Get();
  auto *profile = window.GetProfile();
  if (!current_tab || current_tab->GetBrowserWindowInterface() != &window ||
      !profile || current_tab->GetProfile() != profile ||
      !profile->IsRegularProfile()) {
    return nullptr;
  }
  auto *contents = current_tab->GetContents();
  auto *frame = document.AsRenderFrameHostIfValid();
  return contents && frame && frame == contents->GetPrimaryMainFrame() &&
                 contents->GetLastCommittedURL() == url &&
                 contents->GetVisibleURL() == url
             ? contents
             : nullptr;
}

bool StartAgentSession(tabs::TabInterface &tab, SessionOptions options,
                       SessionEvent events) {
  if (!IsProductionAgentEnabled() || !PinnedAgentComponent() || !events ||
      options.provider.empty() ||
      (options.prompt.empty() && options.resume_session_id.empty()) ||
      options.resume_session_id.size() > 128 || !PromptFits(options.prompt) ||
      options.approved_program.executable.empty() ||
      TranscriptReader().busy()) {
    return false;
  }
  auto *window = tab.GetBrowserWindowInterface();
  auto *profile = window ? window->GetProfile() : nullptr;
  if (!profile || !profile->GetPrefs()->GetBoolean(kHistoryEnabled) ||
      !CanStartAgentSession(tab)) {
    return false;
  }
  if (options.context_tab &&
      (!options.resume_session_id.empty() ||
       !options.context_tab->GetContents(*window) ||
       !PromptFits(base::StrCat({options.prompt, kContextTabInstruction})))) {
    return false;
  }
  if ((options.restricted_provider || options.provider_web_access) &&
      profile->GetPrefs()
              ->GetDict(kProviderConsents)
              .FindInt(options.provider)
              .value_or(0) < (options.isolation == ProviderIsolation::kJob ? 3
                              : options.provider_web_access ? 2
                                                            : 1)) {
    return false;
  }
  auto *service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    return false;
  }

  ProductionSession() = std::make_unique<AgentSession>(tab, *service);
  ProductionSession()->StartProduction(std::move(options), std::move(events));
  return true;
}

bool PlaceAgentChatCard(ChatCard &card, BrowserWindowInterface &window,
                        const gfx::Rect &bounds) {
  if (bounds.IsEmpty()) {
    card.Detach();
    return true;
  }
  auto *browser_view = BrowserView::GetBrowserViewForBrowser(&window);
  if (!browser_view) {
    return false;
  }
  // The shell's bounds are relative to the native client area, like the page
  // card's. Map them the way the page is mapped and keep them in the view.
  gfx::Rect placed = bounds;
  POINT origin{};
  if (const auto hwnd = views::HWNDForView(browser_view);
      hwnd && ::ClientToScreen(hwnd, &origin)) {
    const auto screen_origin =
        display::win::GetScreenWin()
            ->ScreenToDIPRect(hwnd, gfx::Rect(origin.x, origin.y, 0, 0))
            .origin();
    const auto view_origin = browser_view->GetBoundsInScreen().origin();
    placed.Offset(screen_origin.x() - view_origin.x(),
                  screen_origin.y() - view_origin.y());
  }
  placed.Intersect(browser_view->GetLocalBounds());
  return !placed.IsEmpty() && card.SetPlacement(*browser_view, placed);
}

bool HasRunningAgentSession(BrowserWindowInterface &window) {
  auto &session = ProductionSession();
  return session && session->OwnsChatWindow(window) && !session->stopped();
}

bool AgentChatUsesTab(tabs::TabInterface &tab) {
  const auto &session = ProductionSession();
  return session && session->UsesTab(tab);
}

bool IsAgentWorkingOnTab(tabs::TabInterface &tab) {
  const auto &production = ProductionSession();
  const auto &development = DevelopmentSession();
  return (production && production->ShowsWorkingOn(tab)) ||
         (development && development->ShowsWorkingOn(tab));
}

bool HasAgentChat(BrowserWindowInterface &window) {
  auto &session = ProductionSession();
  return session && session->chat_card() && session->OwnsChatWindow(window);
}

bool CanStartAgentSession(tabs::TabInterface &tab) {
  auto *window = tab.GetBrowserWindowInterface();
  auto *profile = window ? window->GetProfile() : nullptr;
  auto *contents = tab.GetContents();
  return profile && profile->IsRegularProfile() && contents &&
         (contents->GetLastCommittedURL().SchemeIsHTTPOrHTTPS() ||
          contents->GetLastCommittedURL() == GURL(kAgentNewTabUrl)) &&
         !TranscriptReader().busy() &&
         (!ProductionSession() || ProductionSession()->stopped());
}

bool StopAgentTurn(BrowserWindowInterface &window) {
  auto &session = ProductionSession();
  if (!session || !session->OwnsChatWindow(window) || session->stopped()) {
    return false;
  }
  session->StopTurn();
  return true;
}

void CloseAgentChat(BrowserWindowInterface &window) {
  auto &session = ProductionSession();
  if (session && session->OwnsChatWindow(window)) {
    session.reset();
  }
}

bool SendAgentMessage(std::string message, std::string model,
                      std::string reasoning_option, std::string reasoning,
                      std::optional<ContextTab> context_tab) {
  return ProductionSession() &&
         ProductionSession()->SendMessage(
             std::move(message), std::move(model), std::move(reasoning_option),
             std::move(reasoning), std::move(context_tab));
}

bool SetAgentProviderConsent(Profile &profile, std::string_view provider,
                             bool provider_web_access, bool allowed,
                             bool job_only) {
  if (!profile.IsRegularProfile() || !IsProductionAgentEnabled() ||
      provider.empty() || provider.size() > 256) {
    return false;
  }
  ScopedDictPrefUpdate consents(profile.GetPrefs(), kProviderConsents);
  if (allowed) {
    consents->Set(provider, job_only ? 3 : provider_web_access ? 2 : 1);
  } else {
    consents->Remove(provider);
  }
  return true;
}

bool ListAgentSavedSessions(Profile &profile, AgentHistoryList reply) {
  if (!profile.IsRegularProfile() || !IsProductionAgentEnabled() || !reply) {
    return false;
  }
  auto *os_crypt = g_browser_process->os_crypt_async();
  if (!os_crypt) {
    return false;
  }
  os_crypt->GetInstance(base::BindOnce(
      [](base::FilePath path, AgentHistoryList reply,
         scoped_refptr<os_crypt_async::Encryptor> encryptor) {
        HistoryTaskRunner()->PostTaskAndReplyWithResult(
            FROM_HERE, base::BindOnce(&ListHistory, path, std::move(encryptor)),
            std::move(reply));
      },
      profile.GetPath(), std::move(reply)));
  return true;
}

bool OpenAgentSavedTranscript(Profile &profile, std::string session_id,
                              int cursor, SavedTranscriptReply reply) {
  if (!profile.IsRegularProfile() || !IsProductionAgentEnabled() || !reply) {
    return false;
  }
  auto &session = ProductionSession();
  if (session && !session->stopped()) {
    return session->profile_path() == profile.GetPath() &&
           session->OpenSavedTranscript(std::move(session_id), cursor,
                                        std::move(reply));
  }
  return TranscriptReader().Open(profile, std::move(session_id), cursor,
                                 std::move(reply));
}

bool SetAgentSessionArchived(Profile &profile, std::string session_id,
                             bool archived,
                             base::OnceCallback<void(bool)> reply) {
  if (!profile.IsRegularProfile() || !IsProductionAgentEnabled() || !reply) {
    return false;
  }
  return HistoryTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&ArchiveHistorySession, profile.GetPath(),
                     std::move(session_id), archived),
      std::move(reply));
}

bool DeleteAgentSavedSessions(Profile &profile,
                              std::optional<std::string> session_id,
                              std::optional<std::string> space_id,
                              base::OnceCallback<void(bool)> reply) {
  if (!profile.IsRegularProfile() || !IsProductionAgentEnabled() || !reply) {
    return false;
  }
  const auto path = profile.GetPath();
  return HistoryTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&DeleteHistoryRecords, path, std::move(session_id),
                     std::move(space_id)),
      base::BindOnce(
          [](base::FilePath path, base::OnceCallback<void(bool)> reply,
             std::optional<base::ListValue> deleted) {
            if (deleted) {
              TranscriptReader().CancelDeleted(path, *deleted);
            }
            if (auto &session = ProductionSession();
                session && session->profile_path() == path && deleted &&
                !deleted->empty()) {
              session->PurgeHistory(std::move(*deleted));
            }
            std::move(reply).Run(deleted.has_value());
          },
          path, std::move(reply)));
}

content::WebContents *GetAgentChatContents() {
  auto &session = ProductionSession();
  auto *card =
      session && IsProductionAgentEnabled() ? session->chat_card() : nullptr;
  return card ? &card->contents() : nullptr;
}

void SetAgentChatTheme(bool dark) {
  auto &session = ProductionSession();
  if (auto *card = session ? session->chat_card() : nullptr) {
    card->SetDark(dark);
  }
}

bool SetAgentChatBounds(BrowserWindowInterface &window,
                        const gfx::Rect &bounds) {
  auto &session = ProductionSession();
  return session && session->SetChatBounds(window, bounds);
}

bool OpenAgentChatForWindow(BrowserWindowInterface &window) {
  auto &session = ProductionSession();
  return session && session->OpenChat(window);
}

void StopAgentSession() { ProductionSession().reset(); }

void MaybeStartDevelopmentSession() {
  if (krypton::privacy::PrefName(krypton::privacy::Id::kAiAgent).empty()) {
    return;
  }
  if (auto *registry = krypton::privacy::GetRegistry();
      registry && IsProductionAgentEnabled()) {
    StartHistoryMaintenance(
        base::BindRepeating(
            [](const base::FilePath &profile, base::ListValue ids) {
              TranscriptReader().CancelDeleted(profile, ids);
              if (auto &session = ProductionSession();
                  session && session->profile_path() == profile) {
                session->PurgeHistory(std::move(ids));
              }
            }),
        base::BindRepeating([](const base::FilePath &profile) {
          TranscriptReader().CancelProfile(profile);
          if (auto &session = ProductionSession();
              session && session->profile_path() == profile) {
            StopAgentSession();
          }
        }));
    OptInObserver() =
        registry->Observe(base::BindRepeating([](krypton::privacy::Id id) {
          if (id == krypton::privacy::Id::kAiAgent &&
              !IsProductionAgentEnabled()) {
            StopHistoryMaintenance();
            TranscriptReader().Cancel();
            StopAgentSession();
            EraseAllProfileAgentData(base::BindOnce(&RemoveAgentComponent));
          }
        }));
  }
  const bool development =
      base::CommandLine::ForCurrentProcess()->HasSwitch(kDevelopmentSwitch);
  if (!development &&
      !krypton::privacy::IsStartupEnabled(krypton::privacy::Id::kAiAgent)) {
    return;
  }
  // Production sessions are user-initiated. The development switch alone
  // creates a session at startup.
  if (!development || DevelopmentSession()) {
    return;
  }
  auto *collection = GlobalBrowserCollection::GetInstance();
  BrowserWindowInterface *first = nullptr;
  if (collection) {
    collection->ForEach([&first](BrowserWindowInterface *browser) {
      first = browser;
      return false;
    });
  }
  auto *tab = first ? first->GetActiveTabInterface() : nullptr;
  auto *contents = tab ? tab->GetContents() : nullptr;
  auto *profile = first ? first->GetProfile() : nullptr;
  auto *service = profile && profile->IsRegularProfile()
                      ? actor::ActorKeyedService::Get(profile)
                      : nullptr;
  LOG(INFO) << "Krypton Agent ActorKeyedService: "
            << (service ? "available" : "unavailable");
  if (!service || !contents ||
      !contents->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    LOG(WARNING) << "Krypton Agent development pipe needs an HTTP or HTTPS tab "
                    "at startup";
    return;
  }
  DevelopmentSession() = std::make_unique<AgentSession>(*tab, *service);
  DevelopmentSession()->Start();
}

void StopDevelopmentSession() {
  OptInObserver() = {};
  StopHistoryMaintenance();
  TranscriptReader().Cancel();
  StopAgentSession();
  DevelopmentSession().reset();
}

} // namespace krypton_agent
