#include "chrome/browser/ui/krypton_ui/agent_panel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/values_util.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/krypton_agent/agent_session.h"
#include "chrome/browser/krypton_agent/agent_tools.h"
#include "chrome/browser/krypton_agent/chat/chat_card.h"
#include "chrome/browser/krypton_agent/history_prefs.h"
#include "chrome/browser/krypton_agent/panel_connection.h"
#include "chrome/browser/krypton_agent/site_allowlist.h"
#include "chrome/browser/krypton_agent/skill_import_picker.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/krypton_ui/window_session.h"
#include "components/krypton_privacy_features/privacy_features.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_handle.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace krypton_ui {
namespace {
namespace agent = krypton_agent;

constexpr size_t kMaxInstances = 256;

std::string_view String(const base::DictValue& dict, std::string_view key) {
  const auto* value = dict.FindString(key);
  return value ? std::string_view(*value) : std::string_view();
}

std::string_view String(KrStr value) {
  return value.data ? std::string_view(value.data, value.size)
                    : std::string_view();
}

KrStr ToKr(std::string_view value) {
  return {value.data(), value.size()};
}

bool Valid(KrStr value, size_t max) {
  return value.size <= max && (value.size == 0 || value.data);
}

uint32_t Flag(bool set, uint32_t bit) {
  return set ? bit : 0u;
}

bool Bool(const base::DictValue& dict, std::string_view key) {
  return dict.FindBool(key).value_or(false);
}

bool IsBrowserOnly(const base::DictValue& provider) {
  return String(provider, "sandbox_mode") != "job" &&
         (String(provider, "tier") == "browser_only" ||
          String(provider, "tier") == "browser-only");
}

bool HasWebAccess(const base::DictValue& provider) {
  return provider.FindBool("provider_side_web_access").value_or(true);
}

bool Consented(Profile& profile, const base::DictValue& provider) {
  const int required = String(provider, "sandbox_mode") == "job" ? 3
                       : HasWebAccess(provider)                  ? 2
                                                                 : 1;
  return profile.GetPrefs()
             ->GetDict(agent::kProviderConsents)
             .FindInt(String(provider, "instance_id"))
             .value_or(0) >= required;
}

std::optional<agent::ProviderLaunchRequest> ProviderLaunch(
    const base::DictValue& provider) {
  const auto instance = String(provider, "instance_id");
  const auto mode = String(provider, "sandbox_mode");
  const auto driver = String(provider, "driver");
  const auto path = [&provider](std::string_view key) {
    return base::FilePath::FromUTF8Unsafe(String(provider, key));
  };
  auto program = path("program_path");
  if (instance.empty() || !program.IsAbsolute() || program.ReferencesParent() ||
      (mode == "job" && driver != "opencode") ||
      (!mode.empty() && mode != "app_container" && mode != "lpac" &&
       mode != "job" && mode != "none")) {
    return std::nullopt;
  }
  agent::ProviderLaunchRequest launch{
      .instance = std::string(instance),
      .program = {.executable = std::move(program),
                  .cli_directory = driver == "claude" || driver == "codex"
                                       ? path("cli_folder")
                                       : base::FilePath(),
                  .driver = std::string(driver)},
      .isolation = mode == "job"    ? agent::ProviderIsolation::kJob
                   : mode == "lpac" ? agent::ProviderIsolation::kLpac
                                    : agent::ProviderIsolation::kAppContainer};
  if (mode == "job" && driver == "opencode") {
    launch.usage_database_path = String(provider, "usage_database_path");
  }
  if (const auto* runtime = provider.FindDict("runtime_set")) {
    auto interpreter =
        base::FilePath::FromUTF8Unsafe(String(*runtime, "interpreter"));
    if (!base::FilePath::CompareEqualIgnoreCase(
            launch.program.executable.value(), interpreter.value())) {
      return std::nullopt;
    }
    launch.program.script =
        base::FilePath::FromUTF8Unsafe(String(*runtime, "script"));
    launch.program.package_root =
        base::FilePath::FromUTF8Unsafe(String(*runtime, "package_root"));
    if (launch.program.script.empty() || launch.program.package_root.empty()) {
      return std::nullopt;
    }
  }
  return launch;
}

// The model's T3 reasoning option id and the command's level, when the
// level is one the sidecar listed for that model; otherwise both empty and
// the model's default applies.
std::pair<std::string, std::string> Reasoning(const base::DictValue& provider,
                                              std::string_view model,
                                              const KrAgentCommand& command) {
  if (!KR_HAS_FIELD(&command, KrAgentCommand, reasoning) ||
      !command.reasoning.size) {
    return {};
  }
  const std::string_view level(command.reasoning.data, command.reasoning.size);
  const auto* models = provider.FindList("models");
  if (!models) {
    return {};
  }
  for (const auto& value : *models) {
    const auto* entry = value.GetIfDict();
    const auto* id = entry ? entry->FindString("id") : nullptr;
    const auto* reasoning = entry ? entry->FindDict("reasoning") : nullptr;
    if (!id || *id != model || !reasoning) {
      continue;
    }
    const auto* option = reasoning->FindString("option");
    const auto* choices = reasoning->FindList("choices");
    if (!option || !choices) {
      return {};
    }
    for (const auto& item : *choices) {
      const auto* choice = item.GetIfDict();
      const auto* choice_id = choice ? choice->FindString("id") : nullptr;
      if (choice_id && *choice_id == level) {
        return {*option, std::string(level)};
      }
    }
    return {};
  }
  return {};
}

template <typename Map>
const base::DictValue* Find(const Map& map, KrAgentRecordId id) {
  const auto item = map.find(id);
  return item == map.end() ? nullptr : &item->second;
}

template <typename Map, typename Upsert, typename Remove>
void Reconcile(Map& current, Map next, Upsert upsert, Remove remove) {
  for (const auto& [id, record] : current) {
    if (!next.contains(id)) {
      remove(id);
    }
  }
  for (const auto& [id, record] : next) {
    const auto previous = current.find(id);
    if (previous == current.end() || previous->second != record) {
      upsert(id, record);
    }
  }
  current = std::move(next);
}

bool RetentionValue(uint64_t days) {
  return days == 0 || days == 1 || days == 7 || days == 30 || days == 90;
}

}  // namespace

AgentPanel::AgentPanel(BrowserWindowInterface& window, WindowSession& session)
    : window_(window), session_(session) {
  if (profile().IsRegularProfile()) {
    connection_ = std::make_unique<agent::PanelConnection>(
        profile(), base::BindRepeating(&AgentPanel::OnControlEvent,
                                       weak_factory_.GetWeakPtr()));
  }
}

AgentPanel::~AgentPanel() {
  weak_factory_.InvalidateWeakPtrs();
  agent::SetAgentChatBounds(*window_, {});
  agent::CloseAgentChat(*window_);
}

Profile& AgentPanel::profile() const {
  return *window_->GetProfile();
}

const KrShellUI* AgentPanel::api() const {
  return visible_ ? session_->api() : nullptr;
}

bool AgentPanel::Enabled() const {
  return profile().IsRegularProfile() && agent::IsProductionAgentEnabled();
}

