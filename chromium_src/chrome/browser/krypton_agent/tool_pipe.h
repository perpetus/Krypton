#ifndef CHROME_BROWSER_KRYPTON_AGENT_TOOL_PIPE_H_
#define CHROME_BROWSER_KRYPTON_AGENT_TOOL_PIPE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/values.h"
#include "base/win/sid.h"

namespace krypton_agent {

struct ComponentPin;

class ToolPipe {
 public:
  enum class ClientDecision {
    kAccepted,
    kWrongAppContainerSid,
    kWrongImage,
    kFailedSignatureCheck,
    kBadToken,
  };
  using ReportDecision = base::RepeatingCallback<void(ClientDecision)>;
  using Reply = base::OnceCallback<void(base::DictValue)>;
  using Handler = base::RepeatingCallback<void(base::DictValue, Reply)>;

  struct Endpoint {
    std::string name;
    base::FilePath token_file;
  };
  using Ready = base::OnceCallback<void(Endpoint)>;

  static std::unique_ptr<ToolPipe> Start(Handler handler);
  static std::unique_ptr<ToolPipe> StartVerified(
      Handler handler,
      const base::FilePath& relay,
      const ComponentPin& pin,

      const std::optional<base::win::Sid>& app_container,
      Ready ready,
      ReportDecision report_decision = {},
      bool job_only = false);
  ~ToolPipe();
  ToolPipe(const ToolPipe&) = delete;
  ToolPipe& operator=(const ToolPipe&) = delete;

 private:
  class State;
  explicit ToolPipe(scoped_refptr<State> state);

  scoped_refptr<State> state_;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_TOOL_PIPE_H_
