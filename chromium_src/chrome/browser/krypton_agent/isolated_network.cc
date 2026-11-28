#include "chrome/browser/krypton_agent/isolated_network.h"

#include <memory>
#include <utility>

#include "base/containers/flat_map.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/memory/self_deleting.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "base/token.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/krypton_agent_partition.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "url/origin.h"

namespace krypton_agent {
namespace {

constexpr char kChatUrl[] = "chrome-untrusted://krypton-agent-chat/";
constexpr int kLinkHandlerKey = 0;
constexpr int kRenderDocumentsKey = 0;
constexpr int kRenderRegistryKey = 0;

class RenderRegistry final : public base::SupportsUserData::Data {
 public:
  base::flat_map<std::string, std::weak_ptr<const std::string>> documents;
};

class RenderDocuments final : public base::SupportsUserData::Data {
 public:
  explicit RenderDocuments(RenderRegistry& registry) : registry_(registry) {}

  ~RenderDocuments() override {
    for (auto& [id, document] : html) {
      document.reset();
      auto entry = registry_->documents.find(id);
      if (entry != registry_->documents.end() && entry->second.expired()) {
        registry_->documents.erase(entry);
      }
    }
  }

  base::flat_map<std::string, std::shared_ptr<const std::string>> html;
  bool full_view = false;

 private:
  // A BrowserContext outlives its WebContents.
  const raw_ref<RenderRegistry> registry_;
};

RenderDocuments& DocumentsFor(content::WebContents& contents) {
  auto* documents =
      static_cast<RenderDocuments*>(contents.GetUserData(&kRenderDocumentsKey));
  if (!documents) {
    auto* context = contents.GetBrowserContext();
    auto* registry =
        static_cast<RenderRegistry*>(context->GetUserData(&kRenderRegistryKey));
    if (!registry) {
      auto new_registry = std::make_unique<RenderRegistry>();
      registry = new_registry.get();
      context->SetUserData(&kRenderRegistryKey, std::move(new_registry));
    }
    auto new_documents = std::make_unique<RenderDocuments>(*registry);
    documents = new_documents.get();
    contents.SetUserData(&kRenderDocumentsKey, std::move(new_documents));
  }
  return *documents;
}

class LinkHandlerData final : public base::SupportsUserData::Data {
 public:
  explicit LinkHandlerData(IsolatedLinkHandler handler)
      : handler(std::move(handler)) {}
  const IsolatedLinkHandler handler;
};

class IsolatedNavigationThrottle final : public content::NavigationThrottle {
 public:
  explicit IsolatedNavigationThrottle(
      content::NavigationThrottleRegistry& registry)
      : NavigationThrottle(registry) {}

  ThrottleCheckResult WillStartRequest() override { return CheckDestination(); }

  ThrottleCheckResult WillRedirectRequest() override {
    return CANCEL_AND_IGNORE;
  }

  ThrottleCheckResult WillCommitWithoutUrlLoader() override {
    return CheckDestination();
  }

  const char* GetNameForLogging() override {
    return "KryptonAgentIsolatedNavigation";
  }

 private:
  ThrottleCheckResult CheckDestination() {
    auto& navigation = *navigation_handle();
    if (!navigation.IsRendererInitiated() &&
        navigation.IsInPrimaryMainFrame() &&
        navigation.GetURL().spec() == kChatUrl) {
      return PROCEED;
    }
    auto* contents = navigation.GetWebContents();
    const auto& url = navigation.GetURL();
    if (!navigation.IsRendererInitiated() &&
        navigation.IsInPrimaryMainFrame() && url.path().starts_with("/view/") &&
        FindIsolatedRender(*contents, url)) {
      return PROCEED;
    }
    auto* parent = navigation.GetParentFrame();
    if (navigation.IsRendererInitiated() && parent &&
        parent == contents->GetPrimaryMainFrame() &&
        parent->GetLastCommittedURL().spec() == kChatUrl &&
        navigation.GetInitiatorOrigin() == parent->GetLastCommittedOrigin() &&
        FindIsolatedRender(*contents, url)) {
      return PROCEED;
    }
    if (navigation.IsRendererInitiated() && navigation.IsInPrimaryMainFrame() &&
        navigation.HasUserGesture() &&
        contents->GetLastCommittedURL().spec() == kChatUrl &&
        url.SchemeIsHTTPOrHTTPS() && !url.has_username() &&
        !url.has_password() && url.spec().size() <= 8192) {
      const auto* data = static_cast<LinkHandlerData*>(
          contents->GetUserData(&kLinkHandlerKey));
      if (data && data->handler) {
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(data->handler, url));
      }
    }
    return CANCEL_AND_IGNORE;
  }
};

class DeniedFactory final : public network::SelfDeletingURLLoaderFactory {
 public:
  DeniedFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      base::SelfDeletingPassKey key)
      : SelfDeletingURLLoaderFactory(std::move(receiver), key) {}

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& annotation) override {
    mojo::Remote<network::mojom::URLLoaderClient> request_client(
        std::move(client));
    if (request_client) {
      request_client->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
    }
  }

 private:
  ~DeniedFactory() override = default;
};

}  // namespace

