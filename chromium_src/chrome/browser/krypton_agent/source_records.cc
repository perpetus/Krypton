#include "chrome/browser/krypton_agent/source_records.h"

#include <set>
#include <utility>

#include "base/memory/ref_counted_memory.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "components/search_engines/template_url_service.h"
#include "content/public/browser/web_contents.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "ui/gfx/image/image.h"

namespace krypton_agent {
namespace {

constexpr size_t kMaximumSources = 100;

std::vector<uint8_t> CaptureFavicon(content::WebContents& contents) {
  const auto* driver =
      favicon::ContentFaviconDriver::FromWebContents(&contents);
  if (!driver || !driver->FaviconIsValid()) {
    return {};
  }
  const auto image = driver->GetFavicon();
  if (image.IsEmpty() || image.Width() > 64 || image.Height() > 64) {
    return {};
  }
  const auto png = image.As1xPNGBytes();
  if (!png || png->size() > 16 * 1024) {
    return {};
  }
  return std::vector<uint8_t>(png->begin(), png->end());
}

}  // namespace

SourceRecords::SourceRecords() = default;
SourceRecords::~SourceRecords() = default;

void SourceRecords::StartTurn() {
  current_turn_.clear();
}

std::string SourceRecords::Record(content::WebContents& contents) {
  const auto& url = contents.GetLastCommittedURL();
  if (!url.SchemeIsHTTPOrHTTPS() || url.has_username() || url.has_password() ||
      url.spec().size() > 8192 || entries_.size() >= kMaximumSources) {
    return {};
  }
  auto* search = TemplateURLServiceFactory::GetForProfile(
      Profile::FromBrowserContext(contents.GetBrowserContext()));
  if (search && search->IsSearchResultsPageFromDefaultSearchProvider(url)) {
    return {};
  }
  GURL::Replacements replacements;
  replacements.ClearQuery();
  replacements.ClearRef();
  auto display_url = url.ReplaceComponents(replacements).spec();
  auto site = net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  if (site.empty()) {
    site = url.host();
  }
  std::string title;
  base::TruncateUTF8ToByteSize(base::UTF16ToUTF8(contents.GetTitle()), 256,
                               &title);
  const auto id =
      base::StrCat({"S", base::NumberToString(entries_.size() + 1)});
  entries_.emplace(
      id, SourceCitation{id, url, std::move(title), std::move(site),
                         std::move(display_url), CaptureFavicon(contents)});
  current_turn_.push_back(id);
  return id;
}

void SourceRecords::Reuse(std::string_view id) {
  if (const auto entry = entries_.find(id); entry != entries_.end()) {
    current_turn_.push_back(entry->first);
  }
}

std::vector<SourceCitation> SourceRecords::ForAnswer(
    std::string_view answer) const {
  std::vector<SourceCitation> sources;
  std::set<std::string_view> included;
  for (size_t offset = 0;
       (offset = answer.find("[S", offset)) != std::string_view::npos;
       offset += 2) {
    const auto candidate = answer.substr(offset + 1, 5);
    const auto close = candidate.find(']');
    if (close == std::string_view::npos) {
      continue;
    }
    const auto entry = entries_.find(candidate.substr(0, close));
    if (entry != entries_.end() && included.insert(entry->first).second) {
      sources.push_back(entry->second);
      sources.back().cited = true;
    }
  }
  for (const auto& id : current_turn_) {
    if (included.insert(id).second) {
      sources.push_back(entries_.at(id));
    }
  }
  return sources;
}

}  // namespace krypton_agent
