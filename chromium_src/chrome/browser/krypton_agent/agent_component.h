#ifndef CHROME_BROWSER_KRYPTON_AGENT_AGENT_COMPONENT_H_
#define CHROME_BROWSER_KRYPTON_AGENT_AGENT_COMPONENT_H_

#include <cstdint>
#include <memory>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace krypton_agent {

struct ComponentPin {
  std::string_view version;
  std::string_view url;
  uint64_t size;
  std::string_view sha256;
  int protocol;
  uint64_t launcher_size;
  std::string_view launcher_sha256;
  bool unsigned_development = false;
};

const ComponentPin* PinnedAgentComponent();
bool IsProductionAgentEnabled();
bool VerifyAgentImage(const base::FilePath& image, const ComponentPin& pin);
bool VerifyAgentLauncher(const base::FilePath& directory,
                         const ComponentPin& pin);

class AgentComponent {
 public:
  enum class Error {
    kDisabled,
    kNoPinnedRelease,
    kBusy,
    kDirectory,
    kDownload,
    kVerification,
  };

  using Callback =
      base::OnceCallback<void(base::expected<base::FilePath, Error>)>;

  using Progress =
      base::RepeatingCallback<void(bool verifying, double fraction)>;

  AgentComponent();
  ~AgentComponent();
  AgentComponent(const AgentComponent&) = delete;
  AgentComponent& operator=(const AgentComponent&) = delete;

  void Ensure(scoped_refptr<network::SharedURLLoaderFactory> factory,
              Callback callback,
              Progress progress = {});
  void Cancel();

 private:
  struct Paths;
  void OnPrepared(scoped_refptr<network::SharedURLLoaderFactory> factory,
                  const ComponentPin* pin,
                  std::unique_ptr<Paths> paths);
  void OnDownloaded(const ComponentPin* pin, base::FilePath file);
  void Finish(base::expected<base::FilePath, Error> result);

  std::unique_ptr<Paths> paths_;
  std::unique_ptr<network::SimpleURLLoader> download_;
  Callback callback_;
  Progress progress_;
  base::WeakPtrFactory<AgentComponent> weak_factory_{this};
};

// Called on the browser sequence after stopping the agent and closing its
// pipes. File removal runs on the thread pool.
void RemoveAgentComponent();

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_AGENT_COMPONENT_H_
