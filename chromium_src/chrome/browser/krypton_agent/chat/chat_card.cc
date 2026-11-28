#include "chrome/browser/krypton_agent/chat/chat_card.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/flat_map.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/token.h"
#include "base/types/expected.h"
#include "chrome/browser/krypton_agent/agent_component.h"
#include "chrome/browser/krypton_agent/agent_tools.h"
#include "chrome/browser/krypton_agent/chat/render_kit_assets.h"
#include "chrome/browser/krypton_agent/chat/transcript.mojom.h"
#include "chrome/browser/krypton_agent/chat/transcript_assets.h"
#include "chrome/browser/krypton_agent/page_reader.h"
#include "chrome/browser/page_content_annotations/page_content_screenshot_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "components/page_content_annotations/content/page_content_screenshot_service.h"
#include "components/paint_preview/common/mojom/paint_preview_types.mojom.h"
#include "content/public/browser/krypton_agent_partition.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/webui_config.h"
#include "content/public/browser/webui_config_map.h"
#include "content/public/common/url_constants.h"
#include "mojo/public/cpp/bindings/clone_traits.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/zlib/google/compression_utils.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/webui/untrusted_web_ui_controller.h"
#include "url/gurl.h"

namespace krypton_agent {
namespace {

constexpr char kHost[] = "krypton-agent-chat";
// The data source is keyed by origin URL, including the trailing slash.
constexpr char kUrl[] = "chrome-untrusted://krypton-agent-chat/";
// Message and item IDs are the browser's own ("user-3", a 32-digit digest).
constexpr size_t kMaxIdBytes = 128;

const chat_assets::KitAsset* KitAsset(std::string_view path) {
  const auto* entry = std::ranges::find(chat_assets::kKitAssets, path,
                                        &chat_assets::KitAsset::path);
  return entry == std::end(chat_assets::kKitAssets) ? nullptr : entry;
}

std::unique_ptr<content::WebContents> CreateIsolatedContents(
    Profile& profile,
    const GURL& url,
    bool paint_while_hidden = false) {
  const auto partition = content::StoragePartitionConfig::Create(
      &profile, content::kKryptonAgentPartition,
      base::Token::CreateRandom().ToString(), /*in_memory=*/true);
  content::WebContents::CreateParams params(
      &profile, content::SiteInstance::CreateForFixedStoragePartition(
                    &profile, url, partition));
  params.initially_hidden = !paint_while_hidden;
  params.initially_hidden_but_painting = paint_while_hidden;
  return content::WebContents::Create(params);
}

mojom::TranscriptItemPtr ToMojom(const TranscriptEntry& item) {
  return mojom::TranscriptItem::New(item.id, item.kind, item.status, item.title,
                                    item.detail, item.text, item.output,
                                    item.offset);
}

class DisplayState final : public content::WebContentsUserData<DisplayState>,
                           public content::WebContentsObserver,
                           public blink::mojom::BrowserInterfaceBroker,
                           public mojom::TranscriptConnection {
 public:
  ~DisplayState() override = default;

  void Append(std::string_view id, std::string_view text) {
    if (id.empty() || id.size() > kMaxIdBytes || messages_.contains(id)) {
      return;
    }
    auto [item, inserted] =
        messages_.try_emplace(std::string(id), Message{std::string(text)});
    if (inserted) {
      order_.push_back(item->first);
      if (transcript_) {
        transcript_->Append(item->first, item->second.text);
      }
    }
  }

  void Update(std::string_view id, std::string_view text) {
    auto entry = messages_.find(id);
    if (entry == messages_.end() || entry->second.finished) {
      return;
    }
    entry->second.text = text;
    if (transcript_) {
      transcript_->Update(entry->first, entry->second.text);
    }
  }

  void Finish(std::string_view id) {
    auto entry = messages_.find(id);
    if (entry == messages_.end() || entry->second.finished) {
      return;
    }
    entry->second.finished = true;
    if (transcript_) {
      transcript_->Finish(entry->first);
    }
  }

  void SetSources(std::string_view id, std::vector<SourceCitation> sources) {
    auto entry = messages_.find(id);
    if (entry == messages_.end() || !entry->second.sources.empty()) {
      return;
    }
    for (auto& source : sources) {
      source_locations_.try_emplace(source.id, std::move(source.destination));
      entry->second.sources.push_back(mojom::SourceCitation::New(
          std::move(source.id), std::move(source.title), std::move(source.site),
          std::move(source.display_url), std::move(source.favicon_png),
          source.cited));
    }
    if (transcript_) {
      transcript_->SetSources(entry->first, mojo::Clone(entry->second.sources));
    }
  }

  void SetItem(std::string_view message_id, TranscriptEntry item) {
    auto entry = messages_.find(message_id);
    if (entry == messages_.end() || item.id.empty() ||
        item.id.size() > kMaxIdBytes) {
      return;
    }
    auto& items = entry->second.items;
    auto existing = std::ranges::find(items, item.id, &TranscriptEntry::id);
    if (existing != items.end()) {
      // An entry keeps its first position in the message.
      item.offset = existing->offset;
      *existing = std::move(item);
    } else {
      items.push_back(std::move(item));
      existing = std::prev(items.end());
    }
    if (transcript_) {
      transcript_->SetItem(entry->first, ToMojom(*existing));
    }
  }

  void SetWorking(bool working) {
    working_ = working;
    if (transcript_) {
      transcript_->SetWorking(working_);
    }
  }

  std::string ShowHtml(std::string_view message_id,
                       std::string html,
                       std::string title,
                       int width,
                       int height) {
    auto id = RegisterIsolatedRender(*web_contents(), std::move(html));
    if (id.empty()) {
      return {};
    }
    auto& render = renders_.emplace_back(
        Render{std::string(message_id), id, std::move(title), width, height});
    SendRender(render);
    return id;
  }

  void SetDark(bool dark) {
    dark_ = dark;
    if (transcript_) {
      transcript_->SetTheme(dark_);
    }
  }

  void SetPromptSuggestionHandler(
      base::RepeatingCallback<void(std::string)> handler) {
    suggest_prompt_ = std::move(handler);
  }

  void Connect(mojo::PendingRemote<mojom::Transcript> transcript) override {
    if (!transcript) {
      return;
    }
    transcript_.reset();
    transcript_.Bind(std::move(transcript));
    SetDark(dark_);
    for (const auto& id : order_) {
      const auto& message = messages_.at(id);
      transcript_->Append(id, message.text);
      for (const auto& item : message.items) {
        transcript_->SetItem(id, ToMojom(item));
      }
      if (!message.sources.empty()) {
        transcript_->SetSources(id, mojo::Clone(message.sources));
      }
      if (message.finished) {
        transcript_->Finish(id);
      }
    }
    for (const auto& render : renders_) {
      SendRender(render);
    }
    transcript_->SetWorking(working_);
  }

  void OpenSource(const std::string& source_id) override {
    if (!IsProductionAgentEnabled() ||
        web_contents()->GetLastCommittedURL().spec() != kUrl) {
      return;
    }
    const auto source = source_locations_.find(source_id);
    auto* frame = web_contents()->GetPrimaryMainFrame();
    auto* profile =
        Profile::FromBrowserContext(web_contents()->GetBrowserContext());
    if (source == source_locations_.end() ||
        !source->second.SchemeIsHTTPOrHTTPS() ||
        source->second.has_username() || source->second.has_password() ||
        !frame || !profile->IsRegularProfile() ||
        !frame->ConsumeTransientUserActivation()) {
      return;
    }
    NavigateParams params(profile, source->second, ui::PAGE_TRANSITION_LINK);
    params.disposition = WindowOpenDisposition::kNewForegroundTab;
    Navigate(&params);
  }

  void OpenRender(const std::string& render_id) override {
    if (!AllowRenderRequest(render_id)) {
      return;
    }
    auto& profile =
        *Profile::FromBrowserContext(web_contents()->GetBrowserContext());
    const GURL url(base::StrCat({kAgentRenderOrigin, "view/", render_id}));
    auto contents = CreateIsolatedContents(profile, url);
    if (!AttachIsolatedRender(*web_contents(), *contents, render_id)) {
      return;
    }
    contents->GetController().LoadURLWithParams(
        content::NavigationController::LoadURLParams(url));
    NavigateParams params(&profile, url, ui::PAGE_TRANSITION_LINK);
    params.contents_to_insert = std::move(contents);
    params.disposition = WindowOpenDisposition::kNewForegroundTab;
    Navigate(&params);
  }

  void SuggestPrompt(const std::string& render_id,
                     const std::string& text) override {
    if (!suggest_prompt_ || text.empty() || text.size() > 480000 ||
        render_id.size() != 32) {
      return;
    }
    auto* primary = web_contents()->GetPrimaryMainFrame();
    if (!primary) {
      return;
    }
    const GURL url(base::StrCat({kAgentRenderOrigin, render_id}));
    bool active = false;
    primary->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
      if (frame->GetParent() == primary &&
          frame->GetLastCommittedURL() == url &&
          frame->HasTransientUserActivation()) {
        active = true;
      }
    });
    if (active && AllowRenderRequest(render_id)) {
      suggest_prompt_.Run(text);
    }
  }

