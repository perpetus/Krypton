// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/krypton_translate/provider.h"

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/krypton_translate/installer.h"
#include "chrome/browser/krypton_translate/model_packs.h"
#include "chrome/browser/on_device_translation/service_controller_manager_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/on_device_translation/service_controller.h"
#include "components/on_device_translation/service_controller_manager.h"
#include "components/on_device_translation/translator.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "components/translate/core/browser/translation_provider.h"
#include "components/translate/core/common/translate_errors.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "third_party/blink/public/mojom/ai/ai_common.mojom.h"
#include "third_party/blink/public/mojom/ai/model_streaming_responder.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace krypton::translation {
namespace {
using Error = translate::TranslateErrors;
using Pair = std::pair<std::string, std::string>;
using Controller = on_device_translation::OnDeviceTranslationController;

class Session final : public translate::PlainTextTranslationSession,
                      public blink::mojom::ModelStreamingResponder {
 public:
  Session(Profile& profile, const GURL& url)
      : profile_(&profile), origin_(url::Origin::Create(url)) {}
  ~Session() override = default;

 private:
  bool Allowed() const {
    return !profile_->IsOffTheRecord() &&
           translate::TranslatePrefs(profile_->GetPrefs())
               .IsTranslateAllowedByPolicy();
  }
  void TranslateTexts(const std::string& source,
                      const std::string& target,
                      std::vector<std::u16string> texts,
                      TextsCallback callback) override {
    if (callback_) {
      std::move(callback).Run(std::nullopt, source, Error::TRANSLATION_ERROR);
      return;
    }
    if (!Allowed() || !Installer::Get()) {
      std::move(callback).Run(std::nullopt, source,
                              Error::INITIALIZATION_ERROR);
      return;
    }
    pair_ = {CanonicalLanguage(source), CanonicalLanguage(target)};
    if (!RequiredPacks(pair_.first, pair_.second)) {
      std::move(callback).Run(std::nullopt, source,
                              Error::UNSUPPORTED_LANGUAGE);
      return;
    }
    size_t characters = 0;
    for (const auto& text : texts)
      characters += text.size();
    if (texts.size() > 2048 || characters > 2000) {
      std::move(callback).Run(std::nullopt, source, Error::TRANSLATION_ERROR);
      return;
    }
    inputs_ = std::move(texts);
    outputs_.clear();
    outputs_.reserve(inputs_.size());
    callback_ = std::move(callback);
    Installer::Get()->PreparePair(
        pair_.first, pair_.second,
        base::BindOnce(&Session::Prepared, weak_factory_.GetWeakPtr()));
  }
  void Prepared(bool ready) {
    if (!ready || !Allowed()) {
      Finish(Error::INITIALIZATION_ERROR);
      return;
    }
    if (translators_.contains(pair_)) {
      Next();
      return;
    }
    if (translators_.size() >= 4) {
      translators_.clear();
    }
    auto* manager =
        on_device_translation::ServiceControllerManagerFactory::GetInstance()
            ->Get(profile_);
    if (!manager) {
      Finish(Error::INITIALIZATION_ERROR);
      return;
    }
    manager->CreateTranslator(
        origin_, pair_.first, pair_.second,
        base::BindOnce(&Session::Created, weak_factory_.GetWeakPtr()));
  }
  void Created(
      base::expected<
          mojo::PendingRemote<on_device_translation::mojom::OnDeviceTranslator>,
          Controller::CreateTranslatorError> result) {
    if (!result.has_value() || !Allowed()) {
      Finish(Error::INITIALIZATION_ERROR);
      return;
    }
    translators_[pair_] = std::make_unique<on_device_translation::Translator>(
        base::BindRepeating(
            [](base::WeakPtr<Session> self) { return self && self->Allowed(); },
            weak_factory_.GetWeakPtr()),
        pair_.first, pair_.second, std::move(*result));
    Next();
  }
  void Next() {
    if (!Allowed()) {
      Finish(Error::TRANSLATION_ERROR);
      return;
    }
    while (outputs_.size() < inputs_.size() && inputs_[outputs_.size()].empty())
      outputs_.emplace_back();
    if (outputs_.size() == inputs_.size()) {
      Finish(Error::NONE);
      return;
    }
    const auto input = base::UTF16ToUTF8(inputs_[outputs_.size()]);
    if (input.size() > 8192) {
      Finish(Error::TRANSLATION_ERROR);
      return;
    }
    current_.clear();
    response_.reset();
    auto remote = response_.BindNewPipeAndPassRemote();
    response_.set_disconnect_handler(
        base::BindOnce(&Session::Disconnected, weak_factory_.GetWeakPtr()));
    translators_.at(pair_)->Translate(input, std::move(remote));
  }
  void OnStreaming(const std::string& text) override {
    if (text.size() > 65536 - current_.size()) {
      Finish(Error::TRANSLATION_ERROR);
      return;
    }
    current_ += text;
  }
  void OnCompletion(blink::mojom::ModelExecutionContextInfoPtr) override {
    response_.reset();
    if (!base::IsStringUTF8(current_)) {
      Finish(Error::TRANSLATION_ERROR);
      return;
    }
    outputs_.push_back(base::UTF8ToUTF16(current_));
    Next();
  }
  void OnError(blink::mojom::ModelStreamingResponseStatus,
               blink::mojom::QuotaErrorInfoPtr) override {
    translators_.erase(pair_);
    Finish(Error::TRANSLATION_ERROR);
  }
  void OnToolCalls(std::vector<blink::mojom::ToolCallPtr>) override {
    Finish(Error::TRANSLATION_ERROR);
  }
  void OnContextOverflow() override { Finish(Error::TRANSLATION_ERROR); }
  void Disconnected() {
    if (callback_)
      Finish(Error::TRANSLATION_ERROR);
  }
  void Finish(Error error) {
    response_.reset();
    auto callback = std::move(callback_);
    if (!callback)
      return;
    // The document may disappear in this callback. No member access follows it.
    auto output = error == Error::NONE ? std::make_optional(std::move(outputs_))
                                       : std::nullopt;
    std::string source = pair_.first;
    inputs_.clear();
    current_.clear();
    std::move(callback).Run(output, source, error);
  }

  // The DocumentService owns this session and dies before its BrowserContext.
  const raw_ptr<Profile> profile_;
  const url::Origin origin_;
  Pair pair_;
  TextsCallback callback_;
  std::vector<std::u16string> inputs_, outputs_;
  std::string current_;
  std::map<Pair, std::unique_ptr<on_device_translation::Translator>>
      translators_;
  mojo::Receiver<blink::mojom::ModelStreamingResponder> response_{this};
  base::WeakPtrFactory<Session> weak_factory_{this};
};

class Provider final : public translate::TranslationProvider {
 public:
  bool IsAvailable() const override { return Installer::Get() != nullptr; }
  bool IsOnDevice() const override { return true; }
  translate::TranslationBatchLimits GetBatchLimits() const override {
    return {16, 2000};
  }
  std::unique_ptr<translate::TranslationSession> StartSession(
      const GURL&) override {
    // Never infer a profile from an active window or a URL shared by profiles.
    return nullptr;
  }
  std::unique_ptr<translate::TranslationSession> StartSession(
      const GURL& url,
      content::BrowserContext* context) override {
    auto* profile = Profile::FromBrowserContext(context);
    if (!profile || profile->IsOffTheRecord() || !url.SchemeIsHTTPOrHTTPS() ||
        !translate::TranslatePrefs(profile->GetPrefs())
             .IsTranslateAllowedByPolicy())
      return nullptr;
    return std::make_unique<Session>(*profile, url);
  }
};
}  // namespace
void RegisterProvider() {
  translate::RegisterTranslationProvider(std::make_unique<Provider>());
}
}  // namespace krypton::translation
