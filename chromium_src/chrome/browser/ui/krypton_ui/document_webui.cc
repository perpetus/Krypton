// Copyright 2026 The Krypton Authors
// Use of this source code is governed by the GPL-3.0 license in LICENSE.
#include "chrome/browser/ui/krypton_ui/document_webui.h"
#include "chrome/browser/ui/krypton_ui/local_models_webui.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/krypton_ui/document_store.h"
#include "chrome/browser/ui/select_file_policy/chrome_select_file_policy.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/browser/webui_config.h"
#include "content/public/browser/webui_config_map.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "third_party/krypton_documents/grit/krypton_documents_resources.h"
#include "third_party/krypton_documents/grit/krypton_documents_resources_map.h"
#include "ui/base/webui/resource_path.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "ui/webui/untrusted_web_ui_controller.h"

namespace krypton_ui {
namespace {
// chrome-untrusted:// data sources are looked up by origin URL, which always
// has a trailing slash. Without it every editor request fails with
// ERR_INVALID_URL and the frame shows a "temporarily down" error page.
constexpr char kEditorSource[] = "chrome-untrusted://krypton-documents/";
void Policy(content::WebUIDataSource* source, bool trusted) {
  using Directive = network::mojom::CSPDirectiveName;
  source->OverrideContentSecurityPolicy(Directive::DefaultSrc,
                                        "default-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::ScriptSrc,
                                        "script-src 'self';");
  source->OverrideContentSecurityPolicy(Directive::FontSrc, "font-src 'self';");
  source->OverrideContentSecurityPolicy(Directive::ConnectSrc,
                                        "connect-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::ImgSrc,
                                        "img-src 'self' data: blob:;");
  source->OverrideContentSecurityPolicy(
      Directive::StyleSrc,
      trusted ? "style-src 'self';" : "style-src 'self' 'unsafe-inline';");
  source->OverrideContentSecurityPolicy(Directive::ObjectSrc,
                                        "object-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::BaseURI, "base-uri 'none';");
  source->OverrideContentSecurityPolicy(Directive::FormAction,
                                        "form-action 'none';");
  source->OverrideContentSecurityPolicy(
      Directive::FrameSrc,
      trusted ? "frame-src chrome-untrusted://krypton-documents;"
              : "frame-src 'none';");
  source->OverrideContentSecurityPolicy(Directive::WorkerSrc,
                                        "worker-src 'none';");
}
bool Enabled(content::BrowserContext* context) {
  auto* profile = Profile::FromBrowserContext(context);
  return profile->IsRegularProfile() && !profile->IsOffTheRecord();
}
class DocumentHandler final : public content::WebUIMessageHandler,
                              public ui::SelectFileDialog::Listener {
 public:
  DocumentHandler(DocumentKind kind, std::string id)
      : kind_(kind), id_(std::move(id)) {}
  ~DocumentHandler() override {
    if (dialog_)
      dialog_->ListenerDestroyed();
  }
  void SetDark(bool dark) {
    dark_ = dark;
    if (IsJavascriptAllowed())
      CallJavascriptFunction("kryptonDocumentThemeChanged", dark);
  }

 private:
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "documentRequest",
        base::BindRepeating(&DocumentHandler::Handle, base::Unretained(this)));
  }
  void OnJavascriptDisallowed() override {
    weak_factory_.InvalidateWeakPtrs();
    pending_ = 0;
    if (dialog_)
      dialog_->ListenerDestroyed();
    dialog_.reset();
    export_token_.clear();
    export_png_.clear();
    store_.reset();
  }
  void Reply(std::string token, base::DictValue response) {
    if (pending_)
      --pending_;
    if (!IsJavascriptAllowed())
      return;
    if (const auto* id = response.FindString("id"); id && IsDocumentId(*id))
      id_ = *id;
    response.Set("dark", dark_);
    CallJavascriptFunction("kryptonDocumentReply", token, response);
  }
  void Fail(std::string token, std::string_view error) {
    if (IsJavascriptAllowed())
      CallJavascriptFunction("kryptonDocumentReply", token,
                             base::DictValue().Set("error", error));
  }
  void Handle(const base::ListValue& args) {
    auto* contents = web_ui()->GetWebContents();
    if (!Enabled(contents->GetBrowserContext()) || args.size() != 2 ||
        !args[0].is_string() || !args[1].is_dict())
      return;
    const std::string token = args[0].GetString();
    if (token.empty() || token.size() > 64 || !base::IsStringASCII(token))
      return;
    AllowJavascript();
    const auto& request = args[1].GetDict();
    const auto* op = request.FindString("operation");
    const auto* id = request.FindString("id");
    // Bound each primitive before cloning. Never accept nested objects, paths,
    // arbitrary file extensions, remote URLs or renderer-supplied destinations.
    size_t bytes = 0;
    if (request.size() > 6)
      return Fail(token, "Invalid document request.");
    for (const auto [key, value] : request) {
      if (!value.is_string() || key.size() > 32 ||
          value.GetString().size() > 12 * 1024 * 1024 ||
          (bytes += value.GetString().size()) > 13 * 1024 * 1024)
        return Fail(token, "Document request exceeds the size limit.");
    }
    if (!op || !id ||
        (!IsDocumentId(*id) && !(*op == "load" && *id == "new")) ||
        pending_ >= 2)
      return Fail(token, "Document is busy or the id is invalid.");
    auto store = DocumentStore::ForProfile(Profile::FromWebUI(web_ui()));
    if (!store)
      return Fail(token, "Documents are unavailable in this profile.");
    if (*op == "export") {
      const auto* format = request.FindString("format");
      if (dialog_ || !format || (*format != "png" && *format != "document") ||
          (kind_ == DocumentKind::kNote && *format == "png") ||
          !contents->GetPrimaryMainFrame()->HasTransientUserActivation())
        return Fail(token, "Choose Export again to open the save dialog.");
      const std::string extension = *format == "png" ? "png"
                                    : kind_ == DocumentKind::kNote
                                        ? "md"
                                        : "excalidraw";
      id_ = *id;
      export_token_ = token;
      export_format_ = *format;
      export_png_ = request.FindString("png") ? *request.FindString("png") : "";
      store_ = std::move(store);
      dialog_ = ui::SelectFileDialog::Create(
          this, std::make_unique<ChromeSelectFilePolicy>(contents));
      ui::SelectFileDialog::FileTypeInfo types;
      types.extensions = {{base::FilePath::FromUTF8Unsafe(extension).value()}};
      types.include_all_files = false;
      ++pending_;
      dialog_->SelectFile(
          ui::SelectFileDialog::SELECT_SAVEAS_FILE, u"Export",
          base::FilePath::FromUTF8Unsafe("Krypton." + extension), &types, 1,
          base::FilePath::FromUTF8Unsafe(extension).value(),
          contents->GetTopLevelNativeWindow(), nullptr);
      return;
    }
    if (*op != "load" && *op != "save" && *op != "list" && *op != "rename" &&
        *op != "delete" && *op != "insertImage")
      return Fail(token, "Unsupported document operation.");
    ++pending_;
    store->Request(kind_, request.Clone(),
                   base::BindOnce(&DocumentHandler::Reply,
                                  weak_factory_.GetWeakPtr(), token));
  }
  void FileSelected(const ui::SelectedFileInfo& file, int index) override {
    dialog_.reset();
    store_->Export(
        kind_, id_, std::move(export_format_), std::move(export_png_),
        file.file_path,
        base::BindOnce(&DocumentHandler::Reply, weak_factory_.GetWeakPtr(),
                       std::exchange(export_token_, {})));
    store_.reset();
  }
  void FileSelectionCanceled() override {
    dialog_.reset();
    store_.reset();
    export_png_.clear();
    Reply(std::exchange(export_token_, {}),
          base::DictValue().Set("canceled", true));
  }
  const DocumentKind kind_;
  std::string id_;
  size_t pending_ = 0;
  bool dark_ = false;
  scoped_refptr<DocumentStore> store_;
  scoped_refptr<ui::SelectFileDialog> dialog_;
  std::string export_token_, export_format_, export_png_;
  base::WeakPtrFactory<DocumentHandler> weak_factory_{this};
};
class DocumentsUI final : public content::WebUIController {
 public:
  DocumentsUI(content::WebUI* web_ui, const GURL& url)
      : WebUIController(web_ui) {
    const bool note = url.host() == "notes";
    std::string id(url.path().substr(1));
    if (id.empty())
      id = "new";
    auto* source = content::WebUIDataSource::CreateAndAdd(
        web_ui->GetWebContents()->GetBrowserContext(), std::string(url.host()));
    for (const auto& resource : kKryptonDocumentsResources)
      if (base::StartsWith(resource.path, "trusted/"))
        source->AddResourcePath(resource.path, resource.id);
    source->SetDefaultResource(IDR_KRYPTON_DOCUMENTS_TRUSTED_INDEX_HTML);
    Policy(source, true);
    web_ui->AddRequestableScheme(content::kChromeUIUntrustedScheme);
    auto handler = std::make_unique<DocumentHandler>(
        note ? DocumentKind::kNote : DocumentKind::kEasel, id);
    handler_ = handler.get();
    web_ui->AddMessageHandler(std::move(handler));
  }
  void SetDark(bool dark) { handler_->SetDark(dark); }
  bool IsJavascriptErrorReportingEnabled() override { return false; }
  WEB_UI_CONTROLLER_TYPE_DECL();

