// Copyright 2026 The Krypton Authors. GPL-3.0.
#include "chrome/browser/krypton_speech/recognizer_client.h"
#include <utility>
#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/speech/speech_recognition_service.h"
#include "chrome/browser/speech/speech_recognition_service_factory.h"
#include "content/public/browser/audio_service.h"
#include "media/audio/audio_device_description.h"
#include "media/base/audio_parameters.h"
#include "media/mojo/mojom/audio_stream_factory.mojom.h"

namespace krypton::speech {
RecognizerClient::RecognizerClient(Profile& profile,
                                   std::string language,
                                   Result result,
                                   Finished finished)
    : result_(std::move(result)),
      finished_(std::move(finished)),
      device_id_(media::AudioDeviceDescription::kDefaultDeviceId) {
  if (auto preferred =
          MediaCaptureDevicesDispatcher::GetInstance()
              ->GetPreferredAudioDeviceForBrowserContext(&profile, {})) {
    device_id_ = std::move(preferred->id);
  }
  auto* service = SpeechRecognitionServiceFactory::GetForProfile(&profile);
  if (!service)
    return;
  service->BindAudioSourceSpeechRecognitionContext(
      context_.BindNewPipeAndPassReceiver());
  auto options = media::mojom::SpeechRecognitionOptions::New();
  options->recognition_mode = media::mojom::SpeechRecognitionMode::kIme;
  options->recognizer_client_type =
      media::mojom::RecognizerClientType::kDictation;
  options->language = std::move(language);
  options->enable_formatting = true;
  options->is_server_based = false;
  options->allow_multi_language = false;
  context_->BindAudioSourceFetcher(
      fetcher_.BindNewPipeAndPassReceiver(),
      receiver_.BindNewPipeAndPassRemote(), std::move(options),
      base::BindPostTaskToCurrentDefault(base::BindOnce(
          &RecognizerClient::Bound, weak_factory_.GetWeakPtr())));
  context_.set_disconnect_handler(base::BindOnce(
      &RecognizerClient::Disconnected, weak_factory_.GetWeakPtr()));
  fetcher_.set_disconnect_handler(base::BindOnce(
      &RecognizerClient::Disconnected, weak_factory_.GetWeakPtr()));
}
RecognizerClient::~RecognizerClient() {
  Stop();
}
void RecognizerClient::Start() {
  active_ = true;
  if (!context_.is_bound()) {
    Finish(RecognizerError::kServiceUnavailable);
    return;
  }
  // Bound() starts capture after the existing utility interface is ready.
}
void RecognizerClient::Stop() {
  active_ = false;
  weak_factory_.InvalidateWeakPtrs();
  if (fetcher_.is_bound())
    fetcher_->Stop();
  fetcher_.reset();
  receiver_.reset();
  context_.reset();
  audio_system_.reset();
}
void RecognizerClient::Bound(bool) {
  if (!active_)
    return;
  audio_system_ = content::CreateAudioSystemForAudioService();
  audio_system_->GetInputStreamParameters(
      device_id_, base::BindOnce(&RecognizerClient::InputParameters,
                                 weak_factory_.GetWeakPtr()));
}
void RecognizerClient::InputParameters(
    const std::optional<media::AudioParameters>& params) {
  if (!active_ || !fetcher_.is_bound())
    return;
  if (!params || !params->IsValid()) {
    if (device_id_ != media::AudioDeviceDescription::kDefaultDeviceId) {
      // A previously preferred microphone may have been unplugged.
      device_id_ = media::AudioDeviceDescription::kDefaultDeviceId;
      audio_system_->GetInputStreamParameters(
          device_id_, base::BindOnce(&RecognizerClient::InputParameters,
                                     weak_factory_.GetWeakPtr()));
      return;
    }
    Finish(RecognizerError::kNoInputDevice);
    return;
  }
  mojo::PendingRemote<media::mojom::AudioStreamFactory> factory;
  content::GetAudioServiceStreamFactoryBinder().Run(
      factory.InitWithNewPipeAndPassReceiver());
  // Preserve the Windows device's reported format. The fetcher performs the
  // resampling and mono conversion before delivering it to the recognizer.
  fetcher_->Start(std::move(factory), device_id_, *params);
}
void RecognizerClient::OnSpeechRecognitionRecognitionEvent(
    const media::SpeechRecognitionResult& result,
    OnSpeechRecognitionRecognitionEventCallback reply) {
  const bool valid = active_ && result.transcription.size() <= 32768;
  std::move(reply).Run(valid && !result.is_final);
  if (!valid) {
    Finish(RecognizerError::kRecognitionFailed);
    return;
  }
  // Last statement: the receiver's owner may cancel/destroy this client.
  auto callback = result_;
  callback.Run(result.transcription, result.is_final);
}
void RecognizerClient::Finish(RecognizerError error) {
  if (!active_)
    return;
  Stop();
  if (finished_)
    std::move(finished_).Run(error);
}
void RecognizerClient::OnSpeechRecognitionError() {
  Finish(RecognizerError::kRecognitionFailed);
}
void RecognizerClient::Disconnected() {
  Finish(RecognizerError::kServiceUnavailable);
}
void RecognizerClient::OnSpeechRecognitionStopped() {
  Finish(RecognizerError::kNone);
}
void RecognizerClient::OnLanguageIdentificationEvent(
    media::mojom::LanguageIdentificationEventPtr) {}
}  // namespace krypton::speech