 private:
  friend class content::WebContentsUserData<DisplayState>;
  explicit DisplayState(content::WebContents* contents)
      : content::WebContentsUserData<DisplayState>(*contents),
        content::WebContentsObserver(contents) {}

  bool AllowRenderRequest(std::string_view id) {
    auto* frame = web_contents()->GetPrimaryMainFrame();
    return IsProductionAgentEnabled() && frame &&
           web_contents()->GetLastCommittedURL().spec() == kUrl &&
           Profile::FromBrowserContext(web_contents()->GetBrowserContext())
               ->IsRegularProfile() &&
           std::ranges::any_of(renders_,
                               [id](const Render& render) {
                                 return !render.message_id.empty() &&
                                        render.id == id;
                               }) &&
           frame->ConsumeTransientUserActivation();
  }

  void ReadyToCommitNavigation(content::NavigationHandle* navigation) override {
    if (!navigation->IsInPrimaryMainFrame()) {
      return;
    }
    receiver_.reset();
    broker_.reset();
    transcript_.reset();
    if (navigation->GetURL() == GURL(kUrl) && !navigation->IsErrorPage()) {
      navigation->GetRenderFrameHost()->EnableMojoJsBindingsWithBroker(
          broker_.BindNewPipeAndPassRemote());
    }
  }