void AgentPanel::SetVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  if (!visible_) {
    context_tab_.reset();
    SetBounds({});
    Observe(nullptr);
    prefs_.RemoveAll();
    ++history_generation_;
    // Pending user decisions stay in the host until the panel is shown again.
    return;
  }
  if (!profile().IsRegularProfile()) {
    PublishState();
    return;
  }
  if (!prefs_.prefs()) {
    prefs_.Init(profile().GetPrefs());
  }
  for (const char* key :
       {agent::kHistoryEnabled, agent::kArchiveAfterDays,
        agent::kDeleteAfterDays, agent::kProviderConsents,
        agent::kSiteAllowlist, agent::kNavigationPermission}) {
    prefs_.Add(key, base::BindRepeating(&AgentPanel::PrefChanged,
                                        base::Unretained(this)));
  }
  UpdateActiveTab();
  PublishSites();
  PublishProviders();
  if (Enabled()) {
    RefreshHistory();
    RefreshProviders(false);
  }
  if (const auto* ui = api();
      KR_HAS_SLOT(ui, KrShellUI, clear_agent_activity) &&
      ui->clear_agent_activity) {
    ui->clear_agent_activity(ui->self);
  }
  for (const auto& [id, entry] : activity_) {
    PublishActivity(id, entry);
  }
  const auto* ui = api();
  for (auto it = published_reviews_.begin(); it != published_reviews_.end();) {
    if (!reviews_.contains(*it) &&
        KR_HAS_SLOT(ui, KrShellUI, remove_agent_request) &&
        ui->remove_agent_request) {
      ui->remove_agent_request(ui->self, *it);
      it = published_reviews_.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [id, request] : reviews_) {
    PublishReview(id, request);
  }
}

void AgentPanel::SetBounds(KrRect bounds) {
  bounds_ = {};
  if (visible_ && std::isfinite(bounds.x) && std::isfinite(bounds.y) &&
      std::isfinite(bounds.width) && std::isfinite(bounds.height) &&
      bounds.x >= 0 && bounds.y >= 0 && bounds.width >= 1 &&
      bounds.height >= 1 && bounds.x + bounds.width < 1000000 &&
      bounds.y + bounds.height < 1000000) {
    bounds_.SetRect(static_cast<int>(std::ceil(bounds.x)),
                    static_cast<int>(std::ceil(bounds.y)),
                    static_cast<int>(std::floor(bounds.x + bounds.width) -
                                     std::ceil(bounds.x)),
                    static_cast<int>(std::floor(bounds.y + bounds.height) -
                                     std::ceil(bounds.y)));
  }
  if (saved_card_) {
    agent::SetAgentChatBounds(*window_, {});
    agent::PlaceAgentChatCard(*saved_card_, *window_, bounds_);
  } else {
    agent::SetAgentChatBounds(*window_, bounds_);
  }
}

void AgentPanel::SetDark(bool dark) {
  dark_ = dark;
  if (agent::HasAgentChat(*window_)) {
    agent::SetAgentChatTheme(dark);
  }
  if (saved_card_) {
    saved_card_->SetDark(dark);
  }
}

bool AgentPanel::Show() {
  const auto* ui = session_->api();
  return Enabled() && KR_HAS_SLOT(ui, KrShellUI, show_agent_panel) &&
         ui->show_agent_panel &&
         ui->show_agent_panel(ui->self, true, true) == KR_OK;
}

void AgentPanel::UpdateActiveTab() {
  if (!visible_) {
    return;
  }
  auto* tab = window_->GetActiveTabInterface();
  Observe(tab ? tab->GetContents() : nullptr);
  PublishState();
}

void AgentPanel::DidFinishNavigation(content::NavigationHandle* navigation) {
  if (navigation->IsInPrimaryMainFrame() && navigation->HasCommitted()) {
    PublishState();
  }
}

void AgentPanel::WebContentsDestroyed() {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&AgentPanel::UpdateActiveTab, weak_factory_.GetWeakPtr()));
}

void AgentPanel::PrivacyChanged() {
  if (!Enabled()) {
    if (connection_) {
      connection_->Close();
    }
    picker_.reset();
    ResetShownSession();
    providers_.clear();
    PublishProviders();
    OnHistory(++history_generation_, base::ListValue());
    OnSkills(base::DictValue().Set("skills", base::ListValue()));
    PublishSites();
  } else if (visible_) {
    RefreshProviders(false);
    RefreshHistory();
  }
  PublishState();
}

void AgentPanel::PrefChanged() {
  PublishState();
  PublishProviders();
  PublishSites();
}

void AgentPanel::PublishState() {
  const auto* ui = api();
  if (!KR_HAS_SLOT(ui, KrShellUI, set_agent_state) || !ui->set_agent_state) {
    return;
  }
  auto* tab = window_->GetActiveTabInterface();
  const bool enabled = Enabled();
  const bool installing = enabled && connection_->busy();
  const bool ready = enabled && connection_->ready();
  const auto* pin = agent::PinnedAgentComponent();
  const bool transcript = saved_card_ || agent::HasAgentChat(*window_);
  const auto* prefs = profile().GetPrefs();
  const std::string_view error = enabled && !pin
                                     ? "A signed AI component is unavailable."
                                     : std::string_view(error_);
  const KrAgentState state{
      .struct_size = sizeof(KrAgentState),
      .availability = static_cast<uint32_t>(!enabled     ? KR_AGENT_OFF
                                            : installing ? KR_AGENT_INSTALLING
                                            : ready ? KR_AGENT_READY
                                                    : KR_AGENT_UNAVAILABLE),
      .status = status_,
      .flags = Flag(enabled && prefs->GetBoolean(agent::kHistoryEnabled),
                    KR_AGENT_HISTORY_ENABLED) |
               Flag(enabled && tab && agent::CanStartAgentSession(*tab) &&
                        !saved_pending_,
                    KR_AGENT_CAN_START) |
               Flag(transcript, KR_AGENT_TRANSCRIPT) |
               Flag(showing_saved_ && transcript, KR_AGENT_SAVED_TRANSCRIPT) |
               Flag(enabled && pin && !installing && !ready &&
                        !agent::HasRunningAgentSession(*window_),
                    KR_AGENT_COMPONENT_UPDATE) |
               Flag(installing && connection_->verifying(),
                    KR_AGENT_COMPONENT_VERIFYING),
      .component_progress =
          installing && !connection_->verifying()
              ? static_cast<int32_t>(connection_->progress() * 100)
              : -1,
      .privacy_feature =
          static_cast<KrPrivacyFeatureId>(krypton::privacy::Id::kAiAgent),
      .provider = provider_,
      .session = shown_id_,
      .archive_after_days = static_cast<uint32_t>(
          std::max(0, prefs->GetInteger(agent::kArchiveAfterDays))),
      .delete_after_days = static_cast<uint32_t>(
          std::max(0, prefs->GetInteger(agent::kDeleteAfterDays))),
      .model = ToKr(model_),
      .title = ToKr(title_),
      .notice = ToKr(notice_),
      .error = ToKr(error),
      .component_version =
          ToKr(ready && pin ? pin->version : std::string_view()),
      .user_content_warning = ToKr(agent::kUserContentWarning),
      .navigation_permission =
          static_cast<uint32_t>(agent::GetNavigationPermission(*prefs)),
      .context_tab = context_tab_
                         ? static_cast<KrTabId>(context_tab_->tab.raw_value())
                         : 0};
  ui->set_agent_state(ui->self, &state);
}

KrAgentRecordId AgentPanel::Id(Names& names, std::string_view key) {
  auto [id, inserted] = names.try_emplace(std::string(key), next_id_);
  if (inserted) {
    ++next_id_;
  }
  return id->second;
}

uint32_t AgentPanel::SiteFlags(KrStr entry) const {
  if (!Enabled() || !Valid(entry, 512)) {
    return 0;
  }
  const auto status = agent::GetAgentSiteEntryStatus(profile(), String(entry));
  return Flag(status.valid, KR_AGENT_SITE_VALID) |
         Flag(status.user_content, KR_AGENT_SITE_USER_CONTENT);
}

