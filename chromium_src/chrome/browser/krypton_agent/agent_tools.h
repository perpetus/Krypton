#ifndef CHROME_BROWSER_KRYPTON_AGENT_AGENT_TOOLS_H_
#define CHROME_BROWSER_KRYPTON_AGENT_AGENT_TOOLS_H_

#include <memory>
#include <optional>
#include <string_view>

#include "base/values.h"
#include "chrome/browser/actor/actor_surface_handle.h"

namespace actor {
class ToolRegistry;
class ToolRequest;
}  // namespace actor

namespace krypton_agent {
struct PageOutline;

enum class KryptonToolId {
  kReadPage,
  kWebSearch,
  kReadUrl,
  kReadContextTab,
  kRequestHandoff,
  kMoveMouse,
  kDragAndRelease,
  kSetChecked,
  kReadElement,
  kCapturePage,
  kListWorkTabs,
  kCloseWorkTab,
  kPlayMedia,
  kPauseMedia,
  kSeekMedia,
  kHelp,
  kHtmlRender,
  kHtmlPreview,
  kHtmlGuide
};

inline constexpr int kMaxHtmlBytes = 512'000;
inline constexpr int kMinHtmlWidth = 240;
inline constexpr int kMaxHtmlWidth = 1600;
inline constexpr int kMinHtmlHeight = 80;
inline constexpr int kMaxHtmlHeight = 2000;
inline constexpr int kMaxHtmlTitleBytes = 200;
inline constexpr int kDefaultHtmlWidth = 728;

std::optional<KryptonToolId> FindKryptonTool(std::string_view name);
std::string_view GetKryptonHelp(std::string_view topic);
base::ListValue ListTools(const actor::ToolRegistry& registry);
bool ValidHtmlArguments(const base::DictValue& arguments, bool preview);
bool ValidArguments(const actor::ToolRegistry& registry,
                    std::string_view tool,
                    const base::DictValue& arguments);
std::unique_ptr<actor::ToolRequest> BuildRequest(
    std::string_view tool,
    const base::DictValue& arguments,
    actor::ActorSurfaceHandle surface,
    const PageOutline& page);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_AGENT_TOOLS_H_