  void GetInterface(mojo::GenericPendingReceiver receiver) override {
    auto connection = receiver.As<mojom::TranscriptConnection>();
    if (connection && !receiver_.is_bound()) {
      receiver_.Bind(std::move(connection));
    }
  }

  struct Message {
    std::string text;
    bool finished = false;
    std::vector<mojom::SourceCitationPtr> sources;
    std::vector<TranscriptEntry> items;
  };
  struct Render {
    std::string message_id;
    std::string id;
    std::string title;
    int width;
    int height;
  };

  void SendRender(const Render& render) {
    if (transcript_) {
      transcript_->ShowHtml(render.message_id, render.id, render.title,
                            render.width, render.height);
    }
  }

  mojo::Receiver<blink::mojom::BrowserInterfaceBroker> broker_{this};
  mojo::Receiver<mojom::TranscriptConnection> receiver_{this};
  mojo::Remote<mojom::Transcript> transcript_;
  base::flat_map<std::string, Message> messages_;
  base::flat_map<std::string, GURL> source_locations_;
  std::vector<std::string> order_;
  std::vector<Render> renders_;
  base::RepeatingCallback<void(std::string)> suggest_prompt_;
  bool dark_ = false;
  bool working_ = false;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(DisplayState);

class RenderSource final : public content::URLDataSource {
 public:
  std::string GetSource() override { return kAgentRenderOrigin; }
  std::string GetMimeType(const GURL& url) override {
    const auto path = URLToRequestPath(url);
    if (!path.starts_with("kit/")) {
      return "text/html";
    }
    if (path.ends_with(".css")) {
      return "text/css";
    }
    if (path.ends_with(".js") || path.ends_with(".mjs")) {
      return "application/javascript";
    }
    return "text/plain";
  }
  bool ShouldServeMimeTypeAsContentTypeHeader() override { return true; }
  std::string GetAccessControlAllowOriginForOrigin(
      const std::string& origin) override {
    // Module imports from the sandbox have an opaque origin. Only kit assets
    // can be requested as subresources under the render's CSP.
    return origin == "null" ? "*" : "";
  }
  bool AllowCaching() override { return false; }
  bool ShouldReplaceExistingSource() override { return false; }
  bool ShouldDenyXFrameOptions() override { return false; }