KrResult AgentPanel::Command(const KrAgentCommand* input) {
  // `reasoning` is appended; older modules omit it.
  if (!input ||
      input->struct_size < offsetof(KrAgentCommand, provider_count) +
                               sizeof(input->provider_count) ||
      (KR_HAS_FIELD(input, KrAgentCommand, reasoning) &&
       !Valid(input->reasoning, 256)) ||
      !Valid(input->text, 512 * 1024) || !Valid(input->model, 256) ||
      !Valid(input->name, 100) || !Valid(input->description, 500) ||
      input->provider_count > kMaxInstances ||
      (input->provider_count && !input->providers)) {
    return KR_ERR_INVALID_ARGUMENT;
  }
  if (!Enabled()) {
    return KR_ERR_UNSUPPORTED;
  }
  const auto& command = *input;
  const auto* provider = Find(providers_, command.provider);
  const auto* saved = Find(sessions_, command.id);
  bool accepted = false;
  switch (command.kind) {
    case KR_AGENT_COMMAND_SET_SURFACE: {
      auto* tab = window_->GetActiveTabInterface();
      if (visible_ && command.value == 1 && tab &&
          tab->GetContents()->GetVisibleURL() == GURL(agent::kAgentNewTabUrl) &&
          agent::HasAgentChat(*window_) && !agent::AgentChatUsesTab(*tab)) {
        ResetShownSession();
        PublishState();
      }
      accepted = command.value <= 1;
      break;
    }
    case KR_AGENT_COMMAND_START:
      accepted = visible_ && Start(command, {});
      break;
    case KR_AGENT_COMMAND_SEND: {
      auto reasoning =
          provider ? Reasoning(*provider, String(command.model), command)
                   : std::pair<std::string, std::string>{};
      accepted =
          visible_ && !showing_saved_ && status_ == KR_AGENT_IDLE &&
          agent::HasRunningAgentSession(*window_) &&
          agent::SendAgentMessage(std::string(String(command.text)),
                                  std::string(String(command.model)),
                                  std::move(reasoning.first),
                                  std::move(reasoning.second), context_tab_);
      if (accepted) {
        context_tab_.reset();
        status_ = KR_AGENT_RUNNING;
        PublishState();
      }
      break;
    }
    case KR_AGENT_COMMAND_SET_CONTEXT_TAB: {
      if (!visible_ || showing_saved_ || saved_pending_ ||
          status_ == KR_AGENT_STARTING || status_ == KR_AGENT_RUNNING ||
          status_ == KR_AGENT_WAITING ||
          command.id >
              static_cast<KrTabId>(std::numeric_limits<int32_t>::max())) {
        break;
      }
      if (command.id == 0) {
        context_tab_.reset();
      } else {
        auto* tab = tabs::TabHandle(static_cast<int32_t>(command.id)).Get();
        if (!tab) {
          break;
        }
        auto context = agent::ContextTab::Capture(*tab, *window_);
        if (!context) {
          break;
        }
        context_tab_ = std::move(context);
      }
      accepted = true;
      PublishState();
      break;
    }
    case KR_AGENT_COMMAND_STOP:
      accepted = !showing_saved_ && agent::StopAgentTurn(*window_);
      break;
    case KR_AGENT_COMMAND_NEW_SESSION:
      ResetShownSession();
      PublishState();
      accepted = true;
      break;
    case KR_AGENT_COMMAND_SET_CONSENT:
      accepted = provider && command.value <= 1 &&
                 agent::SetAgentProviderConsent(
                     profile(), String(*provider, "instance_id"),
                     HasWebAccess(*provider), command.value != 0,
                     String(*provider, "sandbox_mode") == "job");
      break;
    case KR_AGENT_COMMAND_SIGN_IN:
      accepted = provider && Bool(*provider, "can_sign_in") &&
                 connection_->Request(
                     "auth.login",
                     base::DictValue()
                         .Set("source", "settings")
                         .Set("provider", String(*provider, "instance_id")),
                     base::BindOnce(&AgentPanel::OnMutation,
                                    weak_factory_.GetWeakPtr()));
      break;
    case KR_AGENT_COMMAND_REFRESH_PROVIDERS:
      accepted = RefreshProviders(true);
      break;
    case KR_AGENT_COMMAND_OPEN_SESSION:
      accepted = visible_ && OpenSaved(command.id);
      break;
    case KR_AGENT_COMMAND_RESUME_SESSION:
      if (visible_ && saved && !String(*saved, "native_thread_id").empty()) {
        const auto instance = provider_ids_.find(String(*saved, "provider"));
        if (instance != provider_ids_.end()) {
          auto resume = command;
          resume.provider = instance->second;
          resume.model = ToKr(String(*saved, "model"));
          accepted = Start(resume, String(*saved, "session_id"));
        }
      }
      break;
    case KR_AGENT_COMMAND_ARCHIVE_SESSION:
      accepted = saved && command.value <= 1 &&
                 agent::SetAgentSessionArchived(
                     profile(), std::string(String(*saved, "session_id")),
                     command.value != 0,
                     base::BindOnce(&AgentPanel::OnHistoryChanged,
                                    weak_factory_.GetWeakPtr()));
      break;
    case KR_AGENT_COMMAND_DELETE_SESSION:
    case KR_AGENT_COMMAND_DELETE_HISTORY:
      if (saved || command.kind == KR_AGENT_COMMAND_DELETE_HISTORY) {
        const auto id =
            command.kind == KR_AGENT_COMMAND_DELETE_HISTORY
                ? std::nullopt
                : std::make_optional(std::string(String(*saved, "session_id")));
        accepted = agent::DeleteAgentSavedSessions(
            profile(), id, std::nullopt,
            base::BindOnce(&AgentPanel::OnHistoryChanged,
                           weak_factory_.GetWeakPtr()));
      }
      break;
    case KR_AGENT_COMMAND_SET_HISTORY:
      if (command.value <= 1 &&
          !profile().GetPrefs()->IsManagedPreference(agent::kHistoryEnabled)) {
        profile().GetPrefs()->SetBoolean(agent::kHistoryEnabled,
                                         command.value != 0);
        accepted = true;
      }
      break;
    case KR_AGENT_COMMAND_SET_ARCHIVE_AFTER:
    case KR_AGENT_COMMAND_SET_DELETE_AFTER: {
      const char* key = command.kind == KR_AGENT_COMMAND_SET_ARCHIVE_AFTER
                            ? agent::kArchiveAfterDays
                            : agent::kDeleteAfterDays;
      if (RetentionValue(command.value) &&
          !profile().GetPrefs()->IsManagedPreference(key)) {
        profile().GetPrefs()->SetInteger(key, static_cast<int>(command.value));
        accepted = true;
      }
      break;
    }
    case KR_AGENT_COMMAND_UPDATE_COMPONENT:
      accepted = !agent::HasRunningAgentSession(*window_) &&
                 connection_->UpdateComponent();
      break;
    case KR_AGENT_COMMAND_ADD_SITE:
      accepted = agent::SetAgentSiteAllowlistEntry(
          profile(), String(command.text), true,
          (command.flags & KR_AGENT_SITE_USER_CONTENT) != 0);
      break;
    case KR_AGENT_COMMAND_SET_NAVIGATION_PERMISSION:
      if (command.value >= KR_AGENT_NAVIGATION_ASK &&
          command.value <= KR_AGENT_NAVIGATION_BLOCK &&
          !profile().GetPrefs()->IsManagedPreference(
              agent::kNavigationPermission)) {
        profile().GetPrefs()->SetInteger(agent::kNavigationPermission,
                                         static_cast<int>(command.value));
        if (!visible_) {
          PublishState();
        }
        accepted = true;
      }
      break;
    case KR_AGENT_COMMAND_REMOVE_SITE:
      if (const auto* site = Find(sites_, command.id)) {
        const std::string entry(String(*site, "entry"));
        accepted =
            agent::SetAgentSiteAllowlistEntry(profile(), entry, false, false);
      }
      break;
    case KR_AGENT_COMMAND_IMPORT_SKILL: {
      auto* tab = window_->GetActiveTabInterface();
      auto* contents = tab ? tab->GetContents() : nullptr;
      if (command.value <= 1 && contents && connection_->ready() && !picker_) {
        picker_ = std::make_unique<agent::SkillImportPicker>();
        accepted = picker_->Open(
            *web_contents(),
            command.value == 0 ? agent::SkillImportPicker::Kind::kFolder
                               : agent::SkillImportPicker::Kind::kZip,
            base::BindPostTaskToCurrentDefault(base::BindOnce(
                &AgentPanel::SkillSelected, weak_factory_.GetWeakPtr())));
        if (!accepted) {
          picker_.reset();
        }
      }
      break;
    }
    case KR_AGENT_COMMAND_SAVE_SKILL:
    case KR_AGENT_COMMAND_SET_SKILL_PROVIDERS:
    case KR_AGENT_COMMAND_RESET_SKILL:
    case KR_AGENT_COMMAND_REMOVE_SKILL:
    case KR_AGENT_COMMAND_APPROVE_SKILL: {
      const auto* skill = Find(skills_, command.id);
      if (!skill &&
          (command.id != 0 || command.kind != KR_AGENT_COMMAND_SAVE_SKILL)) {
        break;
      }
      base::DictValue params;
      params.Set("id", skill ? String(*skill, "id") : "new");
      if (command.kind == KR_AGENT_COMMAND_SAVE_SKILL ||
          command.kind == KR_AGENT_COMMAND_SET_SKILL_PROVIDERS) {
        base::ListValue instances;
        for (size_t index = 0; index < command.provider_count; ++index) {
          const auto* instance = Find(providers_, command.providers[index]);
          if (!instance) {
            return KR_ERR_INVALID_ARGUMENT;
          }
          instances.Append(String(*instance, "instance_id"));
        }
        // Supply the full catalogue so unselected built-in defaults can be
        // disabled.
        base::ListValue all_instances;
        for (const auto& [id, instance] : providers_) {
          all_instances.Append(String(instance, "instance_id"));
        }
        params.Set("instances", std::move(instances));
        params.Set("all_instances", std::move(all_instances));
      }
      switch (command.kind) {
        case KR_AGENT_COMMAND_SAVE_SKILL:
          params.Set("name", String(command.name));
          params.Set("description", String(command.description));
          params.Set("instructions", String(command.text));
          accepted = MutateSkill("skills.save", std::move(params));
          break;
        case KR_AGENT_COMMAND_SET_SKILL_PROVIDERS:
          accepted = MutateSkill("skills.set_providers", std::move(params));
          break;
        case KR_AGENT_COMMAND_RESET_SKILL:
          accepted = MutateSkill("skills.reset", std::move(params));
          break;
        case KR_AGENT_COMMAND_REMOVE_SKILL:
          accepted = MutateSkill("skills.remove", std::move(params));
          break;
        case KR_AGENT_COMMAND_APPROVE_SKILL:
          accepted = MutateSkill("skills.approve", std::move(params));
          break;
      }
      break;
    }
    default:
      break;
  }
  return accepted ? KR_OK : KR_ERR_INVALID_ARGUMENT;
}

