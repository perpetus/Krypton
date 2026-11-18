// Copyright 2026 The Krypton Authors. GPL-3.0.
#ifndef CHROME_BROWSER_KRYPTON_SPEECH_RECOGNIZER_CLIENT_H_
#define CHROME_BROWSER_KRYPTON_SPEECH_RECOGNIZER_CLIENT_H_
#include <memory>
#include <string>
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "media/audio/audio_system.h"
#include "media/mojo/mojom/speech_recognition.mojom.h"
#include "media/mojo/mojom/speech_recognition_service.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
class Profile;
namespace krypton::speech {
enum class RecognizerError {
  kNone,
  kServiceUnavailable,
  kNoInputDevice,
  kRecognitionFailed,
};
// Browser-owned Windows microphone client. It has no RenderFrameHost, site
// permission, Web Speech session, network recognizer or persistent audio sink.
class RecognizerClient final
    : public media::mojom::SpeechRecognitionRecognizerClient {
 public:
  using Result = base::RepeatingCallback<void(std::string, bool)>;
  using Finished = base::OnceCallback<void(RecognizerError)>;
  RecognizerClient(Profile& profile,
                   std::string language,
                   Result result,
                   Finished finished);
  ~RecognizerClient() override;
  void Start();
  void Stop();

 private:
  void Bound(bool multichannel);
  void InputParameters(const std::optional<media::AudioParameters>& params);
  void Disconnected();
  void OnSpeechRecognitionRecognitionEvent(
      const media::SpeechRecognitionResult& result,
      OnSpeechRecognitionRecognitionEventCallback reply) override;
  void OnSpeechRecognitionError() override;
  void OnSpeechRecognitionStopped() override;
  void OnLanguageIdentificationEvent(
      media::mojom::LanguageIdentificationEventPtr) override;
  void Finish(RecognizerError error);
  Result result_;
  Finished finished_;
  std::string device_id_;
  bool active_ = false;
  std::unique_ptr<media::AudioSystem> audio_system_;
  mojo::Remote<media::mojom::AudioSourceSpeechRecognitionContext> context_;
  mojo::Remote<media::mojom::AudioSourceFetcher> fetcher_;
  mojo::Receiver<media::mojom::SpeechRecognitionRecognizerClient> receiver_{
      this};
  base::WeakPtrFactory<RecognizerClient> weak_factory_{this};
};
}  // namespace krypton::speech
#endif
