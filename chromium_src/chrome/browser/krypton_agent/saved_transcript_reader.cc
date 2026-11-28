#include "chrome/browser/krypton_agent/saved_transcript_reader.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/krypton_agent/sidecar_process.h"
#include "chrome/browser/profiles/profile.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace krypton_agent {

std::optional<base::DictValue> ExtractTranscriptPage(base::DictValue response) {
  auto result = response.Extract("result");
  if (!result || !result->is_dict() || !result->GetDict().FindList("events")) {
    return std::nullopt;
  }
  auto page = std::move(*result).TakeDict();
  base::DictValue transcript;
  transcript.Set("events", std::move(*page.Extract("events")));
  if (auto continuation = page.FindInt("continuation");
      continuation && *continuation >= 0) {
    transcript.Set("continuation", *continuation);
  }
  transcript.Set("recoverable", page.FindDict("recoverable") != nullptr);
  return transcript;
}

SavedTranscriptReader::SavedTranscriptReader() = default;
SavedTranscriptReader::~SavedTranscriptReader() = default;

bool SavedTranscriptReader::Open(Profile& profile,
                                 std::string session_id,
                                 int cursor,
                                 SavedTranscriptReply reply) {
  if (busy() || !reply || !profile.IsRegularProfile() ||
      !IsProductionAgentEnabled() || !PinnedAgentComponent() ||
      session_id.empty() || session_id.size() > 128 || cursor < 0) {
    return false;
  }
  auto* os_crypt = g_browser_process->os_crypt_async();
  if (!os_crypt) {
    return false;
  }
  reply_ = std::move(reply);
  profile_path_ = profile.GetPath();
  session_id_ = std::move(session_id);
  timeout_.Start(FROM_HERE, base::Seconds(20),
                 base::BindOnce(&SavedTranscriptReader::Cancel,
                                weak_factory_.GetWeakPtr()));

  // A saved transcript requires an already installed, verified component.
  // Opening history never starts a component download.
  component_.Ensure(nullptr, base::BindOnce(&SavedTranscriptReader::OnComponent,
                                            weak_factory_.GetWeakPtr()));
  if (busy()) {
    os_crypt->GetInstance(base::BindOnce(&SavedTranscriptReader::OnEncryptor,
                                         weak_factory_.GetWeakPtr(), cursor));
  }
  return true;
}

void SavedTranscriptReader::OnEncryptor(
    int cursor,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (!HistoryTaskRunner()->PostTaskAndReplyWithResult(
          FROM_HERE,
          base::BindOnce(&PrepareTranscriptRequest, profile_path_,
                         std::move(encryptor), session_id_, cursor),
          base::BindOnce(&SavedTranscriptReader::OnPrepared,
                         weak_factory_.GetWeakPtr()))) {
    Cancel();
  }
}

void SavedTranscriptReader::OnPrepared(
    std::optional<TranscriptRequest> request) {
  if (!request || !IsProductionAgentEnabled()) {
    Cancel();
    return;
  }
  request_ = std::move(request);
  if (sidecar_ready_) {
    OnReady({});
  }
}

void SavedTranscriptReader::OnComponent(
    base::expected<base::FilePath, AgentComponent::Error> path) {
  const auto* pin = PinnedAgentComponent();
  if (!path || !pin || !IsProductionAgentEnabled()) {
    Cancel();
    return;
  }
  sidecar_ = SidecarProcess::Start(
      *path, *pin, std::nullopt,
      base::BindRepeating(&SavedTranscriptReader::OnMessage,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&SavedTranscriptReader::Cancel,
                     weak_factory_.GetWeakPtr()),
      base::BindOnce(&SavedTranscriptReader::OnReady,
                     weak_factory_.GetWeakPtr()));
  if (!sidecar_) {
    Cancel();
  }
}

void SavedTranscriptReader::OnReady(base::DictValue authorization) {
  const auto* pin = PinnedAgentComponent();
  if (!pin || !IsProductionAgentEnabled() || !authorization.empty()) {
    Cancel();
    return;
  }
  sidecar_ready_ = true;
  if (!request_) {
    return;
  }
  if (!Send("b-1", "hello",
            base::DictValue()
                .Set("protocol_version", pin->protocol)
                .Set("history", std::move(request_->history)))) {
    Cancel();
  }
}

void SavedTranscriptReader::OnMessage(base::DictValue message) {
  const auto* type = message.FindString("type");
  const auto* id = message.FindString("id");
  if (!IsProductionAgentEnabled() || !type || *type != "response" || !id) {
    Cancel();
    return;
  }
  if (!hello_complete_) {
    const auto* pin = PinnedAgentComponent();
    const auto* result = message.FindDict("result");
    const auto* version =
        result ? result->FindString("sidecar_version") : nullptr;
    if (*id != "b-1" || !pin || !result || !version ||
        *version != pin->version ||
        result->FindInt("protocol_version") != pin->protocol ||
        result->FindBool("history_v1") != true || !request_ ||
        !Send("b-2", "session.open", std::move(request_->params))) {
      Cancel();
      return;
    }
    hello_complete_ = true;
    request_.reset();
    return;
  }
  Finish(*id == "b-2" ? ExtractTranscriptPage(std::move(message))
                      : std::nullopt);
}

bool SavedTranscriptReader::Send(std::string_view id,
                                 std::string_view method,
                                 base::DictValue params) {
  return sidecar_ && sidecar_->Send(base::DictValue()
                                        .Set("v", 1)
                                        .Set("type", "request")
                                        .Set("id", id)
                                        .Set("method", method)
                                        .Set("params", std::move(params)));
}

void SavedTranscriptReader::CancelProfile(const base::FilePath& profile) {
  if (busy() && profile_path_ == profile) {
    Cancel();
  }
}

void SavedTranscriptReader::CancelDeleted(const base::FilePath& profile,
                                          const base::ListValue& ids) {
  if (busy() && profile_path_ == profile && ids.contains(session_id_)) {
    Cancel();
  }
}

void SavedTranscriptReader::Cancel() {
  Finish(std::nullopt);
}

void SavedTranscriptReader::Finish(std::optional<base::DictValue> transcript) {
  weak_factory_.InvalidateWeakPtrs();
  timeout_.Stop();
  sidecar_.reset();
  component_.Cancel();
  request_.reset();
  profile_path_.clear();
  session_id_.clear();
  hello_complete_ = false;
  sidecar_ready_ = false;
  if (reply_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(reply_), std::move(transcript)));
  }
}

}  // namespace krypton_agent