bool AgentPanel::Start(const KrAgentCommand& command, std::string_view resume) {
  const auto* provider = Find(providers_, command.provider);
  auto* tab = window_->GetActiveTabInterface();
  if (!provider || !tab || saved_pending_ || !connection_->ready() ||
      !Bool(*provider, "installed") || !Bool(*provider, "signed_in") ||
      ((!IsBrowserOnly(*provider) || HasWebAccess(*provider)) &&
       !Consented(profile(), *provider))) {
    return false;
  }
  auto launch = ProviderLaunch(*provider);
  if (!launch) {
    return false;
  }
  agent::SessionOptions options;
  options.provider = String(*provider, "instance_id");
  options.model = String(command.model);
  std::tie(options.reasoning_option, options.reasoning) =
      Reasoning(*provider, options.model, command);
  options.prompt = String(command.text);
  options.resume_session_id = resume;
  if (resume.empty()) {
    options.context_tab = context_tab_;
  }
  options.allow_actions = command.flags & KR_AGENT_START_ALLOW_ACTIONS;
  options.allow_research = command.flags & KR_AGENT_START_ALLOW_RESEARCH;
  options.allow_tab_list = command.flags & KR_AGENT_START_ALLOW_TAB_LIST;
  options.approved_program = std::move(launch->program);
  options.isolation = launch->isolation;
  options.unsandboxed_development_provider =
      String(*provider, "sandbox_mode") == "none" ||
      (String(*provider, "sandbox_mode").empty() &&
       String(*provider, "sandbox") == "not_sandboxed");
  options.restricted_provider = !IsBrowserOnly(*provider);
  options.provider_web_access = HasWebAccess(*provider);
  const unsigned generation = session_generation_ + 1;
  options.review = base::BindRepeating(&AgentPanel::ReviewAction,
                                       weak_factory_.GetWeakPtr(), generation);
  options.handoff = base::BindRepeating(&AgentPanel::ReviewHandoff,
                                        weak_factory_.GetWeakPtr(), generation);
  options.review_sign_in = base::BindRepeating(
      &AgentPanel::ReviewSignIn, weak_factory_.GetWeakPtr(), generation);
  options.suggest_prompt = base::BindRepeating(
      &AgentPanel::SuggestPrompt, weak_factory_.GetWeakPtr(), generation);
  if (!agent::StartAgentSession(
          *tab, std::move(options),
          base::BindRepeating(&AgentPanel::OnEvent, weak_factory_.GetWeakPtr(),
                              generation))) {
    return false;
  }
  ++session_generation_;
  context_tab_.reset();
  ++transcript_generation_;
  saved_card_.reset();
  showing_saved_ = false;
  ClearReviews();
  activity_.clear();
  activity_ids_.clear();
  if (const auto* ui = api();
      KR_HAS_SLOT(ui, KrShellUI, clear_agent_activity) &&
      ui->clear_agent_activity) {
    ui->clear_agent_activity(ui->self);
  }
  provider_ = command.provider;
  shown_session_ = resume;
  shown_id_ = resume.empty() ? 0 : Id(session_ids_, resume);
  model_ = String(command.model);
  const auto* record = Find(sessions_, shown_id_);
  title_ =
      record ? String(*record, "title") : String(command.text).substr(0, 256);
  status_ = KR_AGENT_STARTING;
  notice_.clear();
  error_.clear();
  SetDark(dark_);
  agent::SetAgentChatBounds(*window_, bounds_);
  PublishState();
  return true;
}

void AgentPanel::ResetShownSession() {
  context_tab_.reset();
  ++session_generation_;
  ++transcript_generation_;
  saved_pending_ = false;
  showing_saved_ = false;
  saved_card_.reset();
  agent::CloseAgentChat(*window_);
  ClearReviews();
  activity_.clear();
  activity_ids_.clear();
  if (const auto* ui = api();
      KR_HAS_SLOT(ui, KrShellUI, clear_agent_activity) &&
      ui->clear_agent_activity) {
    ui->clear_agent_activity(ui->self);
  }
  provider_ = 0;
  shown_id_ = 0;
  shown_session_.clear();
  model_.clear();
  title_.clear();
  notice_.clear();
  error_.clear();
  status_ = KR_AGENT_IDLE;
}

