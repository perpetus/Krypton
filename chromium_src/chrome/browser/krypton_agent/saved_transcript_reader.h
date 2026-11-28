#ifndef CHROME_BROWSER_KRYPTON_AGENT_SAVED_TRANSCRIPT_READER_H_
#define CHROME_BROWSER_KRYPTON_AGENT_SAVED_TRANSCRIPT_READER_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/krypton_agent/agent_session.h"
#include "chrome/browser/krypton_agent/history_index.h"

class Profile;

namespace os_crypt_async {
class Encryptor;
}

namespace krypton_agent {

class SidecarProcess;

// Outside a live session, open a saved transcript over a short-lived, pinned
// supervisor connection. This connection never grants a provider launch or
// creates an actor task or relay.
class SavedTranscriptReader {
 public:
  SavedTranscriptReader();
  ~SavedTranscriptReader();

  bool Open(Profile& profile,
            std::string session_id,
            int cursor,
            SavedTranscriptReply reply);
  void CancelProfile(const base::FilePath& profile);
  void CancelDeleted(const base::FilePath& profile, const base::ListValue& ids);
  void Cancel();
  bool busy() const { return !!reply_; }

 private:
  void OnEncryptor(int cursor,
                   scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void OnPrepared(std::optional<TranscriptRequest> request);
  void OnComponent(base::expected<base::FilePath, AgentComponent::Error> path);
  void OnReady(base::DictValue authorization);
  void OnMessage(base::DictValue message);
  bool Send(std::string_view id,
            std::string_view method,
            base::DictValue params);
  void Finish(std::optional<base::DictValue> transcript);

  AgentComponent component_;
  std::unique_ptr<SidecarProcess> sidecar_;
  base::FilePath profile_path_;
  std::string session_id_;
  std::optional<TranscriptRequest> request_;
  SavedTranscriptReply reply_;
  base::OneShotTimer timeout_;
  bool hello_complete_ = false;
  bool sidecar_ready_ = false;
  base::WeakPtrFactory<SavedTranscriptReader> weak_factory_{this};
};

std::optional<base::DictValue> ExtractTranscriptPage(base::DictValue response);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_SAVED_TRANSCRIPT_READER_H_
