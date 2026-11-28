#include "chrome/browser/krypton_agent/privacy_filter.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "base/i18n/icubridge/icu_bridge.h"
#include "base/i18n/icubridge/normalizer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "crypto/hash.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "third_party/icu/source/common/unicode/utf16.h"

namespace krypton_agent {
namespace {

std::string Normalize(std::string_view input) {
  auto normalized = base::i18n::IcuBridge::GetInstance().normalizer().Normalize(
      base::i18n::IcuBridge::Normalizer::NormalizationForm::NFC,
      base::UTF8ToUTF16(input));
  std::u16string clean;
  for (size_t index = 0; index < normalized.size();) {
    const size_t start = index;
    UChar32 point;
    U16_NEXT(normalized.data(), index, normalized.size(), point);
    if (u_charType(point) != U_CONTROL_CHAR &&
        !u_hasBinaryProperty(point, UCHAR_DEFAULT_IGNORABLE_CODE_POINT) &&
        !u_hasBinaryProperty(point, UCHAR_BIDI_CONTROL)) {
      clean.append(normalized, start, index - start);
    } else if (point == '\n' || point == '\r' || point == '\t') {
      clean.push_back(u' ');
    }
  }
  return base::UTF16ToUTF8(clean);
}

bool Luhn(std::string_view digits) {
  unsigned sum = 0;
  bool double_digit = false;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    unsigned digit = *it - '0';
    if (double_digit) {
      digit *= 2;
      if (digit > 9) {
        digit -= 9;
      }
    }
    sum += digit;
    double_digit = !double_digit;
  }
  return sum % 10 == 0;
}

bool IsIban(std::string_view text) {
  if (text.size() < 15 || text.size() > 34 || !base::IsAsciiAlpha(text[0]) ||
      !base::IsAsciiAlpha(text[1]) || !base::IsAsciiDigit(text[2]) ||
      !base::IsAsciiDigit(text[3])) {
    return false;
  }
  unsigned remainder = 0;
  for (size_t index = 0; index < text.size(); ++index) {
    const char letter = base::ToUpperASCII(text[(index + 4) % text.size()]);
    if (base::IsAsciiDigit(letter)) {
      remainder = (remainder * 10 + letter - '0') % 97;
    } else if (base::IsAsciiAlpha(letter)) {
      remainder = (remainder * 100 + letter - 'A' + 10) % 97;
    } else {
      return false;
    }
  }
  return remainder == 1;
}

std::string_view TokenKind(std::string_view token) {
  const auto at = token.find('@');
  if (at != 0 && at != token.npos && token.find('.', at) != token.npos) {
    return "EMAIL";
  }
  if (IsIban(token)) {
    return "IBAN";
  }
  const bool token_chars = std::ranges::all_of(token, [](char letter) {
    return base::IsAsciiAlpha(letter) || base::IsAsciiDigit(letter) ||
           letter == '-' || letter == '_' || letter == '=' || letter == '+' ||
           letter == '/' || letter == '.';
  });
  if (token_chars && (token.size() >= 32 || token.starts_with("eyJ"))) {
    return "TOKEN";
  }
  return {};
}

std::vector<std::string_view> Words(std::string_view value) {
  std::vector<std::string_view> words;
  const auto is_word = [](char c) {
    return base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) ||
           static_cast<unsigned char>(c) >= 0x80;
  };
  size_t pos = 0;
  while (pos < value.size()) {
    if (!is_word(value[pos])) {
      ++pos;
      continue;
    }
    const size_t beginning = pos++;
    while (pos < value.size() && is_word(value[pos])) {
      ++pos;
    }
    words.push_back(value.substr(beginning, pos - beginning));
  }
  return words;
}

template <typename Visit>
void VisitShingles(std::string_view text, Visit visit) {
  const std::string normalized = base::ToLowerASCII(Normalize(text));
  const auto words = Words(normalized);
  for (size_t index = 0; index + 2 < words.size(); ++index) {
    const auto hash = crypto::hash::Sha256(base::StrCat(
        {words[index], " ", words[index + 1], " ", words[index + 2]}));
    if (!visit(hash)) {
      return;
    }
  }
}

}  // namespace

PrivacyFilter::PrivacyFilter(PlaceholderMode mode) : mode_(mode) {}
PrivacyFilter::~PrivacyFilter() = default;

std::string PrivacyFilter::Remember(std::string_view kind,
                                    std::string_view value,
                                    const url::Origin& origin) {
  ++redaction_count_;
  if (mode_ == PlaceholderMode::kRedact || entries_.size() >= 1024 ||
      value.size() > 4096) {
    return "[REDACTED]";
  }
  auto name =
      base::StrCat({"[", kind, "_", base::NumberToString(next_id_++), "]"});
  entries_.emplace(name, Entry{std::string(value), origin});
  return name;
}

std::string PrivacyFilter::Field(std::string_view value,
                                 const url::Origin& origin) {
  if (value.empty()) {
    return {};
  }
  TrackText(value.substr(0, 4096), origin);
  return Remember("FIELD", value, origin);
}

std::string PrivacyFilter::Url(const GURL& url, const url::Origin& origin) {
  if (!url.SchemeIsHTTPOrHTTPS()) {
    ++redaction_count_;
    return {};
  }
  if (url.has_username() || url.has_password() || url.has_query() ||
      url.has_ref()) {
    ++redaction_count_;
  }
  GURL::Replacements trim;
  trim.ClearUsername();
  trim.ClearPassword();
  trim.ClearQuery();
  trim.ClearRef();
  const GURL safe = url.ReplaceComponents(trim);
  return base::StrCat(
      {url::Origin::Create(safe).Serialize(), Text(safe.path(), origin)});
}