void AgentPanel::SuggestPrompt(unsigned generation, std::string text) {
  if (generation != session_generation_ || !visible_ || showing_saved_ ||
      !Enabled()) {
    return;
  }
  const auto* ui = api();
  if (KR_HAS_SLOT(ui, KrShellUI, suggest_agent_prompt) &&
      ui->suggest_agent_prompt) {
    ui->suggest_agent_prompt(ui->self, ToKr(text));
  }
}

void AgentPanel::OnEvent(unsigned generation, base::DictValue event) {
  if (generation != session_generation_ || !Enabled()) {
    return;
  }
  const auto* data = event.FindDict("data");
  if (!data) {
    return;
  }
  const auto name = String(event, "event");
  const auto message = String(*data, "message");
  if (name == "browser.tool_activity") {
    const auto key = String(*data, "id");
    if (!key.empty()) {
      const auto id = Id(activity_ids_, key);
      activity_.insert_or_assign(id, data->Clone());
      PublishActivity(id, *data);
    }
    return;
  }
  if (name == "session.identity") {
    shown_session_ = String(*data, "session_id");
    shown_id_ = Id(session_ids_, shown_session_);
    RefreshHistory();
  } else if (name == "session.user_content") {
    notice_ = message;
    PublishSites();
  } else if (name == "session.recoverable") {
    RefreshHistory();
  } else if (name == "session.done") {
    RefreshHistory();
  } else if (name == "session.handoff") {
    status_ = KR_AGENT_WAITING;
  } else if (name == "session.handoff_result") {
    status_ = KR_AGENT_RUNNING;
  } else if (name == "session.error" || name == "session.origin_boundary") {
    status_ = KR_AGENT_FAILED;
    error_ =
        message.empty() ? "The agent session could not continue." : message;
    ClearReviews();
  } else if (name == "session.state") {
    const auto state =
        String(*data, "state").empty() ? message : String(*data, "state");
    if (state == "starting") {
      status_ = KR_AGENT_STARTING;
    } else if (state == "running") {
      status_ = KR_AGENT_RUNNING;
    } else if (state == "waiting_for_user") {
      status_ = KR_AGENT_WAITING;
    } else if (state == "idle") {
      status_ = KR_AGENT_IDLE;
    } else if (state == "stopped" || state == "failed") {
      if (status_ != KR_AGENT_FAILED) {
        status_ = state == "stopped" ? KR_AGENT_STOPPED : KR_AGENT_FAILED;
      }
      ClearReviews();
      RefreshHistory();
    }
  } else {
    return;
  }
  PublishState();
}

void AgentPanel::OnControlEvent(base::DictValue event) {
  const auto name = String(event, "event");
  if (name == "provider.changed") {
    if (visible_) {
      RefreshProviders(false);
    }
    return;
  }
  if (name == "component.error") {
    error_ = "The signed AI component could not be opened.";
  } else if (name == "component.ready") {
    error_.clear();
    RefreshProviders(false);
  }
  PublishState();
}

void AgentPanel::ReviewAction(unsigned generation,
                              base::DictValue action,
                              Decision decision) {
  if (generation != session_generation_ || !visible_ || !Enabled()) {
    std::move(decision).Run(false);
    return;
  }
  const auto* arguments = action.FindDict("arguments");
  const auto tool = String(action, "tool");
  if (tool == "capture_page") {
    AddReview({KR_AGENT_REQUEST_ACTION,
               0,
               std::string(String(action, "title")),
               std::string(String(action, "detail")),
               std::string(String(action, "target")),
               {},
               std::move(decision)});
    return;
  }
  const auto url =
      arguments ? String(*arguments, "url") : String(action, "url");
  const auto destination = GURL(url);
  const bool navigation = Bool(action, "navigation");
  const bool user_content =
      destination.is_valid() &&
      agent::IsKnownUserContentSite(profile(), destination);
  const std::string always_allow(
      navigation && Bool(action, "can_always_allow") &&
              destination.SchemeIs("https") &&
              !profile().GetPrefs()->IsManagedPreference(agent::kSiteAllowlist)
          ? url::Origin::Create(destination).Serialize()
          : std::string());
  std::string detail = "Allow the browser action once?";
  if (const auto source = String(action, "source"); !source.empty()) {
    detail =
        base::StrCat({"The action includes information from ", source, "."});
  } else if (String(action, "reason") == "sensitive_destination") {
    detail = "The destination contains a potentially sensitive value.";
  } else if (!tool.empty()) {
    detail = base::StrCat({"The agent requested ", tool, "."});
  }
  if (arguments) {
    for (const char* key : {"query", "text", "value", "dom_node_id"}) {
      if (const auto value = String(*arguments, key); !value.empty()) {
        base::StrAppend(&detail, {"\n", key, ": ", value.substr(0, 2048)});
      } else if (auto node = arguments->FindInt(key)) {
        base::StrAppend(&detail,
                        {"\n", key, ": ", base::NumberToString(*node)});
      }
    }
  }
  if (user_content) {
    base::StrAppend(&detail, {"\n\n", agent::kUserContentWarning});
  }
  AddReview({static_cast<uint32_t>(navigation ? KR_AGENT_REQUEST_NAVIGATION
                                              : KR_AGENT_REQUEST_ACTION),
             Flag(user_content, KR_AGENT_REQUEST_USER_CONTENT) |
                 Flag(!always_allow.empty(), KR_AGENT_REQUEST_CAN_ALWAYS_ALLOW),
             navigation ? "Open a site" : "Review a browser action",
             std::move(detail), std::string(url), always_allow,
             std::move(decision)});
}

void AgentPanel::ReviewHandoff(unsigned generation,
                               std::string reason,
                               Decision decision) {
  if (generation != session_generation_ || !Enabled()) {
    std::move(decision).Run(false);
    return;
  }
  std::string detail =
      "Finish the step in the shared tab, then choose Continue.";
  if (reason == "password" || reason == "one_time_code" ||
      reason == "sign_in") {
    detail =
        "Finish signing in directly in the shared tab, then choose Continue.";
  }
  AddReview({KR_AGENT_REQUEST_HANDOFF,
             0,
             "Take over in the browser",
             std::move(detail),
             {},
             {},
             std::move(decision)});
}

void AgentPanel::ReviewSignIn(
    unsigned generation,
    std::string site,
    base::OnceCallback<void(agent::SignInDecision)> decision) {
  if (generation != session_generation_ || !visible_ || !Enabled()) {
    std::move(decision).Run(agent::SignInDecision::kDeny);
    return;
  }
  AddReview({KR_AGENT_REQUEST_SIGN_IN,
             KR_AGENT_REQUEST_CAN_ALLOW_IN_CHAT,
             base::StrCat({"Let the agent read ", site, " with your sign-in?"}),
             "The agent can read filtered content that is visible to your "
             "account on this site.",
             std::move(site),
             {},
             {},
             std::move(decision)});
}

void AgentPanel::AddReview(Review review) {
  const auto id = next_id_++;
  const auto [entry, inserted] = reviews_.emplace(id, std::move(review));
  if (inserted) {
    PublishReview(id, entry->second);
    status_ = KR_AGENT_WAITING;
    PublishState();
  }
}

void AgentPanel::PublishReview(KrAgentRecordId id, const Review& review) {
  const auto* ui = api();
  if (KR_HAS_SLOT(ui, KrShellUI, upsert_agent_request) &&
      ui->upsert_agent_request) {
    const KrAgentRequest value{sizeof(KrAgentRequest),
                               id,
                               review.kind,
                               review.flags,
                               ToKr(review.title),
                               ToKr(review.detail),
                               ToKr(review.target),
                               {}};
    ui->upsert_agent_request(ui->self, &value);
    published_reviews_.insert(id);
  }
}

