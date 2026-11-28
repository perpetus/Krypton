#ifndef CHROME_BROWSER_KRYPTON_AGENT_PAGE_READER_H_
#define CHROME_BROWSER_KRYPTON_AGENT_PAGE_READER_H_

#include <stddef.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/functional/callback_forward.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "components/optimization_guide/content/browser/page_content_proto_provider.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {
class WebContents;
}

namespace krypton_agent {
class PrivacyFilter;

struct PageTarget {
  std::string document;
  std::string label;
  GURL link;
  int control_type = 0;
  std::optional<bool> checked;
  bool disabled = false;
  bool in_form = false;
  bool sensitive_field = false;
  bool actionable = false;

  bool operator==(const PageTarget&) const = default;
};

enum class HandoffReason { kNone, kSignIn, kVerify, kEnterInformation };
enum class PageReadMode { kActionable, kTextOnly, kResearch };

struct ResearchLink {
  GURL url;
  std::string title;
  bool heading = false;
};

struct PageOutline {
  PageOutline();
  PageOutline(PageOutline&&);
  PageOutline& operator=(PageOutline&&);
  ~PageOutline();

  bool ContainsVisible(const base::Value& condition) const;

  base::flat_map<int, PageTarget> targets;
  std::vector<std::string> rows;
  std::vector<ResearchLink> links;
  base::flat_map<int, size_t> element_rows;
  std::string document;
  std::string source_id;
  size_t hidden = 0;
  HandoffReason handoff = HandoffReason::kNone;
  bool sensitive_page = false;
  bool contains_redactions = false;
  bool instruction_like = false;
  bool user_content = false;
  bool truncated = false;
};

inline constexpr size_t kMaxPaintPreviewRecordingBytes = 16 * 1024 * 1024;

void ReadPage(content::WebContents& contents,
              optimization_guide::OnAIPageContentDone done);
PageOutline ExtractOutline(
    const optimization_guide::proto::AnnotatedPageContent& content,
    PrivacyFilter& filter,
    const url::Origin& origin,
    bool categorized_user_content = false,
    PageReadMode mode = PageReadMode::kActionable);
std::string WrapContent(std::string text,
                        const url::Origin& origin,
                        std::string_view tab = {},
                        std::string_view source_id = {});

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_PAGE_READER_H_
