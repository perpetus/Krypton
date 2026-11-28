#include "chrome/browser/krypton_agent/panel_connection.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_agent/sidecar_process.h"
#include "chrome/browser/profiles/profile.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace krypton_agent {

PanelConnection::PanelConnection(Profile& profile, Events events)
    : profile_(profile), events_(std::move(events)) {}
PanelConnection::~PanelConnection() = default;

bool PanelConnection::Request(std::string method,
                              base::DictValue params,
                              Reply reply) {
  if (!profile_->IsRegularProfile() || !IsProductionAgentEnabled() ||
      !PinnedAgentComponent() || queue_.size() >= 16 || !reply ||
      (method != "providers.list" && method != "providers.refresh" &&
       method != "providers.authorize" && method != "auth.login" &&
       !method.starts_with("skills."))) {
    return false;
  }
  queue_.push_back({std::move(method), std::move(params), std::move(reply)});
  if (hello_) {
    Dispatch();
  } else if (!process_ && !installing_) {
    Connect();
  }
  return true;
}

bool PanelConnection::AuthorizeProviders(
    std::vector<std::string> instances,
    std::vector<ProviderLaunchRequest> requests,
    Reply reply) {
  if (!hello_ || !process_ || profile_token_.empty() || preparing_providers_ ||
      !IsProductionAgentEnabled()) {
    return false;
  }
  preparing_providers_ = process_->AuthorizeProviders(
      profile_token_, std::move(instances), std::move(requests),
      base::BindOnce(&PanelConnection::OnProvidersPrepared,
                     weak_factory_.GetWeakPtr(), std::move(reply)));
  return preparing_providers_;
}

void PanelConnection::OnProvidersPrepared(Reply reply, base::DictValue params) {
  preparing_providers_ = false;
  queue_.push_front(
      {"providers.authorize", std::move(params), std::move(reply)});
  Dispatch();
}

bool PanelConnection::UpdateComponent() {
  if (!profile_->IsRegularProfile() || !IsProductionAgentEnabled() ||
      !PinnedAgentComponent() || installing_ || !queue_.empty()) {
    return false;
  }
  Close();
  installing_ = true;
  component_.Ensure(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess(),
      base::BindPostTaskToCurrentDefault(base::BindOnce(
          &PanelConnection::OnComponent, weak_factory_.GetWeakPtr())),
      base::BindRepeating(&PanelConnection::OnProgress,
                          weak_factory_.GetWeakPtr()));
  Changed();
  return true;
}

void PanelConnection::Connect() {
  installing_ = true;
  component_.Ensure(
      nullptr,
      base::BindPostTaskToCurrentDefault(base::BindOnce(
          &PanelConnection::OnComponent, weak_factory_.GetWeakPtr())),
      base::BindRepeating(&PanelConnection::OnProgress,
                          weak_factory_.GetWeakPtr()));
  Changed();
}

void PanelConnection::OnProgress(bool verifying, double progress) {
  verifying_ = verifying;
  progress_ = progress;
  Changed();
}

void PanelConnection::OnComponent(
    base::expected<base::FilePath, AgentComponent::Error> path) {
  installing_ = false;
  verifying_ = false;
  const auto* pin = PinnedAgentComponent();
  if (!path || !pin || !IsProductionAgentEnabled()) {
    Fail();
    return;
  }
  verified_ = true;
  process_ = SidecarProcess::Start(
      *path, *pin, std::nullopt,
      base::BindRepeating(&PanelConnection::OnMessage,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&PanelConnection::Fail, weak_factory_.GetWeakPtr()),
      base::BindOnce(&PanelConnection::OnReady, weak_factory_.GetWeakPtr()));
  if (!process_) {
    Fail();
    return;
  }
  timeout_.Start(
      FROM_HERE, base::Seconds(20),
      base::BindOnce(&PanelConnection::Fail, weak_factory_.GetWeakPtr()));
  Changed();
}

void PanelConnection::OnReady(base::DictValue authorization) {
  auto* crypt = g_browser_process->os_crypt_async();
  if (!authorization.empty() || !crypt || !IsProductionAgentEnabled()) {
    Fail();
    return;
  }
  crypt->GetInstance(base::BindOnce(&PanelConnection::OnEncryptor,
                                    weak_factory_.GetWeakPtr()));
}