std::string RegisterIsolatedRender(content::WebContents& contents,
                                   std::string html,
                                   bool full_view) {
  auto& documents = DocumentsFor(contents);
  documents.full_view = full_view;
  auto* registry = static_cast<RenderRegistry*>(
      contents.GetBrowserContext()->GetUserData(&kRenderRegistryKey));
  auto id = base::Token::CreateRandom().ToString();
  auto document = std::make_shared<const std::string>(std::move(html));
  registry->documents.emplace(id, document);
  documents.html.emplace(id, std::move(document));
  return id;
}

bool AttachIsolatedRender(content::WebContents& source,
                          content::WebContents& view,
                          std::string_view id) {
  if (source.GetBrowserContext() != view.GetBrowserContext() ||
      !FindIsolatedRender(
          source, GURL(std::string(kAgentRenderOrigin) + std::string(id)))) {
    return false;
  }
  auto* registry = static_cast<RenderRegistry*>(
      source.GetBrowserContext()->GetUserData(&kRenderRegistryKey));
  auto entry = registry->documents.find(id);
  if (entry == registry->documents.end()) {
    return false;
  }
  auto document = entry->second.lock();
  if (!document) {
    return false;
  }
  auto& documents = DocumentsFor(view);
  documents.full_view = true;
  documents.html.emplace(std::string(id), std::move(document));
  return true;
}

const std::string* FindIsolatedRender(content::WebContents& contents,
                                      const GURL& url) {
  if (!url.spec().starts_with(kAgentRenderOrigin) || !url.is_valid() ||
      url.has_query() || url.has_ref()) {
    return nullptr;
  }
  auto* documents =
      static_cast<RenderDocuments*>(contents.GetUserData(&kRenderDocumentsKey));
  if (!documents) {
    return nullptr;
  }
  const std::string_view path = url.path();
  const std::string_view prefix = documents->full_view ? "/view/" : "/";
  if (!path.starts_with(prefix) || path.size() != prefix.size() + 32) {
    return nullptr;
  }
  const auto it = documents->html.find(path.substr(prefix.size()));
  return it == documents->html.end() ? nullptr : it->second.get();
}

bool DenyIsolatedNetwork(content::RenderFrameHost* frame,
                         int render_process_id,
                         network::URLLoaderFactoryBuilder& factory,
                         bool is_for_network_service) {
  auto* process = frame ? frame->GetProcess()
                        : content::RenderProcessHost::FromID(render_process_id);
  if (!process ||
      !content::IsKryptonAgentPartition(process->GetStoragePartition())) {
    return false;
  }
  if (is_for_network_service) {
    auto [receiver, downstream] = factory.Append();
    base::MakeSelfDeleting<DeniedFactory>(std::move(receiver));
  }
  return true;
}

bool RestrictIsolatedNavigation(content::NavigationThrottleRegistry& registry) {
  auto* frame =
      registry.GetNavigationHandle().GetWebContents()->GetPrimaryMainFrame();
  if (!frame ||
      !content::IsKryptonAgentPartition(frame->GetStoragePartition())) {
    return false;
  }
  registry.AddThrottle(std::make_unique<IsolatedNavigationThrottle>(registry));
  return true;
}

void SetIsolatedLinkHandler(content::WebContents& contents,
                            IsolatedLinkHandler handler) {
  contents.SetUserData(&kLinkHandlerKey,
                       std::make_unique<LinkHandlerData>(std::move(handler)));
}

}  // namespace krypton_agent
