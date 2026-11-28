#ifndef CHROME_BROWSER_KRYPTON_AGENT_PANEL_CONNECTION_H_
#define CHROME_BROWSER_KRYPTON_AGENT_PANEL_CONNECTION_H_

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/krypton_agent/history_index.h"
#include "chrome/browser/krypton_agent/launch_authorization.h"

class Profile;

namespace os_crypt_async {
class Encryptor;
}

namespace krypton_agent {
class SidecarProcess;

// Verified stdio channel for native Settings. It has no task, relay, or
// launch authorization for sessions.
class PanelConnection {
 public:
  using Reply = base::OnceCallback<void(std::optional<base::DictValue>)>;
  using Events = base::RepeatingCallback<void(base::DictValue)>;

  PanelConnection(Profile& profile, Events events);
  ~PanelConnection();

  bool Request(std::string method, base::DictValue params, Reply reply);
  bool AuthorizeProviders(std::vector<std::string> instances,
                          std::vector<ProviderLaunchRequest> requests,
                          Reply reply);
  bool UpdateComponent();
  void Close();

  bool busy() const { return installing_; }
  bool verifying() const { return verifying_; }
  double progress() const { return progress_; }
  bool ready() const { return verified_; }

 private:
  struct RequestData {
    std::string method;
    base::DictValue params;
    Reply reply;
  };
  void Connect();
  void OnComponent(base::expected<base::FilePath, AgentComponent::Error> path);
  void OnProgress(bool verifying, double progress);
  void OnEncryptor(scoped_refptr<os_crypt_async::Encryptor> encryptor);
  void OnContext(std::optional<HistoryContext> context);
  void OnReady(base::DictValue authorization);
  void OnMessage(base::DictValue message);
  void OnProvidersPrepared(Reply reply, base::DictValue authorizations);
  void Dispatch();
  bool Send(std::string_view id,
            std::string_view method,
            base::DictValue params);
  void Fail();
  void Changed();

  const raw_ref<Profile> profile_;
  Events events_;
  AgentComponent component_;
  std::unique_ptr<SidecarProcess> process_;
  std::deque<RequestData> queue_;
  base::OneShotTimer timeout_;
  std::string request_id_;
  std::string profile_token_;
  bool preparing_providers_ = false;
  unsigned sequence_ = 0;
  double progress_ = 0;
  bool hello_ = false;
  bool installing_ = false;
  bool verifying_ = false;
  bool verified_ = false;
  base::WeakPtrFactory<PanelConnection> weak_factory_{this};
};

}  // namespace krypton_agent
#endif  // CHROME_BROWSER_KRYPTON_AGENT_PANEL_CONNECTION_H_