void PanelConnection::OnEncryptor(
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (!IsProductionAgentEnabled() ||
      !HistoryTaskRunner()->PostTaskAndReplyWithResult(
          FROM_HERE,
          base::BindOnce(&PrepareSettingsContext, profile_->GetPath(),
                         std::move(encryptor)),
          base::BindOnce(&PanelConnection::OnContext,
                         weak_factory_.GetWeakPtr()))) {
    Fail();
  }
}

void PanelConnection::OnContext(std::optional<HistoryContext> context) {
  const auto* pin = PinnedAgentComponent();
  if (context) {
    profile_token_ = context->profile_token;
  }
  if (!context || !pin || !IsProductionAgentEnabled() ||
      !Send("b-panel-hello", "hello",
            base::DictValue()
                .Set("protocol_version", pin->protocol)
                .Set("profile_token", std::move(context->profile_token))
                .Set("diagnostics", pin->unsigned_development)
                .Set("credentials", std::move(context->credentials)))) {
    Fail();
  }
}

void PanelConnection::OnMessage(base::DictValue message) {
  const auto* type = message.FindString("type");
  const auto* id = message.FindString("id");
  const auto* result = message.FindDict("result");
  if (!type || !IsProductionAgentEnabled()) {
    Fail();
    return;
  }
  if (!hello_) {
    const auto* pin = PinnedAgentComponent();
    const auto* version =
        result ? result->FindString("sidecar_version") : nullptr;
    if (*type != "response" || !id || *id != "b-panel-hello" || !pin ||
        !version || *version != pin->version ||
        result->FindInt("protocol_version") != pin->protocol) {
      Fail();
      return;
    }
    hello_ = true;
    timeout_.Stop();
    if (queue_.empty()) {
      events_.Run(base::DictValue().Set("event", "component.ready"));
    }
    Dispatch();
    return;
  }
  if (*type == "event") {
    const auto* event = message.FindString("event");
    if (event && *event == "provider.changed") {
      events_.Run(std::move(message));
    }
    return;
  }
  if (*type != "response" || !id || *id != request_id_ || queue_.empty()) {
    return;
  }
  timeout_.Stop();
  request_id_.clear();
  auto reply = std::move(queue_.front().reply);
  queue_.pop_front();
  auto weak = weak_factory_.GetWeakPtr();
  std::move(reply).Run(result ? std::make_optional(result->Clone())
                              : std::nullopt);
  if (weak) {
    weak->Dispatch();
  }
}

void PanelConnection::Dispatch() {
  if (!hello_ || preparing_providers_ || queue_.empty() ||
      !request_id_.empty()) {
    return;
  }
  auto& request = queue_.front();
  request_id_ = base::StrCat({"b-panel-", base::NumberToString(++sequence_)});
  if (!Send(request_id_, request.method, std::move(request.params))) {
    Fail();
    return;
  }
  timeout_.Start(
      FROM_HERE,
      request.method == "auth.login" ? base::Minutes(5) : base::Seconds(20),
      base::BindOnce(&PanelConnection::Fail, weak_factory_.GetWeakPtr()));
}

bool PanelConnection::Send(std::string_view id,
                           std::string_view method,
                           base::DictValue params) {
  return process_ && process_->Send(base::DictValue()
                                        .Set("v", 1)
                                        .Set("type", "request")
                                        .Set("id", id)
                                        .Set("method", method)
                                        .Set("params", std::move(params)));
}

void PanelConnection::Close() {
  weak_factory_.InvalidateWeakPtrs();
  timeout_.Stop();
  component_.Cancel();
  process_.reset();
  queue_.clear();
  request_id_.clear();
  profile_token_.clear();
  preparing_providers_ = false;
  hello_ = false;
  installing_ = false;
  verifying_ = false;
  verified_ = false;
  progress_ = 0;
}

void PanelConnection::Fail() {
  auto queue = std::move(queue_);
  Close();
  auto weak = weak_factory_.GetWeakPtr();
  events_.Run(base::DictValue().Set("event", "component.error"));
  for (auto& request : queue) {
    if (!weak) {
      return;
    }
    std::move(request.reply).Run(std::nullopt);
  }
}

void PanelConnection::Changed() {
  events_.Run(base::DictValue().Set("event", "component.changed"));
}

}  // namespace krypton_agent