  void StartDataRequest(const GURL& url,
                        const content::WebContents::Getter& getter,
                        GotDataCallback callback) override {
    auto* contents = getter.Run();
    auto* frame = contents ? contents->GetPrimaryMainFrame() : nullptr;
    if (!frame || !IsProductionAgentEnabled() ||
        !content::IsKryptonAgentPartition(frame->GetStoragePartition())) {
      std::move(callback).Run(nullptr);
      return;
    }
    const auto path = URLToRequestPath(url);
    if (!url.has_query() && !url.has_ref()) {
      if (const auto* asset = KitAsset(path)) {
        auto& body = kit_cache_[path];
        if (!body) {
          std::string decoded;
          if (compression::GzipUncompress(asset->gzip, &decoded)) {
            body = base::MakeRefCounted<base::RefCountedString>(
                std::move(decoded));
          }
        }
        std::move(callback).Run(body);
        return;
      }
    }
    const auto* html = contents ? FindIsolatedRender(*contents, url) : nullptr;
    scoped_refptr<base::RefCountedString> body;
    if (html) {
      body = base::MakeRefCounted<base::RefCountedString>(*html);
    }
    std::move(callback).Run(std::move(body));
  }

  std::string GetContentSecurityPolicy(
      network::mojom::CSPDirectiveName directive) override {
    using Directive = network::mojom::CSPDirectiveName;
    switch (directive) {
      case Directive::DefaultSrc:
        return "default-src 'none'; sandbox allow-scripts; "
               "frame-ancestors chrome-untrusted://krypton-agent-chat;";
      case Directive::ScriptSrc:
        return "script-src 'unsafe-inline' "
               "chrome-untrusted://krypton-agent-render/kit/;";
      case Directive::StyleSrc:
        return "style-src 'unsafe-inline' "
               "chrome-untrusted://krypton-agent-render/kit/;";
      case Directive::ImgSrc:
        return "img-src data: blob: "
               "chrome-untrusted://krypton-agent-render/kit/;";
      case Directive::FontSrc:
        return "font-src data: blob: "
               "chrome-untrusted://krypton-agent-render/kit/;";
      case Directive::FormAction:
        return "form-action 'none';";
      case Directive::BaseURI:
        return "base-uri 'none';";
      default:
        return {};
    }
  }

 private:
  base::flat_map<std::string, scoped_refptr<base::RefCountedString>> kit_cache_;
};

class ChatUI final : public ui::UntrustedWebUIController {
 public:
  explicit ChatUI(content::WebUI* web_ui)
      : ui::UntrustedWebUIController(web_ui) {
    auto* source = content::WebUIDataSource::CreateAndAdd(
        web_ui->GetWebContents()->GetBrowserContext(), kUrl);
    source->SetRequestFilter(
        base::BindRepeating([](const std::string&) { return true; }),
        base::BindRepeating(
            [](const std::string& path,
               content::WebUIDataSource::GotDataCallback reply) {
              std::string_view body;
              if (path.empty()) {
                body = chat_assets::kTranscriptHtml;
              } else if (path == "transcript.css") {
                body = chat_assets::kTranscriptCss;
              } else if (path == "transcript.js") {
                body = chat_assets::kTranscriptJs;
              } else if (path == "transcript.mojom-webui.js") {
                body = chat_assets::kTranscriptMojo;
              }
              std::move(reply).Run(
                  body.empty() ? nullptr
                               : base::MakeRefCounted<base::RefCountedString>(
                                     std::string(body)));
            }));
    using Directive = network::mojom::CSPDirectiveName;
    source->OverrideContentSecurityPolicy(Directive::DefaultSrc,
                                          "default-src 'none';");
    source->OverrideContentSecurityPolicy(
        Directive::ScriptSrc,
        "script-src 'self' "
        "chrome-untrusted://resources/mojo/mojo/public/js/bindings.js;");
    source->OverrideContentSecurityPolicy(Directive::StyleSrc,
                                          "style-src 'self';");
    source->OverrideContentSecurityPolicy(Directive::ConnectSrc,
                                          "connect-src 'none';");
    source->OverrideContentSecurityPolicy(Directive::ImgSrc, "img-src blob:;");
    source->OverrideContentSecurityPolicy(Directive::BaseURI,
                                          "base-uri 'none';");
    source->OverrideContentSecurityPolicy(Directive::FormAction,
                                          "form-action 'none';");
    source->OverrideContentSecurityPolicy(
        Directive::FrameSrc,
        "frame-src chrome-untrusted://krypton-agent-render;");
    source->OverrideContentSecurityPolicy(Directive::WorkerSrc,
                                          "worker-src 'none';");
  }

