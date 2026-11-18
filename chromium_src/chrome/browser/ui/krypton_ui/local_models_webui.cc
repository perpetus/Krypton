// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/ui/krypton_ui/local_models_webui.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/krypton_speech/installer.h"
#include "chrome/browser/krypton_translate/installer.h"
#include "chrome/browser/profiles/profile.h"
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

namespace krypton_ui {
namespace {
namespace voice = krypton::speech;
namespace translation = krypton::translation;
constexpr char kHost[] = "krypton-models";
constexpr char kHtml[] =
    R"html(<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>Local models</title><link rel="stylesheet" href="style.css"><script src="app.js" defer></script></head>
<body><main><h1>Local models</h1><p>Translation and voice run on this device. Packs download only when you choose Download. Nothing updates automatically.</p>
<p>Downloads contact the named upstream distributor. No page text, audio or transcripts are uploaded. Packs are shared by profiles on this device.</p>
<p id="status" role="status">Checking installed packs…</p><h2>Translation</h2><div id="translation"></div><h2>Voice</h2><div id="voice"></div></main></body></html>)html";
constexpr char kCss[] =
    R"css(:root{color-scheme:light dark;font:menu}body{margin:0;background:Canvas;color:CanvasText}main{max-width:58rem;margin:3rem auto;padding:0 2rem}p{line-height:1.6}section{display:flex;gap:1rem;align-items:center;border-bottom:1px solid GrayText;padding:1rem 0}section span{flex:1;white-space:pre-line}button{font:inherit;padding:.5rem 1rem}h2{margin-top:2rem})css";
constexpr char kJs[] = R"js('use strict';
window.kryptonLocalModels = rows => {
  for (const kind of ['translation','voice']) {
    const fragment = document.createDocumentFragment();
    for (const row of rows.filter(row => row.kind === kind)) {
      const section = document.createElement('section');
      const label = document.createElement('span');
      label.textContent = row.label + '\n' + row.status;
      const button = document.createElement('button');
      button.textContent = row.installed ? 'Remove' : `Download ≈${row.mb} MB from ${row.distributor}`;
      button.disabled = row.busy;
      button.addEventListener('click', () => {
        button.disabled = true;
        chrome.send('localModels', [row.installed ? 'remove' : 'download', row.kind, row.id]);
      });
      section.append(label, button); fragment.append(section);
    }
    document.getElementById(kind).replaceChildren(fragment);
  }
  document.getElementById('status').textContent = '';
};
chrome.send('localModels', ['list']);
)js";
bool Enabled(content::BrowserContext* context) {
  auto* profile = Profile::FromBrowserContext(context);
  return profile->IsRegularProfile() && !profile->IsOffTheRecord();
}
class ModelsHandler final
    : public content::WebUIMessageHandler,
      public on_device_translation::OnDeviceTranslationInstaller::Observer,
      public ::speech::SodaInstaller::Observer {
 public:
  ModelsHandler() {
    if (auto* installer = translation::Installer::Get()) {
      translation_ = installer->GetWeakPtr();
      translation_->AddObserver(this);
    }
    if (auto* installer = voice::Installer::Get()) {
      voice_ = installer->GetWeakPtr();
      voice_->AddObserver(this);
    }
  }
  ~ModelsHandler() override {
    if (translation_)
      translation_->RemoveObserver(this);
    if (voice_)
      voice_->RemoveObserver(this);
  }

 private:
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "localModels",
        base::BindRepeating(&ModelsHandler::Handle, base::Unretained(this)));
  }
  void OnJavascriptDisallowed() override { weak_factory_.InvalidateWeakPtrs(); }
  void Handle(const base::ListValue& args) {
    auto* contents = web_ui()->GetWebContents();
    if (!Enabled(contents->GetBrowserContext()) || args.empty() ||
        !args[0].is_string())
      return;
    const auto& operation = args[0].GetString();
    if (operation == "list" && args.size() == 1) {
      AllowJavascript();
      if (scanned_) {
        Publish();
        return;
      }
      scanned_ = true;
      const auto done = base::BindRepeating(&ModelsHandler::Publish,
                                            weak_factory_.GetWeakPtr());
      if (translation_)
        translation_->ScanInstalled(base::BindOnce(done));
      if (voice_)
        voice_->Scan(base::BindOnce(done));
      Publish();
      return;
    }
    if (!IsJavascriptAllowed() || args.size() != 3 || !args[1].is_string() ||
        !args[2].is_string() || args[2].GetString().size() > 64 ||
        (operation != "download" && operation != "remove") ||
        !contents->GetPrimaryMainFrame()->HasTransientUserActivation())
      return;
    const auto& kind = args[1].GetString();
    const auto& id = args[2].GetString();
    auto* profile = Profile::FromWebUI(web_ui());
    if (kind == "translation" && translation_) {
      for (const auto& pack : translation::ModelPacks()) {
        if (base::NumberToString(static_cast<int>(pack.key)) != id)
          continue;
        if (operation == "download")
          translation_->RequestDownload(*profile, pack.key);
        else
          translation_->UnInstallLanguagePack(pack.key);
        break;
      }
    } else if (kind == "voice" && voice_) {
      for (const auto& pack : krypton_voice::kPacks) {
        if (pack.id != id)
          continue;
        if (operation == "download")
          voice_->RequestDownload(*profile, id);
        else
          voice_->RemovePack(id);
        break;
      }
    }
    Publish();
  }
  void Publish() {
    if (!IsJavascriptAllowed())
      return;
    base::ListValue rows;
    if (translation_) {
      const auto installed = translation_->InstalledLanguagePacks();
      for (const auto& pack : translation::ModelPacks()) {
        const int progress = translation_->Progress(pack.key);
        const bool ready = installed.contains(pack.key);
        const auto error = translation_->Error(pack.key);
        rows.Append(
            base::DictValue()
                .Set("kind", "translation")
                .Set("id", base::NumberToString(static_cast<int>(pack.key)))
                .Set("label", "English ↔ " + std::string(pack.language))
                .Set("installed", ready)
                .Set("busy", translation_->IsDownloading())
                .Set("mb",
                     static_cast<int>((pack.download_bytes + 999999) / 1000000))
                .Set("distributor", "Mozilla")
                .Set("status",
                     !error.empty() ? error
                     : progress >= 0
                         ? "Downloading " + base::NumberToString(progress) + "%"
                     : ready ? "Installed"
                             : "Not installed"));
      }
    }
    if (voice_)
      for (const auto& pack : krypton_voice::kPacks) {
        const int progress = voice_->Progress(pack.id);
        const bool ready = voice_->Ready(pack.id);
        const auto error = voice_->Error(pack.id);
        rows.Append(
            base::DictValue()
                .Set("kind", "voice")
                .Set("id", pack.id)
                .Set("label", std::string(pack.language) +
                                  (pack.lightweight ? " — Tiny (lightweight)"
                                                    : " — Small"))
                .Set("installed", ready)
                .Set("busy", voice_->IsDownloading())
                .Set("mb",
                     static_cast<int>((pack.download_size + 999999) / 1000000))
                .Set("distributor", "Moonshine AI")
                .Set("status",
                     !error.empty() ? error
                     : progress >= 0
                         ? "Downloading " + base::NumberToString(progress) + "%"
                     : ready ? "Installed"
                             : "Not installed"));
      }
    CallJavascriptFunction("kryptonLocalModels", rows);
  }
  void OnInstallationChanged() override { Publish(); }
  void OnLanguagePackInstalled(translation::PackKey) override { Publish(); }
  void OnLanguagePackInstallationChanged(translation::PackKey) override {
    Publish();
  }
  void OnLanguagePackProgress(translation::PackKey, int) override { Publish(); }
  void OnSodaInstalled(::speech::LanguageCode) override { Publish(); }
  void OnSodaInstallError(::speech::LanguageCode,
                          ::speech::SodaInstaller::ErrorCode) override {
    Publish();
  }
  void OnSodaProgress(::speech::LanguageCode, int) override { Publish(); }
  bool scanned_ = false;
  base::WeakPtr<translation::Installer> translation_;
  base::WeakPtr<voice::Installer> voice_;
  base::WeakPtrFactory<ModelsHandler> weak_factory_{this};
};
class ModelsUI final : public content::WebUIController {
 public:
  explicit ModelsUI(content::WebUI* web_ui) : WebUIController(web_ui) {
    auto* source = content::WebUIDataSource::CreateAndAdd(
        web_ui->GetWebContents()->GetBrowserContext(), kHost);
    source->SetRequestFilter(
        base::BindRepeating([](const std::string& path) {
          return path.empty() || path == "index.html" || path == "app.js" ||
                 path == "style.css";
        }),
        base::BindRepeating(
            [](const std::string& path,
               content::WebUIDataSource::GotDataCallback callback) {
              const std::string_view data = path == "app.js"      ? kJs
                                            : path == "style.css" ? kCss
                                                                  : kHtml;
              std::move(callback).Run(
                  base::MakeRefCounted<base::RefCountedString>(
                      std::string(data)));
            }));
    using D = network::mojom::CSPDirectiveName;
    source->OverrideContentSecurityPolicy(D::DefaultSrc, "default-src 'none';");
    source->OverrideContentSecurityPolicy(D::ScriptSrc, "script-src 'self';");
    source->OverrideContentSecurityPolicy(D::StyleSrc, "style-src 'self';");
    source->OverrideContentSecurityPolicy(D::ConnectSrc, "connect-src 'none';");
    source->OverrideContentSecurityPolicy(D::FrameAncestors,
                                          "frame-ancestors 'none';");
    web_ui->AddMessageHandler(std::make_unique<ModelsHandler>());
  }
  bool IsJavascriptErrorReportingEnabled() override { return false; }
};
class ModelsConfig final : public content::DefaultWebUIConfig<ModelsUI> {
 public:
  ModelsConfig() : DefaultWebUIConfig(content::kChromeUIScheme, kHost) {}
  bool IsWebUIEnabled(content::BrowserContext* context) override {
    return Enabled(context);
  }
};
}  // namespace
void RegisterLocalModelsWebUIConfig() {
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<ModelsConfig>());
}
}  // namespace krypton_ui
