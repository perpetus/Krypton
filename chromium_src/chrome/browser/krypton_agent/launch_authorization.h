#ifndef CHROME_BROWSER_KRYPTON_AGENT_LAUNCH_AUTHORIZATION_H_
#define CHROME_BROWSER_KRYPTON_AGENT_LAUNCH_AUTHORIZATION_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted.h"
#include "base/process/process.h"
#include "base/values.h"
#include "base/win/sid.h"

namespace krypton_agent {

struct ComponentPin;

enum class ProviderIsolation { kAppContainer, kLpac, kJob };

struct ProviderProgram {
  base::FilePath executable;
  base::FilePath script;
  base::FilePath package_root;
  base::FilePath cli_directory;
  std::string driver;
};

struct ProviderLaunchRequest {
  std::string instance;
  ProviderProgram program;
  ProviderIsolation isolation = ProviderIsolation::kAppContainer;
  std::string usage_database_path;
};

struct ProviderSandbox {
  base::FilePath program;
  base::FilePath working_directory;
  base::FilePath config_directory;
  base::FilePath container_directory;
  base::FilePath policy_directory;
  base::FilePath grant_directory;
  base::FilePath cli_directory;
  base::FilePath script;
  base::FilePath package_root;
  std::string provider_id;
  std::wstring name;
  std::optional<base::win::Sid> sid;
  ProviderIsolation isolation;
  base::ScopedClosureRunner lifetime;
};

std::string_view IsolationName(ProviderIsolation isolation);
bool PrepareAgentPrivateDirectories(const base::FilePath& local_app_data);
bool IsLocalProviderUsageDatabase(std::string_view path);

// `program` is the selected instance's executable resolved by the trusted
// provider registry. Call on a blocking sequence before opening its tool pipe.
std::optional<ProviderSandbox> PrepareProviderSandbox(
    ProviderProgram program,
    std::string_view profile_token,
    std::string_view provider_instance,
    ProviderIsolation isolation,
    std::string_view* failure = nullptr);
void RemoveProviderSandboxes(std::string_view profile_token);
void ReconcileProviderSandboxes(const base::FilePath& local_app_data,
                                std::string_view profile_token,
                                const std::vector<std::string>& instances,
                                base::ListValue* diagnostics);
bool RemoveAllProviderSandboxes(const base::FilePath& local_app_data);

// Owns the authorization pipe for one supervisor and one provider instance.
class LaunchAuthorization {
 public:
  static std::unique_ptr<LaunchAuthorization> Start(
      base::Process supervisor,
      const base::FilePath& component_directory,
      const ComponentPin& pin,
      ProviderSandbox sandbox,
      std::string_view* failure = nullptr);

  ~LaunchAuthorization();
  LaunchAuthorization(const LaunchAuthorization&) = delete;
  LaunchAuthorization& operator=(const LaunchAuthorization&) = delete;

  // Parameters for the trusted sidecar's guard, never sent through the relay.
  base::DictValue Describe() const;

 private:
  class State;
  explicit LaunchAuthorization(scoped_refptr<State> state);
  scoped_refptr<State> state_;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_LAUNCH_AUTHORIZATION_H_