  bool IsJavascriptErrorReportingEnabled() override { return false; }

 private:
  WEB_UI_CONTROLLER_TYPE_DECL();
};

WEB_UI_CONTROLLER_TYPE_IMPL(ChatUI)

class ChatConfig final : public content::DefaultWebUIConfig<ChatUI> {
 public:
  ChatConfig() : DefaultWebUIConfig(content::kChromeUIUntrustedScheme, kHost) {}

  bool IsWebUIEnabled(content::BrowserContext* context) override {
    return Profile::FromBrowserContext(context)->IsRegularProfile() &&
           IsProductionAgentEnabled();
  }
};

class RenderConfig final
    : public content::DefaultWebUIConfig<ui::UntrustedWebUIController> {
 public:
  RenderConfig()
      : DefaultWebUIConfig(content::kChromeUIUntrustedScheme,
                           "krypton-agent-render") {}

  bool IsWebUIEnabled(content::BrowserContext* context) override {
    return Profile::FromBrowserContext(context)->IsRegularProfile() &&
           IsProductionAgentEnabled();
  }
};

void RegisterChat() {
  static const bool registered = [] {
    content::WebUIConfigMap::GetInstance().AddUntrustedWebUIConfig(
        std::make_unique<ChatConfig>());
    content::WebUIConfigMap::GetInstance().AddUntrustedWebUIConfig(
        std::make_unique<RenderConfig>());
    return true;
  }();
  (void)registered;
}

}  // namespace

std::optional<std::string> GetHtmlGuide(std::string_view topic) {
  const auto* asset = KitAsset(base::StrCat({"kit/1/guides/", topic, ".md"}));
  std::string guide;
  if (!asset || !compression::GzipUncompress(asset->gzip, &guide)) {
    return std::nullopt;
  }
  return guide;
}

class HtmlPreview final : public content::WebContentsObserver {
 public:
  HtmlPreview(Profile& profile,
              std::string html,
              int width,
              bool dark,
              base::ListValue steps,
              base::OnceCallback<void(base::DictValue)> reply)
      : width_(width), steps_(std::move(steps)), reply_(std::move(reply)) {
    if (!profile.IsRegularProfile() || !IsProductionAgentEnabled()) {
      Fail("unavailable");
      return;
    }
    RegisterChat();
    content::URLDataSource::Add(&profile, std::make_unique<RenderSource>());
    contents_ = CreateIsolatedContents(profile, GURL(kAgentRenderOrigin),
                                       /*paint_while_hidden=*/true);
    Observe(contents_.get());
    contents_->Resize(gfx::Rect(width_, kCaptureHeight));
    html.insert(
        0, dark ? "<!doctype html><meta name='color-scheme' content='dark'>"
                : "<!doctype html><meta name='color-scheme' content='light'>");
    const auto id = RegisterIsolatedRender(*contents_, std::move(html),
                                           /*full_view=*/true);
    render_url_ = GURL(base::StrCat({kAgentRenderOrigin, "view/", id}));
    capture_ = web_contents()->IncrementCapturerCount(
        gfx::Size(width_, kCaptureHeight), /*stay_hidden=*/false,
        /*stay_awake=*/false, /*is_activity=*/false);
    timeout_.Start(FROM_HERE, base::Seconds(8),
                   base::BindOnce(&HtmlPreview::Fail,
                                  weak_factory_.GetWeakPtr(), "timeout"));
    web_contents()->GetController().LoadURLWithParams(
        content::NavigationController::LoadURLParams(render_url_));
  }