void AgentPanel::ClearReviews() {
  auto reviews = std::exchange(reviews_, {});
  const auto* ui = api();
  for (auto& [id, request] : reviews) {
    if (KR_HAS_SLOT(ui, KrShellUI, remove_agent_request) &&
        ui->remove_agent_request) {
      ui->remove_agent_request(ui->self, id);
      published_reviews_.erase(id);
    }
    if (request.sign_in_decision) {
      std::move(request.sign_in_decision).Run(agent::SignInDecision::kDeny);
    } else {
      std::move(request.decision).Run(false);
    }
  }
}

KrResult AgentPanel::Respond(KrAgentRecordId id, uint32_t decision) {
  auto found = reviews_.find(id);
  if (!visible_ || !Enabled() || found == reviews_.end() ||
      decision > KR_AGENT_ALLOW_SITE_IN_CHAT) {
    return KR_ERR_INVALID_ARGUMENT;
  }
  auto& request = found->second;
  const bool sign_in = request.kind == KR_AGENT_REQUEST_SIGN_IN;
  if (decision == KR_AGENT_ALLOW_SITE_IN_CHAT && !sign_in) {
    return KR_ERR_INVALID_ARGUMENT;
  }
  if (decision == KR_AGENT_ALWAYS_ALLOW_SITE && !sign_in &&
      (request.always_allow.empty() ||
       !agent::SetAgentSiteAllowlistEntry(profile(), request.always_allow, true,
                                          true))) {
    return KR_ERR_INVALID_ARGUMENT;
  }
  const bool handoff = request.kind == KR_AGENT_REQUEST_HANDOFF;
  const bool stop_handoff = handoff && decision == KR_AGENT_DENY;
  auto response = std::move(request.decision);
  auto sign_in_response = std::move(request.sign_in_decision);
  reviews_.erase(found);
  const auto* ui = api();
  if (KR_HAS_SLOT(ui, KrShellUI, remove_agent_request) &&
      ui->remove_agent_request) {
    ui->remove_agent_request(ui->self, id);
    published_reviews_.erase(id);
  }
  if (stop_handoff) {
    agent::StopAgentTurn(*window_);
  } else if (!handoff && reviews_.empty()) {
    status_ = KR_AGENT_RUNNING;
  }
  if (sign_in_response) {
    using SignIn = agent::SignInDecision;
    std::move(sign_in_response)
        .Run(decision == KR_AGENT_DENY                 ? SignIn::kDeny
             : decision == KR_AGENT_ALLOW_SITE_IN_CHAT ? SignIn::kAllowInChat
                                                       : SignIn::kAllowOnce);
  } else {
    std::move(response).Run(decision != KR_AGENT_DENY);
  }
  PublishState();
  return KR_OK;
}

void AgentPanel::PublishActivity(KrAgentRecordId id,
                                 const base::DictValue& entry) {
  const auto* ui = api();
  if (!KR_HAS_SLOT(ui, KrShellUI, upsert_agent_activity) ||
      !ui->upsert_agent_activity) {
    return;
  }
  const auto state = String(entry, "state");
  const uint32_t status = state == "done"     ? KR_AGENT_ACTIVITY_DONE
                          : state == "failed" ? KR_AGENT_ACTIVITY_FAILED
                          : state == "denied" ? KR_AGENT_ACTIVITY_DENIED
                                              : KR_AGENT_ACTIVITY_RUNNING;
  const KrAgentActivity value{
      sizeof(KrAgentActivity), id, status, ToKr(String(entry, "tool")), {}};
  ui->upsert_agent_activity(ui->self, &value);
}

void AgentPanel::RefreshHistory() {
  if (visible_ && Enabled()) {
    agent::ListAgentSavedSessions(
        profile(),
        base::BindOnce(&AgentPanel::OnHistory, weak_factory_.GetWeakPtr(),
                       ++history_generation_));
  }
}

void AgentPanel::OnHistory(unsigned generation,
                           std::optional<base::ListValue> records) {
  const auto* ui = api();
  if (generation != history_generation_ || !ui || !records) {
    return;
  }
  RecordMap next;
  for (auto& value : *records) {
    if (!value.is_dict()) {
      continue;
    }
    auto& record = value.GetDict();
    auto key = String(record, "session_id");
    if (!key.empty()) {
      const auto id = Id(session_ids_, key);
      record.Set("active", key == shown_session_ && !showing_saved_ &&
                               agent::HasRunningAgentSession(*window_));
      next.emplace(id, std::move(value).TakeDict());
    }
  }
  Reconcile(
      sessions_, std::move(next),
      [this, ui](KrAgentRecordId id, const base::DictValue& entry) {
        if (!KR_HAS_SLOT(ui, KrShellUI, upsert_agent_session) ||
            !ui->upsert_agent_session) {
          return;
        }
        const auto provider = String(entry, "provider");
        const auto* known = Find(providers_, Id(provider_ids_, provider));
        const auto detail = known ? String(*known, "name") : provider;
        auto activity = base::ValueToTime(entry.Find("activity"));
        const KrAgentSession row{
            .struct_size = sizeof(KrAgentSession),
            .id = id,
            .flags = Flag(Bool(entry, "archived"), KR_AGENT_SESSION_ARCHIVED) |
                     Flag(!String(entry, "native_thread_id").empty(),
                          KR_AGENT_SESSION_RECOVERABLE) |
                     Flag(Bool(entry, "active"), KR_AGENT_SESSION_ACTIVE),
            .provider = provider.empty() ? 0 : Id(provider_ids_, provider),
            .updated = activity ? activity->InMillisecondsSinceUnixEpoch() : 0,
            .title = ToKr(String(entry, "title")),
            .detail = ToKr(detail)};
        ui->upsert_agent_session(ui->self, &row);
      },
      [ui](KrAgentRecordId id) {
        if (KR_HAS_SLOT(ui, KrShellUI, remove_agent_session) &&
            ui->remove_agent_session) {
          ui->remove_agent_session(ui->self, id);
        }
      });
  if (showing_saved_ && !sessions_.contains(shown_id_)) {
    ResetShownSession();
    PublishState();
  }
}

void AgentPanel::OnHistoryChanged(bool success) {
  if (!success) {
    error_ = "The browser could not update saved sessions.";
    PublishState();
    return;
  }
  RefreshHistory();
}

void AgentPanel::PublishSites() {
  const auto* ui = api();
  if (!ui) {
    return;
  }
  RecordMap next;
  auto records = agent::GetAgentSiteAllowlist(profile());
  for (auto& entry : *records.FindList("entries")) {
    if (entry.is_dict()) {
      const auto id = Id(site_ids_, String(entry.GetDict(), "entry"));
      next.insert_or_assign(id, std::move(entry).TakeDict());
    }
  }
  Reconcile(
      sites_, std::move(next),
      [ui](KrAgentRecordId id, const base::DictValue& entry) {
        if (KR_HAS_SLOT(ui, KrShellUI, upsert_agent_site) &&
            ui->upsert_agent_site) {
          const KrAgentSite value{
              sizeof(KrAgentSite), id,
              Flag(Bool(entry, "user_content"), KR_AGENT_SITE_USER_CONTENT),
              ToKr(String(entry, "entry"))};
          ui->upsert_agent_site(ui->self, &value);
        }
      },
      [ui](KrAgentRecordId id) {
        if (KR_HAS_SLOT(ui, KrShellUI, remove_agent_site) &&
            ui->remove_agent_site) {
          ui->remove_agent_site(ui->self, id);
        }
      });
}

bool AgentPanel::RefreshProviders(bool force) {
  return Enabled() && connection_->Request(
                          force ? "providers.refresh" : "providers.list", {},
                          base::BindOnce(&AgentPanel::OnProviderInventory,
                                         weak_factory_.GetWeakPtr()));
}