std::string PrivacyFilter::Text(std::string_view input,
                                const url::Origin& origin) {
  constexpr size_t kMaxInput = 8192;
  std::string normalized = Normalize(input.substr(0, kMaxInput));
  TrackText(normalized, origin);
  std::string result;
  std::string_view remaining = normalized;
  while (!remaining.empty()) {
    if (remaining.starts_with("https://") || remaining.starts_with("http://")) {
      const size_t end = remaining.find_first_of(" <>\"'\\");
      result += Url(GURL(remaining.substr(0, end)), origin);
      remaining.remove_prefix(std::min(end, remaining.size()));
      continue;
    }

    if (base::IsAsciiDigit(remaining.front()) || remaining.front() == '+') {
      size_t end = 0;
      size_t last_digit = 0;
      std::string digits;
      for (; end < remaining.size(); ++end) {
        const char letter = remaining[end];
        if (base::IsAsciiDigit(letter)) {
          digits.push_back(letter);
          last_digit = end + 1;
        } else if (letter != '+' && letter != ' ' && letter != '-' &&
                   letter != '(' && letter != ')' && letter != '.') {
          break;
        }
      }
      if (digits.size() >= 7) {
        std::string_view kind = "ID";
        if (digits.size() >= 13 && digits.size() <= 19 && Luhn(digits)) {
          kind = "CARD";
        } else if (digits.size() >= 10 && digits.size() <= 15) {
          kind = "PHONE";
        }
        result += Remember(kind, remaining.substr(0, last_digit), origin);
        remaining.remove_prefix(last_digit);
        continue;
      }
    }

    constexpr std::string_view kSeparators = " ,;:<>\"'(){}[]";
    const size_t word_size = remaining.find_first_of(kSeparators);
    if (!word_size) {
      result.push_back(remaining.front());
      remaining.remove_prefix(1);
      continue;
    }
    const auto word = remaining.substr(0, word_size);
    const auto kind = TokenKind(word);
    if (kind.empty()) {
      result.append(word);
    } else {
      result += Remember(kind, word, origin);
    }
    remaining.remove_prefix(word.size());
  }
  return result;
}

std::optional<std::string> PrivacyFilter::Resolve(
    std::string_view text,
    const url::Origin& origin) const {
  const auto found = entries_.find(text);
  if (found != entries_.end()) {
    if (found->second.origin == origin) {
      return found->second.value;
    }
    return std::nullopt;
  }
  if (text.find('[') != text.npos) {
    return std::nullopt;
  }
  return std::string(text);
}

void PrivacyFilter::TrackText(std::string_view text,
                              const url::Origin& origin) {
  if (taint_saturated_) {
    return;
  }
  VisitShingles(text, [&](const auto& hash) {
    constexpr size_t kMaxFingerprints = 65536;
    if (fingerprints_.size() >= kMaxFingerprints) {
      taint_saturated_ = true;
      return false;
    }
    auto [entry, inserted] = fingerprints_.try_emplace(hash, Source{origin});
    if (!inserted && entry->second.origin != origin) {
      entry->second.other_origin = origin;
    }
    return true;
  });
}

std::optional<url::Origin> PrivacyFilter::FindTaint(
    std::string_view value,
    const url::Origin& destination,
    bool allow_same_origin) const {
  if (value.empty()) {
    return std::nullopt;
  }
  const std::string normalized = base::ToLowerASCII(Normalize(value));
  for (const auto& [placeholder, entry] : entries_) {
    const auto raw = base::ToLowerASCII(Normalize(entry.value));
    if (normalized.find(base::ToLowerASCII(placeholder)) != std::string::npos ||
        (!raw.empty() && normalized.find(raw) != std::string::npos)) {
      return entry.origin;
    }
  }
  std::optional<url::Origin> source;
  VisitShingles(normalized, [&](const auto& hash) {
    const auto match = fingerprints_.find(hash);
    if (match == fingerprints_.end()) {
      return true;
    }
    const auto& entry = match->second;
    source = allow_same_origin && entry.origin == destination
                 ? entry.other_origin
                 : std::make_optional(entry.origin);
    return !source;
  });
  return source;
}

void PrivacyFilter::ClearPlaceholders() {
  entries_.clear();
}

void PrivacyFilter::Clear() {
  ClearPlaceholders();
  fingerprints_.clear();
  taint_saturated_ = false;
}

bool PrivacyFilter::LooksLikeInstruction(std::string_view input,
                                         bool user_content) {
  const auto lower = base::ToLowerASCII(input);
  static constexpr std::array<std::string_view, 8> kPhrases{
      "ignore previous",   "ignore all", "system prompt",
      "you are now",       "tools.call", "send the following",
      "developer message", "read_page"};
  if (std::ranges::any_of(
          kPhrases, [&lower](auto phrase) { return lower.contains(phrase); })) {
    return true;
  }
  static constexpr std::array<std::string_view, 7> kUserContentPhrases{
      "follow these instructions",
      "call the tool",
      "do not tell",
      "copy and send",
      "override instructions",
      "reveal the prompt",
      "instructions for the agent"};
  return user_content &&
         std::ranges::any_of(kUserContentPhrases, [&lower](auto phrase) {
           return lower.contains(phrase);
         });
}

}  // namespace krypton_agent