 private:
  raw_ptr<DocumentHandler> handler_ = nullptr;
};
WEB_UI_CONTROLLER_TYPE_IMPL(DocumentsUI)
class DocumentsUntrustedUI final : public ui::UntrustedWebUIController {
 public:
  explicit DocumentsUntrustedUI(content::WebUI* web_ui)
      : UntrustedWebUIController(web_ui) {
    auto* source = content::WebUIDataSource::CreateAndAdd(
        web_ui->GetWebContents()->GetBrowserContext(), kEditorSource);
    for (const auto& resource : kKryptonDocumentsResources)
      if (!base::StartsWith(resource.path, "trusted/"))
        source->AddResourcePath(resource.path, resource.id);
    source->SetDefaultResource(IDR_KRYPTON_DOCUMENTS_INDEX_HTML);
    source->AddFrameAncestor(GURL("chrome://notes"));
    source->AddFrameAncestor(GURL("chrome://easel"));
    Policy(source, false);
    // Schema-based paste uses DOMParser/innerHTML in the isolated renderer.
    // Keep legacy WebUI bindings off; allow no script eval or remote resources.
    source->DisableTrustedTypesCSP();
  }
  bool IsJavascriptErrorReportingEnabled() override { return false; }
};
template <typename T>
class DocumentsConfig final : public content::DefaultWebUIConfig<T> {
 public:
  DocumentsConfig(std::string_view scheme, std::string_view host)
      : content::DefaultWebUIConfig<T>(scheme, host) {}
  bool IsWebUIEnabled(content::BrowserContext* context) override {
    return Enabled(context);
  }
};
}  // namespace
void SetDocumentPageTheme(content::WebContents* contents, bool dark) {
  if (!contents)
    return;
  const auto& url = contents->GetLastCommittedURL();
  if (!url.SchemeIs(content::kChromeUIScheme) ||
      (url.host() != "notes" && url.host() != "easel"))
    return;
  if (auto* web_ui = contents->GetWebUI()) {
    if (auto* controller = web_ui->GetController()->GetAs<DocumentsUI>())
      controller->SetDark(dark);
  }
}
void RegisterDocumentWebUIConfigs() {
  RegisterLocalModelsWebUIConfig();
  auto& map = content::WebUIConfigMap::GetInstance();
  map.AddWebUIConfig(std::make_unique<DocumentsConfig<DocumentsUI>>(
      content::kChromeUIScheme, "notes"));
  map.AddWebUIConfig(std::make_unique<DocumentsConfig<DocumentsUI>>(
      content::kChromeUIScheme, "easel"));
  map.AddUntrustedWebUIConfig(
      std::make_unique<DocumentsConfig<DocumentsUntrustedUI>>(
          content::kChromeUIUntrustedScheme, "krypton-documents"));
}
}  // namespace krypton_ui
