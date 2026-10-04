#pragma once

#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;
class ConfigService;
class HttpClient;

// Define Word in the launcher (Raycast's dictionary): "define serendipity" fetches the word's
// meanings from the free dictionaryapi.dev service and lists them, part of speech first. Return
// copies the definition. Nothing is fetched in offline mode; lookups wait for a short pause in
// typing so a word in progress isn't sent letter by letter.
class DefineProvider : public LauncherProvider {
public:
  struct Meaning {
    std::string partOfSpeech;
    std::string definition;
    std::string example;
  };
  struct Entry {
    std::string word;
    std::string phonetic;
    std::vector<Meaning> meanings;
  };

  DefineProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "define"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Dictionary"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "book"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }

  void setResultsChangedCallback(std::function<void()> callback) override { m_onChanged = std::move(callback); }
  [[nodiscard]] bool isLoading() const override;
  void reset() override;

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;

  // "define serendipity" / "def x" → the word; empty when the text isn't a lookup. Exposed for tests.
  [[nodiscard]] static std::string wordFor(std::string_view text, bool prefixed);
  // Parses dictionaryapi.dev's response; exposed for tests.
  [[nodiscard]] static std::optional<Entry> parse(std::string_view json);
  // The lookup URL for a word.
  [[nodiscard]] static std::string urlFor(std::string_view word);

private:
  [[nodiscard]] std::vector<LauncherResult> resultsFor(std::string_view word) const;
  // Takes the word by value: the caller passes m_pendingWord, which the lookup clears.
  void lookup(std::string word) const;

  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  HttpClient* m_httpClient = nullptr;
  std::function<void()> m_onChanged;
  mutable Timer m_debounce;
  mutable std::string m_pendingWord;
  mutable std::string m_loadingWord;
  mutable std::string m_loadedWord;
  mutable std::optional<Entry> m_entry;
  mutable bool m_notFound = false;
  mutable bool m_failed = false;
};