 private:
  static constexpr int kCaptureHeight = 1200;

  void ReadyToCommitNavigation(content::NavigationHandle* request) override {
    if (request->IsInPrimaryMainFrame()) {
      if (auto* view = request->GetRenderFrameHost()->GetView()) {
        view->SetSize(gfx::Size(width_, kCaptureHeight));
      }
    }
  }

  void DidFinishLoad(content::RenderFrameHost* frame,
                     const GURL& url) override {
    if (!reply_ || measuring_ ||
        frame != web_contents()->GetPrimaryMainFrame() || url != render_url_) {
      return;
    }
    measuring_ = true;
    document_ = frame->GetWeakDocumentPtr();
    NextStep();
  }

  void NextStep() {
    auto* frame = document_.AsRenderFrameHostIfValid();
    if (!reply_ || !frame) {
      Fail("document_gone");
      return;
    }
    if (step_ < steps_.size()) {
      const auto& step = steps_[step_++].GetDict();
      if (const auto wait = step.FindInt("milliseconds")) {
        step_timer_.Start(
            FROM_HERE, base::Milliseconds(*wait),
            base::BindOnce(&HtmlPreview::NextStep, weak_factory_.GetWeakPtr()));
        return;
      }
      const auto data = base::WriteJson(step);
      if (!data) {
        Fail("invalid_step");
        return;
      }
      constexpr std::string_view kRunStep = R"JS(
        (step => {
          const target = document.querySelector(step.selector);
          if (!(target instanceof HTMLElement)) return false;
          if (step.action === 'click') {
            target.click();
          } else if (step.action === 'type') {
            if (!(target instanceof HTMLInputElement) &&
                !(target instanceof HTMLTextAreaElement)) return false;
            target.value = step.text;
            target.dispatchEvent(new Event('input', {bubbles: true}));
            target.dispatchEvent(new Event('change', {bubbles: true}));
          } else {
            for (const type of ['keydown', 'keyup']) {
              target.dispatchEvent(new KeyboardEvent(type, {
                key: step.key, bubbles: true, cancelable: true,
              }));
            }
          }
          return true;
        })
      )JS";
      frame->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(base::StrCat({kRunStep, "(", *data, ")"})),
          base::BindOnce(&HtmlPreview::OnStep, weak_factory_.GetWeakPtr()),
          ISOLATED_WORLD_ID_CHROME_INTERNAL);
      return;
    }
    frame->ExecuteJavaScriptInIsolatedWorld(
        u"Math.ceil(Math.max(document.body?.getBoundingClientRect().bottom ?? "
        u"0,"
        u" document.documentElement.scrollHeight > innerHeight"
        u" ? document.documentElement.scrollHeight : 0))",
        base::BindOnce(&HtmlPreview::Capture, weak_factory_.GetWeakPtr()),
        ISOLATED_WORLD_ID_CHROME_INTERNAL);
  }

  void OnStep(base::Value result) {
    if (result.GetIfBool().value_or(false)) {
      NextStep();
    } else {
      Fail("invalid_step");
    }
  }

  void OnDidAddMessageToConsole(
      content::RenderFrameHost* frame,
      blink::mojom::ConsoleMessageLevel level,
      const std::u16string& text,
      int32_t line,
      const std::u16string& source,
      const std::optional<std::u16string>& stack_trace) override {
    if (!reply_ || console_.size() >= 16 ||
        frame != web_contents()->GetPrimaryMainFrame() ||
        frame->GetLastCommittedURL() != render_url_) {
      return;
    }
    using Level = blink::mojom::ConsoleMessageLevel;
    std::string_view label = "log";
    if (level == Level::kError) {
      label = "error";
    } else if (level == Level::kWarning) {
      label = "warning";
    } else if (level == Level::kInfo) {
      label = "info";
    }
    console_.Append(base::DictValue()
                        .Set("level", label)
                        .Set("text", base::UTF16ToUTF8(text.substr(0, 512))));
  }

  void Capture(base::Value result) {
    if (!reply_) {
      return;
    }
    if (!document_.AsRenderFrameHostIfValid() ||
        !web_contents()->GetRenderWidgetHostView()) {
      Fail("no_view");
      return;
    }
    auto* service = page_content_annotations::
        PageContentScreenshotServiceFactory::GetForProfile(
            Profile::FromBrowserContext(web_contents()->GetBrowserContext()));
    if (!service) {
      Fail("paint_preview_error");
      return;
    }
    const int height = std::max(kMinHtmlHeight, result.GetIfInt().value_or(0));
    const int captured = std::min(height, kCaptureHeight);
    constexpr auto kScrollOffset =
        paint_preview::mojom::ClipCoordOverride::kScrollOffset;
    service->RequestScreenshot(
        web_contents(),
        {.clip_rect = gfx::Rect(width_, captured),
         .scale_factor = 1.0,
         .clip_x_coord_override = kScrollOffset,
         .clip_y_coord_override = kScrollOffset,
         .redaction_params = {},
         .max_per_capture_bytes = kMaxPaintPreviewRecordingBytes},
        base::BindOnce(&HtmlPreview::Encode, weak_factory_.GetWeakPtr(), height,
                       captured));
  }

  void Encode(int height,
              int captured,
              base::expected<const SkBitmap*, std::string> result) {
    if (!reply_) {
      return;
    }
    if (!result.has_value()) {
      Fail("paint_preview_error");
      return;
    }
    if ((*result)->drawsNothing()) {
      Fail("empty_bitmap");
      return;
    }
    auto response = base::DictValue()
                        .Set("width", width_)
                        .Set("contentHeight", height)
                        .Set("capturedHeight", captured);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](SkBitmap bitmap, base::DictValue response) {
              auto png = gfx::PNGCodec::FastEncodeBGRASkBitmap(bitmap, false);
              if (!png || png->size() > 2 * 1024 * 1024) {
                return base::DictValue().Set("failure", "encode_failed");
              }
              response.Set("screenshot",
                           base::DictValue()
                               .Set("mimeType", "image/png")
                               .Set("data", base::Base64Encode(*png))
                               .Set("width", bitmap.width())
                               .Set("height", bitmap.height()));
              return response;
            },
            **result, std::move(response)),
        base::BindOnce(&HtmlPreview::Complete, weak_factory_.GetWeakPtr()));
  }

  void Fail(std::string_view reason) {
    Complete(base::DictValue().Set("failure", reason));
  }

  void Complete(base::DictValue result) {
    if (!reply_) {
      return;
    }
    if (const auto* failure = result.FindString("failure")) {
      LOG(WARNING) << "Krypton Agent HTML preview failed: " << *failure;
    }
    timeout_.Stop();
    step_timer_.Stop();
    capture_.RunAndReset();
    result.Set("consoleMessages", std::move(console_));
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(reply_), std::move(result)));
  }

  std::unique_ptr<content::WebContents> contents_;
  GURL render_url_;
  base::ScopedClosureRunner capture_;
  const int width_;
  const base::ListValue steps_;
  size_t step_ = 0;
  content::WeakDocumentPtr document_;
  base::OnceCallback<void(base::DictValue)> reply_;
  base::ListValue console_;
  base::OneShotTimer timeout_;
  base::OneShotTimer step_timer_;
  bool measuring_ = false;
  base::WeakPtrFactory<HtmlPreview> weak_factory_{this};
};

