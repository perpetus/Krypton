#ifndef CHROME_BROWSER_KRYPTON_AGENT_SIDECAR_PROCESS_H_
#define CHROME_BROWSER_KRYPTON_AGENT_SIDECAR_PROCESS_H_

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/values.h"
#include "chrome/browser/krypton_agent/launch_authorization.h"

namespace krypton_agent {
struct ComponentPin;

// Stdio endpoint owned by the host. Callbacks run on the creating sequence.
class SidecarProcess {
 public:
  using Message = base::RepeatingCallback<void(base::DictValue)>;
  using Closed = base::OnceClosure;
  using Ready = base::OnceCallback<void(base::DictValue)>;

  static std::unique_ptr<SidecarProcess> Start(
      const base::FilePath& path,
      const ComponentPin& pin,
      std::optional<ProviderSandbox> sandbox,
      Message on_message,
      Closed on_closed,
      Ready on_ready);
  ~SidecarProcess();
  SidecarProcess(const SidecarProcess&) = delete;
  SidecarProcess& operator=(const SidecarProcess&) = delete;

  bool Send(base::DictValue message);
  bool AuthorizeProviders(std::string profile_token,
                          std::vector<std::string> instances,
                          std::vector<ProviderLaunchRequest> requests,
                          Ready done);

 private:
  class State;
  explicit SidecarProcess(scoped_refptr<State> state);
  scoped_refptr<State> state_;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_SIDECAR_PROCESS_H_