void AgentPanel::OnProviderInventory(std::optional<base::DictValue> result) {
  const auto* list = result ? result->FindList("providers") : nullptr;
  std::vector<std::string> instances;
  std::vector<agent::ProviderLaunchRequest> requests;
  bool needs_authorization = list && list->empty();
  bool complete_inventory = list && !list->empty();
  if (list) {
    for (const auto& value : *list) {
      const auto* record = value.GetIfDict();
      const auto instance =
          record ? String(*record, "instance_id") : std::string_view();
      if (!instance.empty() && instance.size() <= 256 &&
          !std::ranges::contains(instances, instance)) {
        instances.emplace_back(instance);
      } else {
        complete_inventory = false;
      }
      const auto mode =
          record ? String(*record, "sandbox_mode") : std::string_view();
      if (mode == "app_container" || mode == "lpac" || mode == "job") {
        needs_authorization = true;
        if (auto launch = ProviderLaunch(*record);
            launch && requests.size() < kMaxInstances) {
          requests.push_back(std::move(*launch));
        } else {
          complete_inventory = false;
          LOG(WARNING) << "Krypton Agent provider authorization skipped "
                          "[invalid_launch_description]";
        }
      }
    }
  }
  if (!complete_inventory) {
    // Valid entries can still be authorized. An incomplete list cannot retire
    // a provider from the profile.
    instances.clear();
  }
  if (!needs_authorization || !connection_->AuthorizeProviders(
                                  std::move(instances), std::move(requests),
                                  base::BindOnce(&AgentPanel::OnProviders,
                                                 weak_factory_.GetWeakPtr()))) {
    OnProviders(std::move(result));
  }
}

void AgentPanel::OnProviders(std::optional<base::DictValue> result) {
  const auto* list = result ? result->FindList("providers") : nullptr;
  if (!list) {
    error_ = "The provider catalogue is unavailable.";
    PublishState();
    return;
  }
  RecordMap next;
  unsigned order = 0;
  for (const auto& value : *list) {
    const auto* record = value.GetIfDict();
    if (!record || order >= kMaxInstances) {
      continue;
    }
    const auto key = String(*record, "instance_id");
    if (key.empty() || key.size() > 256) {
      continue;
    }
    next.insert_or_assign(
        Id(provider_ids_, key),
        record->Clone().Set("panel_order", static_cast<int>(order++)));
  }
  providers_ = std::move(next);
  error_.clear();
  PublishProviders();
  RefreshSkills();
  PublishState();
}

void AgentPanel::PublishProviders() {
  const auto* ui = api();
  if (!ui) {
    return;
  }
  RecordMap next;
  for (const auto& [id, provider] : providers_) {
    next.emplace(id, provider.Clone().Set("panel_consented",
                                          Consented(profile(), provider)));
  }
  Reconcile(
      published_providers_, std::move(next),
      [ui](KrAgentRecordId id, const base::DictValue& entry) {
        if (!KR_HAS_SLOT(ui, KrShellUI, upsert_agent_provider) ||
            !ui->upsert_agent_provider) {
          return;
        }
        std::vector<KrAgentModel> models;
        // Each model's reasoning levels; reserved so element pointers stay put.
        std::vector<std::vector<KrAgentChoice>> levels;
        if (const auto* list = entry.FindList("models")) {
          levels.reserve(list->size());
          for (const auto& value : *list) {
            if (const auto* model = value.GetIfDict()) {
              auto& choices = levels.emplace_back();
              if (const auto* reasoning = model->FindDict("reasoning")) {
                if (const auto* items = reasoning->FindList("choices")) {
                  for (const auto& item : *items) {
                    if (const auto* choice = item.GetIfDict()) {
                      choices.push_back({sizeof(KrAgentChoice),
                                         ToKr(String(*choice, "id")),
                                         ToKr(String(*choice, "name")),
                                         Bool(*choice, "default")});
                    }
                  }
                }
              }
              models.push_back(
                  {sizeof(KrAgentModel), ToKr(String(*model, "id")),
                   ToKr(String(*model, "name")), Bool(*model, "default"),
                   choices.data(), choices.size(), sizeof(KrAgentChoice)});
            }
          }
        }
        const auto detail = String(entry, "tier_reason");
        const auto capabilities = String(entry, "local_capabilities");
        const auto explanation =
            capabilities.empty() ? std::string(detail)
                                 : base::StrCat({detail, " ", capabilities});
        const KrAgentProvider value{
            .struct_size = sizeof(KrAgentProvider),
            .id = id,
            .tier = static_cast<uint32_t>(IsBrowserOnly(entry)
                                              ? KR_AGENT_TIER_BROWSER_ONLY
                                              : KR_AGENT_TIER_RESTRICTED),
            .flags =
                Flag(Bool(entry, "installed"), KR_AGENT_PROVIDER_INSTALLED) |
                Flag(Bool(entry, "signed_in"), KR_AGENT_PROVIDER_SIGNED_IN) |
                Flag(Bool(entry, "can_sign_in"),
                     KR_AGENT_PROVIDER_CAN_SIGN_IN) |
                Flag(String(entry, "sandbox_mode") != "job" &&
                         String(entry, "sandbox") == "sandboxed",
                     KR_AGENT_PROVIDER_SANDBOXED) |
                Flag(HasWebAccess(entry), KR_AGENT_PROVIDER_WEB_ACCESS) |
                Flag(Bool(entry, "panel_consented"),
                     KR_AGENT_PROVIDER_CONSENTED) |
                Flag(Bool(entry, "default"), KR_AGENT_PROVIDER_DEFAULT),
            .order =
                static_cast<uint32_t>(entry.FindInt("panel_order").value_or(0)),
            .name = ToKr(String(entry, "name")),
            .detail = ToKr(explanation),
            .status = ToKr(String(entry, "status")),
            .models = models.data(),
            .model_count = models.size(),
            .model_size = sizeof(KrAgentModel),
            .icon = ToKr(String(entry, "icon"))};
        ui->upsert_agent_provider(ui->self, &value);
      },
      [ui](KrAgentRecordId id) {
        if (KR_HAS_SLOT(ui, KrShellUI, remove_agent_provider) &&
            ui->remove_agent_provider) {
          ui->remove_agent_provider(ui->self, id);
        }
      });
}

void AgentPanel::RefreshSkills() {
  if (!visible_) {
    return;
  }
  base::ListValue instances;
  for (const auto& [id, record] : providers_) {
    instances.Append(String(record, "instance_id"));
  }
  connection_->Request(
      "skills.list", base::DictValue().Set("instances", std::move(instances)),
      base::BindOnce(&AgentPanel::OnSkills, weak_factory_.GetWeakPtr()));
}