std::unique_ptr<ChatCard> ChatCard::Create(Profile& profile,
                                           IsolatedLinkHandler on_link) {
  if (!profile.IsRegularProfile() || !IsProductionAgentEnabled()) {
    return nullptr;
  }
  RegisterChat();
  content::URLDataSource::Add(&profile, std::make_unique<RenderSource>());
  const GURL url(kUrl);
  auto contents = CreateIsolatedContents(profile, url);
  contents->SetPageBaseBackgroundColor(SK_ColorTRANSPARENT);
  SetIsolatedLinkHandler(*contents, std::move(on_link));
  DisplayState::CreateForWebContents(contents.get());
  contents->GetController().LoadURLWithParams(
      content::NavigationController::LoadURLParams(url));
  return std::unique_ptr<ChatCard>(new ChatCard(std::move(contents)));
}

ChatCard::ChatCard(std::unique_ptr<content::WebContents> contents)
    : contents_(std::move(contents)) {}

ChatCard::~ChatCard() {
  Detach();
}

content::WebContents& ChatCard::contents() {
  return *contents_;
}

bool ChatCard::SetPlacement(views::View& parent, const gfx::Rect& bounds) {
  if (bounds.IsEmpty() || !parent.GetLocalBounds().Contains(bounds)) {
    Detach();
    return bounds.IsEmpty();
  }
  auto* view = attached_view_.view();
  if (!view || view->parent() != &parent) {
    Detach();
    auto web_view =
        std::make_unique<views::WebView>(contents_->GetBrowserContext());
    web_view->SetProperty(views::kViewIgnoredByLayoutKey, true);
    web_view->SetBoundsRect(bounds);
    view = parent.AddChildView(std::move(web_view));
    attached_view_.SetView(view);
    static_cast<views::WebView*>(view)->SetWebContents(contents_.get());
  } else {
    view->SetBoundsRect(bounds);
  }
  return true;
}

