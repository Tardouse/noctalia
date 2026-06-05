#include "system/app_identity.h"

#include "system/internal_app_metadata.h"
#include "util/string_utils.h"

#include <cctype>
#include <unordered_set>

namespace app_identity {

  namespace {

    std::string identityKey(std::string_view value) {
      std::string key;
      key.reserve(value.size());
      for (const unsigned char ch : value) {
        if (ch == '.' || ch == '-' || ch == '_' || std::isspace(ch) != 0) {
          continue;
        }
        key.push_back(static_cast<char>(std::tolower(ch)));
      }
      return key;
    }

    bool identityKeyMatches(std::string_view valueKey, std::string_view candidate) {
      if (candidate.empty()) {
        return false;
      }
      return valueKey == identityKey(candidate);
    }

    struct DesktopEntryResolution {
      DesktopEntry entry;
      bool matchedDesktopEntry = false;
    };

    [[nodiscard]] std::string desktopEntryStemLower(std::string_view desktopId) {
      const auto slash = desktopId.rfind('/');
      const auto base = slash == std::string_view::npos ? desktopId : desktopId.substr(slash + 1);
      const auto dot = base.rfind('.');
      return StringUtils::toLower(std::string(dot == std::string_view::npos ? base : base.substr(0, dot)));
    }

  } // namespace

  bool matchesLower(
      std::string_view valueLower, std::string_view idLower, std::string_view startupWmClassLower,
      std::string_view nameLower
  ) {
    if (valueLower.empty()) {
      return false;
    }
    const auto valueKey = identityKey(valueLower);
    return valueLower == idLower
        || valueLower == startupWmClassLower
        || valueLower == nameLower
        || (!valueKey.empty()
            && (identityKeyMatches(valueKey, idLower) || identityKeyMatches(valueKey, startupWmClassLower)));
  }

  bool desktopEntryMatchesLower(const DesktopEntry& entry, std::string_view valueLower) {
    return matchesLower(
        valueLower, StringUtils::toLower(entry.id), StringUtils::toLower(entry.startupWmClass), entry.nameLower
    );
  }

  std::optional<DesktopEntry> findDesktopEntry(
      std::string_view appKey, const std::vector<DesktopEntry>& allEntries, DesktopEntryLookupOptions options
  ) {
    if (appKey.empty()) {
      return std::nullopt;
    }

    const std::string appLower = StringUtils::toLower(std::string(appKey));
    for (const auto& entry : allEntries) {
      if (!options.includeHidden && entry.hidden) {
        continue;
      }
      if (!options.includeNoDisplay && entry.noDisplay) {
        continue;
      }
      if (desktopEntryMatchesLower(entry, appLower)) {
        return entry;
      }
    }

    if (!appKey.starts_with("steam_app_")) {
      return std::nullopt;
    }

    const std::string_view steamId = appKey.substr(std::string_view("steam_app_").size());
    if (steamId.empty()) {
      return std::nullopt;
    }
    const std::string runGameToken = std::string("rungameid/") + std::string(steamId);

    for (const auto& entry : allEntries) {
      if (!options.includeHidden && entry.hidden) {
        continue;
      }
      if (!options.includeNoDisplay && entry.noDisplay) {
        continue;
      }
      if (StringUtils::toLower(entry.startupWmClass) == appLower) {
        return entry;
      }
      if (entry.exec.find(runGameToken) != std::string::npos) {
        return entry;
      }
    }

    return std::nullopt;
  }

  std::string resolveIconThemeNameForAppKey(std::string_view appKey, const std::vector<DesktopEntry>& allEntries) {
    if (appKey.empty()) {
      return {};
    }

    if (const auto entry = findDesktopEntry(appKey, allEntries)) {
      if (!entry->icon.empty()) {
        return entry->icon;
      }
    }

    const std::string appLower = StringUtils::toLower(std::string(appKey));
    for (const auto& entry : allEntries) {
      if (entry.icon.empty() || entry.hidden || entry.noDisplay) {
        continue;
      }
      if (desktopEntryMatchesLower(entry, appLower)) {
        return entry.icon;
      }
    }

    for (const auto& entry : allEntries) {
      if (entry.icon.empty()) {
        continue;
      }
      const std::string stem = desktopEntryStemLower(entry.id);
      if (stem == appLower) {
        return entry.icon;
      }
      const auto dash = stem.rfind('-');
      if (dash != std::string::npos) {
        const std::string_view suffix = std::string_view(stem).substr(dash + 1);
        if (suffix == "bin" || suffix == "desktop") {
          if (stem.substr(0, dash) == appLower) {
            return entry.icon;
          }
        }
      }
    }

    return std::string(appKey);
  }

  namespace {

    DesktopEntryResolution
    resolveRunningDesktopEntryWithStatus(std::string_view runningAppId, const std::vector<DesktopEntry>& allEntries) {
      const std::string runningLower = StringUtils::toLower(std::string(runningAppId));

      for (const auto& entry : allEntries) {
        if (entry.hidden || entry.noDisplay) {
          continue;
        }
        if (desktopEntryMatchesLower(entry, runningLower)) {
          return DesktopEntryResolution{
              .entry = entry,
              .matchedDesktopEntry = true,
          };
        }
      }

      DesktopEntryLookupOptions extendedLookup;
      if (runningAppId.starts_with("steam_app_")) {
        extendedLookup.includeHidden = true;
        extendedLookup.includeNoDisplay = true;
      }
      if (auto matched = findDesktopEntry(runningAppId, allEntries, extendedLookup)) {
        if (runningAppId.starts_with("steam_app_") && matched->startupWmClass.empty()) {
          matched->startupWmClass = std::string(runningAppId);
        }
        return DesktopEntryResolution{
            .entry = std::move(*matched),
            .matchedDesktopEntry = true,
        };
      }

      DesktopEntry fallback;
      fallback.id = std::string(runningAppId);
      fallback.name = std::string(runningAppId);
      fallback.nameLower = runningLower;
      internal_apps::applyMetadataToDesktopEntry(fallback);
      fallback.icon = resolveIconThemeNameForAppKey(runningAppId, allEntries);

      return DesktopEntryResolution{
          .entry = fallback,
          .matchedDesktopEntry = false,
      };
    }

  } // namespace

  DesktopEntry resolveRunningDesktopEntry(std::string_view runningAppId, const std::vector<DesktopEntry>& allEntries) {
    return resolveRunningDesktopEntryWithStatus(runningAppId, allEntries).entry;
  }

  std::vector<ResolvedRunningApp>
  resolveRunningApps(const std::vector<std::string>& runningAppIds, const std::vector<DesktopEntry>& allEntries) {
    std::vector<ResolvedRunningApp> resolved;
    resolved.reserve(runningAppIds.size());

    std::unordered_set<std::string> seen;
    seen.reserve(runningAppIds.size());

    for (const auto& runningAppId : runningAppIds) {
      const std::string runningLower = StringUtils::toLower(runningAppId);
      const auto resolution = resolveRunningDesktopEntryWithStatus(runningAppId, allEntries);
      std::string dedupeKey = resolution.matchedDesktopEntry ? StringUtils::toLower(resolution.entry.id) : runningLower;
      if (dedupeKey.empty()) {
        dedupeKey = runningLower;
      }
      if (!seen.insert(dedupeKey).second) {
        continue;
      }

      resolved.push_back(
          ResolvedRunningApp{
              .runningAppId = runningAppId,
              .runningLower = runningLower,
              .entry = resolution.entry,
          }
      );
    }

    return resolved;
  }

} // namespace app_identity
