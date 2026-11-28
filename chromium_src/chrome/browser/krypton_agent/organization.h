#ifndef CHROME_BROWSER_KRYPTON_AGENT_ORGANIZATION_H_
#define CHROME_BROWSER_KRYPTON_AGENT_ORGANIZATION_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/values.h"
#include "components/tabs/public/tab_interface.h"
#include "url/origin.h"

class BrowserWindowInterface;

namespace krypton_ui {
class BrowserAdapter;
}

namespace krypton_agent {
class PrivacyFilter;

class Organization {
 public:
  struct Operation {
    std::string_view name;
    std::string_view description;
    std::string_view required;
    std::string_view optional;
    bool confirm = false;
  };

  struct Result {
    std::string text;
    bool error = false;
    bool page_derived = false;
    bool needs_confirmation = false;
  };

  Organization();
  ~Organization();

  static const Operation* Find(std::string_view name);
  static void List(base::ListValue& tools);
  static bool Valid(const Operation& operation,
                    const base::DictValue& arguments);
  Result Run(const Operation& operation,
             const base::DictValue& arguments,
             BrowserWindowInterface& browser,
             PrivacyFilter& filter,
             const url::Origin& origin,
             bool requires_confirmation,
             bool approved = false);

 private:
  struct TabState {
    tabs::TabHandle tab;
    int position;
    uint64_t space;
    uint64_t folder;
    bool pinned;
    bool essential;
    std::string icon;
  };

  std::string Handle(tabs::TabInterface& tab);
  tabs::TabInterface* Resolve(std::string_view handle,
                              BrowserWindowInterface& browser);
  std::string CatalogHandle(char kind, uint64_t id);
  std::optional<uint64_t> ResolveCatalog(char kind,
                                         std::string_view handle) const;
  Result Read(const Operation& operation,
              const base::DictValue& arguments,
              BrowserWindowInterface& browser,
              krypton_ui::BrowserAdapter& adapter,
              PrivacyFilter& filter,
              const url::Origin& origin);
  void Remember(BrowserWindowInterface& browser,
                const std::vector<tabs::TabInterface*>& tabs);

  base::flat_map<std::string, tabs::TabHandle, std::less<>> handles_;
  base::flat_map<std::string, std::pair<char, uint64_t>, std::less<>> catalog_;
  std::vector<std::vector<TabState>> undo_;
};

}  // namespace krypton_agent

#endif  // CHROME_BROWSER_KRYPTON_AGENT_ORGANIZATION_H_
