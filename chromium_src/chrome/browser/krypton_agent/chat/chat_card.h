#ifndef CHROME_BROWSER_KRYPTON_AGENT_CHAT_CHAT_CARD_H_
#define CHROME_BROWSER_KRYPTON_AGENT_CHAT_CHAT_CARD_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/browser/krypton_agent/isolated_network.h"
#include "chrome/browser/krypton_agent/source_records.h"
#include "ui/views/view_tracker.h"

class Profile;

namespace content {
class WebContents;
}

namespace gfx {
class Rect;
}

namespace views {
class View;
}

namespace krypton_agent {

class HtmlPreview;
std::optional<std::string> GetHtmlGuide(std::string_view topic);

// One entry of an assistant turn's work, as the transcript shows it.
struct TranscriptEntry {
  std::string id, kind, status, title, detail, text, output;
  // Where the entry sits in its message, in UTF-16 code units.
  uint32_t offset = 0;
};

// Owns only the transcript. The caller places its WebContents in the bounds
// reserved by the native layout.
class ChatCard {
 public:
  static std::unique_ptr<ChatCard> Create(Profile& profile,
                                          IsolatedLinkHandler on_link);
  ~ChatCard();

  ChatCard(const ChatCard&) = delete;
  ChatCard& operator=(const ChatCard&) = delete;

  content::WebContents& contents();
  bool SetPlacement(views::View& parent, const gfx::Rect& bounds);
  bool Focus();
  void Detach();
  void Append(std::string_view id, std::string_view text);
  void Update(std::string_view id, std::string_view text);
  void Finish(std::string_view id);
  void SetSources(std::string_view id, std::vector<SourceCitation> sources);
  void SetItem(std::string_view message_id, TranscriptEntry item);
  void SetWorking(bool working);
  void SetDark(bool dark);
  void SetPromptSuggestionHandler(
      base::RepeatingCallback<void(std::string)> handler);
  std::string ShowHtml(std::string_view message_id,
                       std::string html,
                       std::string title,
                       int width,
                       int height);
  void PreviewHtml(std::string html,
                   int width,
                   bool dark,
                   base::ListValue steps,
                   base::OnceCallback<void(base::DictValue)> reply);
  void CancelPreview();

 private:
  explicit ChatCard(std::unique_ptr<content::WebContents> contents);

  std::unique_ptr<content::WebContents> contents_;
  views::ViewTracker attached_view_;
  std::unique_ptr<HtmlPreview> preview_;
  bool dark_ = false;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_CHAT_CHAT_CARD_H_