bool ChatCard::Focus() {
  auto* view = attached_view_.view();
  if (!view || !view->IsDrawn()) {
    return false;
  }
  view->RequestFocus();
  return true;
}

void ChatCard::Detach() {
  auto* view = attached_view_.view();
  attached_view_.SetView(nullptr);
  if (view) {
    static_cast<views::WebView*>(view)->SetWebContents(nullptr);
    if (auto* parent = view->parent()) {
      parent->RemoveChildViewT(view);
    }
  }
}

void ChatCard::Append(std::string_view id, std::string_view text) {
  DisplayState::FromWebContents(contents_.get())->Append(id, text);
}

void ChatCard::Update(std::string_view id, std::string_view text) {
  DisplayState::FromWebContents(contents_.get())->Update(id, text);
}

void ChatCard::Finish(std::string_view id) {
  DisplayState::FromWebContents(contents_.get())->Finish(id);
}

void ChatCard::SetSources(std::string_view id,
                          std::vector<SourceCitation> sources) {
  DisplayState::FromWebContents(contents_.get())
      ->SetSources(id, std::move(sources));
}

void ChatCard::SetItem(std::string_view message_id, TranscriptEntry item) {
  DisplayState::FromWebContents(contents_.get())
      ->SetItem(message_id, std::move(item));
}

void ChatCard::SetWorking(bool working) {
  DisplayState::FromWebContents(contents_.get())->SetWorking(working);
}

void ChatCard::SetDark(bool dark) {
  dark_ = dark;
  DisplayState::FromWebContents(contents_.get())->SetDark(dark);
}

void ChatCard::SetPromptSuggestionHandler(
    base::RepeatingCallback<void(std::string)> handler) {
  DisplayState::FromWebContents(contents_.get())
      ->SetPromptSuggestionHandler(std::move(handler));
}

std::string ChatCard::ShowHtml(std::string_view message_id,
                               std::string html,
                               std::string title,
                               int width,
                               int height) {
  html.insert(
      0, dark_ ? "<!doctype html><meta name='color-scheme' content='dark'>"
               : "<!doctype html><meta name='color-scheme' content='light'>");
  return DisplayState::FromWebContents(contents_.get())
      ->ShowHtml(message_id, std::move(html), std::move(title), width, height);
}

void ChatCard::PreviewHtml(std::string html,
                           int width,
                           bool dark,
                           base::ListValue steps,
                           base::OnceCallback<void(base::DictValue)> reply) {
  preview_ = std::make_unique<HtmlPreview>(
      *Profile::FromBrowserContext(contents_->GetBrowserContext()),
      std::move(html), width, dark, std::move(steps), std::move(reply));
}

void ChatCard::CancelPreview() {
  preview_.reset();
}

}  // namespace krypton_agent