void AgentPanel::OnSkills(std::optional<base::DictValue> result) {
  const auto* ui = api();
  const auto* list = result ? result->FindList("skills") : nullptr;
  if (!ui || !list) {
    return;
  }
  RecordMap next;
  for (const auto& value : *list) {
    if (const auto* entry = value.GetIfDict()) {
      const auto key = String(*entry, "id");
      if (!key.empty()) {
        next.insert_or_assign(Id(skill_ids_, key), entry->Clone());
      }
    }
  }
  Reconcile(
      skills_, std::move(next),
      [this, ui](KrAgentRecordId id, const base::DictValue& entry) {
        if (!KR_HAS_SLOT(ui, KrShellUI, upsert_agent_skill) ||
            !ui->upsert_agent_skill) {
          return;
        }
        std::vector<KrAgentProviderId> enabled;
        if (const auto* instances = entry.FindList("enabled_instances")) {
          for (const auto& item : *instances) {
            if (const auto* instance = item.GetIfString();
                instance && provider_ids_.contains(*instance)) {
              enabled.push_back(provider_ids_.at(*instance));
            }
          }
        }
        const auto origin = String(entry, "origin");
        const KrAgentSkill row{
            .struct_size = sizeof(KrAgentSkill),
            .id = id,
            .origin = static_cast<uint32_t>(
                origin == "built-in"   ? KR_AGENT_SKILL_BUILT_IN
                : origin == "imported" ? KR_AGENT_SKILL_IMPORTED
                                       : KR_AGENT_SKILL_USER),
            .flags = Flag(Bool(entry, "edited"), KR_AGENT_SKILL_EDITED) |
                     Flag(Bool(entry, "needs_review") ||
                              String(entry, "hash_state") == "changed",
                          KR_AGENT_SKILL_NEEDS_REVIEW) |
                     Flag(Bool(entry, "plugin"), KR_AGENT_SKILL_PLUGIN),
            .name = ToKr(String(entry, "name")),
            .description = ToKr(String(entry, "description")),
            .instructions = ToKr(String(entry, "instructions")),
            .ignored = ToKr(String(entry, "ignored")),
            .enabled = enabled.data(),
            .enabled_count = enabled.size()};
        ui->upsert_agent_skill(ui->self, &row);
      },
      [ui](KrAgentRecordId id) {
        if (KR_HAS_SLOT(ui, KrShellUI, remove_agent_skill) &&
            ui->remove_agent_skill) {
          ui->remove_agent_skill(ui->self, id);
        }
      });
}

bool AgentPanel::MutateSkill(std::string method, base::DictValue params) {
  return connection_->Request(
      std::move(method), std::move(params),
      base::BindOnce(&AgentPanel::OnMutation, weak_factory_.GetWeakPtr()));
}

void AgentPanel::SkillSelected(base::FilePath path) {
  picker_.reset();
  if (!path.empty() && Enabled()) {
    MutateSkill("skills.import",
                base::DictValue().Set("path", path.AsUTF8Unsafe()));
  }
}

void AgentPanel::OnMutation(std::optional<base::DictValue> result) {
  if (!result) {
    error_ = "The AI component rejected the settings request.";
    PublishState();
    return;
  }
  error_.clear();
  RefreshProviders(false);
  PublishState();
}

bool AgentPanel::OpenSaved(KrAgentRecordId id) {
  const auto* record = Find(sessions_, id);
  if (!record || saved_pending_ || agent::HasRunningAgentSession(*window_)) {
    return false;
  }
  auto card = agent::ChatCard::Create(profile(), {});
  if (!card) {
    return false;
  }
  const std::string key(String(*record, "session_id"));
  const unsigned generation = transcript_generation_ + 1;
  if (!agent::OpenAgentSavedTranscript(
          profile(), key, 0,
          base::BindOnce(&AgentPanel::OnSavedPage, weak_factory_.GetWeakPtr(),
                         generation, 0))) {
    return false;
  }
  ResetShownSession();
  saved_card_ = std::move(card);
  saved_items_.clear();
  saved_reply_id_.clear();
  saved_reply_.clear();
  saved_pending_ = true;
  showing_saved_ = true;
  shown_session_ = key;
  shown_id_ = id;
  provider_ = Id(provider_ids_, String(*record, "provider"));
  model_ = String(*record, "model");
  title_ = String(*record, "title");
  saved_card_->SetDark(dark_);
  agent::PlaceAgentChatCard(*saved_card_, *window_, bounds_);
  PublishState();
  return true;
}

bool AgentPanel::ReadSaved(int cursor) {
  return agent::OpenAgentSavedTranscript(
      profile(), shown_session_, cursor,
      base::BindOnce(&AgentPanel::OnSavedPage, weak_factory_.GetWeakPtr(),
                     transcript_generation_, cursor));
}

void AgentPanel::OnSavedPage(unsigned generation,
                             int cursor,
                             std::optional<base::DictValue> page) {
  if (generation != transcript_generation_ || !saved_card_ || !Enabled()) {
    return;
  }
  const auto* events = page ? page->FindList("events") : nullptr;
  if (!events) {
    saved_pending_ = false;
    error_ = "The saved transcript could not be opened.";
    PublishState();
    return;
  }
  // A turn's items are saved before its reply, and a long reply in parts.
  const auto flush_items = [this](int seq) {
    if (saved_items_.empty()) {
      return;
    }
    const auto id = base::StrCat({"assistant-", base::NumberToString(seq)});
    saved_card_->Append(id, "");
    for (auto& item : saved_items_) {
      saved_card_->SetItem(id, std::move(item));
    }
    saved_card_->Finish(id);
    saved_items_.clear();
  };
  for (const auto& event : *events) {
    const auto* entry = event.GetIfDict();
    const auto* data = entry ? entry->FindDict("data") : nullptr;
    if (!data) {
      continue;
    }
    const auto kind = String(*entry, "kind");
    const auto seq = entry->FindInt("seq").value_or(0);
    if (seq <= 0) {
      continue;
    }
    if (kind == "message.html" &&
        agent::ValidHtmlArguments(*data, /*preview=*/false)) {
      const auto id = base::StrCat({"render-", base::NumberToString(seq)});
      saved_card_->Append(id, "");
      saved_card_->ShowHtml(id, std::string(String(*data, "html")),
                            std::string(String(*data, "title")),
                            agent::kDefaultHtmlWidth,
                            data->FindInt("height").value_or(480));
      saved_card_->Finish(id);
      continue;
    }
    if (kind == "turn.item") {
      saved_items_.push_back(
          agent::TranscriptEntry{.id = std::string(String(*data, "item_id")),
                                 .kind = std::string(String(*data, "kind")),
                                 .status = std::string(String(*data, "status")),
                                 .title = std::string(String(*data, "title")),
                                 .detail = std::string(String(*data, "detail")),
                                 .text = std::string(String(*data, "text")),
                                 .output = std::string(String(*data, "output")),
                                 .offset = static_cast<uint32_t>(std::max(
                                     0, data->FindInt("offset").value_or(0)))});
      continue;
    }
    if (kind == "message.assistant" &&
        data->FindBool("continued").value_or(false) &&
        !saved_reply_id_.empty()) {
      saved_reply_.append(String(*data, "text"));
      saved_card_->Update(saved_reply_id_, saved_reply_);
      continue;
    }
    if (kind == "message.user" || kind == "message.assistant") {
      if (!saved_reply_id_.empty()) {
        saved_card_->Finish(saved_reply_id_);
        saved_reply_id_.clear();
        saved_reply_.clear();
      }
      if (kind == "message.user") {
        flush_items(seq);
      }
      const auto id =
          base::StrCat({kind == "message.user" ? "user-" : "assistant-",
                        base::NumberToString(seq)});
      saved_card_->Append(id, String(*data, "text"));
      if (kind == "message.assistant") {
        for (auto& item : saved_items_) {
          saved_card_->SetItem(id, std::move(item));
        }
        saved_items_.clear();
        // Later parts of this reply extend it until the next message.
        saved_reply_id_ = id;
        saved_reply_ = std::string(String(*data, "text"));
      } else {
        saved_card_->Finish(id);
      }
    }
  }
  const int continuation = page->FindInt("continuation").value_or(0);
  if (continuation > cursor) {
    if (ReadSaved(continuation)) {
      return;
    }
    error_ = "The rest of the saved transcript could not be opened.";
  }
  if (!saved_reply_id_.empty()) {
    saved_card_->Finish(saved_reply_id_);
    saved_reply_id_.clear();
    saved_reply_.clear();
  }
  // Items of a last turn that ended without a reply.
  flush_items(std::numeric_limits<int>::max());
  saved_pending_ = false;
  PublishState();
}

}  // namespace krypton_ui
