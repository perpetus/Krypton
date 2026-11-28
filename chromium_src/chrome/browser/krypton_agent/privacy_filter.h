#ifndef CHROME_BROWSER_KRYPTON_AGENT_PRIVACY_FILTER_H_
#define CHROME_BROWSER_KRYPTON_AGENT_PRIVACY_FILTER_H_

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "base/containers/flat_map.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace krypton_agent {

class PrivacyFilter {
 public:
  enum class PlaceholderMode { kKeep, kRedact };
  explicit PrivacyFilter(PlaceholderMode mode = PlaceholderMode::kKeep);
  ~PrivacyFilter();

  std::string Text(std::string_view input, const url::Origin& origin);
  std::string Url(const GURL& url, const url::Origin& origin);
  std::string Field(std::string_view value, const url::Origin& origin);

  // Resolves only a complete placeholder. Embedded or foreign placeholders
  // must go through confirmation.
  std::optional<std::string> Resolve(std::string_view text,
                                     const url::Origin& origin) const;
  std::optional<url::Origin> FindTaint(std::string_view value,
                                       const url::Origin& destination,
                                       bool allow_same_origin) const;
  bool taint_saturated() const { return taint_saturated_; }
  uint64_t redaction_count() const { return redaction_count_; }
  void ClearPlaceholders();
  void Clear();
  static bool LooksLikeInstruction(std::string_view input,
                                   bool user_content = false);

 private:
  struct Entry {
    std::string value;
    url::Origin origin;
  };
  std::string Remember(std::string_view kind,
                       std::string_view value,
                       const url::Origin& origin);

  void TrackText(std::string_view text, const url::Origin& origin);
  using Fingerprint = std::array<uint8_t, 32>;
  struct Source {
    url::Origin origin;
    std::optional<url::Origin> other_origin;
  };
  base::flat_map<std::string, Entry, std::less<>> entries_;
  const PlaceholderMode mode_;
  std::map<Fingerprint, Source> fingerprints_;
  bool taint_saturated_ = false;
  uint64_t redaction_count_ = 0;
  unsigned next_id_ = 1;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_PRIVACY_FILTER_H_
