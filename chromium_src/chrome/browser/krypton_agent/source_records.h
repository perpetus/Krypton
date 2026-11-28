#ifndef CHROME_BROWSER_KRYPTON_AGENT_SOURCE_RECORDS_H_
#define CHROME_BROWSER_KRYPTON_AGENT_SOURCE_RECORDS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/flat_map.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}

namespace krypton_agent {

struct SourceCitation {
  std::string id;
  GURL destination;
  std::string title;
  std::string site;
  std::string display_url;
  std::vector<uint8_t> favicon_png;
  bool cited = false;
};

class SourceRecords {
 public:
  SourceRecords();
  ~SourceRecords();
  SourceRecords(const SourceRecords&) = delete;
  SourceRecords& operator=(const SourceRecords&) = delete;

  void StartTurn();
  std::string Record(content::WebContents& contents);
  void Reuse(std::string_view id);
  std::vector<SourceCitation> ForAnswer(std::string_view answer) const;

 private:
  base::flat_map<std::string, SourceCitation> entries_;
  std::vector<std::string> current_turn_;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_SOURCE_RECORDS_H_
