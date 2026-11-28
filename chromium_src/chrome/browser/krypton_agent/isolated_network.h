#ifndef CHROME_BROWSER_KRYPTON_AGENT_ISOLATED_NETWORK_H_
#define CHROME_BROWSER_KRYPTON_AGENT_ISOLATED_NETWORK_H_

#include <string>
#include <string_view>

#include "base/functional/callback.h"

class GURL;

namespace content {
class NavigationThrottleRegistry;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace network {
class URLLoaderFactoryBuilder;
}

namespace krypton_agent {

inline constexpr char kAgentRenderOrigin[] =
    "chrome-untrusted://krypton-agent-render/";

std::string RegisterIsolatedRender(content::WebContents& contents,
                                   std::string html,
                                   bool full_view = false);
const std::string* FindIsolatedRender(content::WebContents& contents,
                                      const GURL& url);
bool AttachIsolatedRender(content::WebContents& source,
                          content::WebContents& view,
                          std::string_view id);

// Takes ownership of network requests from the isolated agent partition.
bool DenyIsolatedNetwork(content::RenderFrameHost* frame,
                         int render_process_id,
                         network::URLLoaderFactoryBuilder& factory,
                         bool is_for_network_service);
bool RestrictIsolatedNavigation(content::NavigationThrottleRegistry& registry);
using IsolatedLinkHandler = base::RepeatingCallback<void(const GURL&)>;
void SetIsolatedLinkHandler(content::WebContents& contents,
                            IsolatedLinkHandler handler);

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_ISOLATED_NETWORK_H_
