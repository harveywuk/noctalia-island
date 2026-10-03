#pragma once

#include "launcher/alias_store.h"
#include "launcher/launcher_provider.h"
#include "launcher/usage_tracker.h"
#include "shell/panel/panel.h"
#include "system/icon_resolver.h"
#include "ui/signal.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ContextMenuPopup;
class Flex;
class Glyph;
class Image;
class Input;
class Label;
class LauncherResultAdapter;
class LauncherAppGridAdapter;
class Renderer;
class Segmented;
class Separator;
class ScrollView;
class VirtualGridView;
class ConfigService;
class AsyncTextureCache;

class LauncherPanel : public Panel {
public:
  LauncherPanel(ConfigService* config, AsyncTextureCache* asyncTextures);
  ~LauncherPanel() override;

  void addProvider(std::unique_ptr<LauncherProvider> provider);
  // Drop every dynamically-registered (plugin-backed) provider, so the enabled
  // plugin set can be re-applied without disturbing the built-in providers.
  void clearDynamicProviders();
  // Drop providers whose stable id starts with `prefix` (e.g. config-driven "dmenu.").
  void clearProvidersWithIdPrefix(std::string_view prefix);
  // Restrict the next open to a single provider (stdin/dmenu session). When set,
  // onInputChanged queries only that provider and skips prefix routing/overview.
  // Cleared on close.
  void setScopedProvider(std::string_view providerId, std::string_view placeholder = {});

  void create() override;
  void onOpen(std::string_view context) override;
  // True while the field still holds `context`, so toggling the same prefix (the Clipboard
  // shortcut's "/clip") closes the launcher instead of reopening it.
  [[nodiscard]] bool isContextActive(std::string_view context) const override;
  void onClose() override;
  void onIconThemeChanged() override;

  // Runs a result without opening the launcher: an alias, or "<provider>:<result id>"
  // (`noctalia msg launcher-run`). Returns an IPC reply line.
  std::string runFromSpec(std::string_view spec);

  void clearUsage();
  void syncUsageTrackingState();

  // Invoked after a terminal close when the activation copied text and the provider
  // supports auto-paste. The host schedules virtual-keyboard paste (clipboard path).
  // Called after a copy-style activation, with the provider that copied, so auto-paste can follow it.
  void setCopiedActivationCallback(std::function<void(const LauncherProvider&)> callback) {
    m_onCopiedActivation = std::move(callback);
  }
  // Copies text for launcher-level actions ("Copy Hotkey Command").
  void setCopyTextCallback(std::function<void(std::string)> callback) { m_copyText = std::move(callback); }

  [[nodiscard]] float preferredWidth() const override { return scaled(560.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(500.0F); }
  [[nodiscard]] float fittedHeight() const override;
  [[nodiscard]] float islandWidth(float availableWidth) const override;
  [[nodiscard]] float islandHeight(float availableHeight) const override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  enum ActiveCategoryType { All, RecentlyUsed, Category };

  struct CategoryFilterSlot {
    ActiveCategoryType type;
    std::size_t categoryIndex = 0;
  };

  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void onInputChanged(const std::string& text);
  void setQuery(std::string query);
  // Re-gather the current query, preserving the selected result by identity.
  void reapplyCurrentQuery();
  // A plugin provider delivered fresh async results — re-gather if the panel is open.
  void onProviderResultsChanged();
  void refreshResults();
  void activateAt(std::size_t index);
  void activateSelected();
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void applyEmptyState();
  [[nodiscard]] std::unique_ptr<Node> buildFooter(float scale);
  void syncFooter();
  void assignSections();
  [[nodiscard]] std::string sectionTitleFor(std::string_view providerId) const;
  [[nodiscard]] std::string kindFor(const LauncherResult& result) const;
  void bindDetailResult();
  [[nodiscard]] bool shouldUseDetailPresentation() const;
  [[nodiscard]] bool startsWithLauncherPrefix(std::string_view text) const;
  void applyProviderConfig(LauncherProvider& provider) const;
  void finishActivation(LauncherProvider& provider, const std::string& resultId, bool copied);
  [[nodiscard]] std::vector<LauncherResult> providerOverviewResults(std::string_view text) const;
  // Raycast's Suggestions: recently run results from any provider, shown when nothing is typed.
  void insertSuggestions();
  // Fallback rows for a typed query ("Search Files for …"), from shell.launcher.fallbacks.
  [[nodiscard]] std::vector<LauncherResult> providerFallbackResults(std::string_view query) const;
  // Tab: puts the selected result's completion (a prefix, a keyword) in the field.
  bool completeSelected();
  // Esc inside a provider view reached from the root search goes back to it; returns false to close.
  bool popToRoot();
  void recordActivation(const LauncherProvider& provider, const std::string& resultId);
  [[nodiscard]] bool openActionsMenu(std::size_t index, float anchorX, float anchorY);
  [[nodiscard]] bool openSelectedActionsMenu();
  // Runs the first extra action of the selected result (Ctrl+Return, Raycast's secondary action).
  bool runSecondaryAction();
  void runProviderAction(const LauncherResult& result, std::string_view actionId);
  [[nodiscard]] LauncherProvider* providerFor(std::string_view providerId) const;
  [[nodiscard]] std::string primaryActionLabelFor(const LauncherResult& result) const;
  [[nodiscard]] bool hasActions(const LauncherResult& result) const;
  [[nodiscard]] std::optional<LauncherResult> resolveAliasTarget(const AliasStore::Target& target) const;
  [[nodiscard]] static std::string specFor(const LauncherResult& result);
  [[nodiscard]] std::string aliasForResult(const LauncherResult& result);
  void applyAliases(std::string_view queryText);
  void beginAliasEdit(const LauncherResult& result);
  void endAliasEdit();
  void beginForm(LauncherForm form, LauncherProvider* provider);
  void endForm(bool saved);
  void showFormField(std::size_t index);
  void submitForm();
  void buildFormRows(const std::string& text);
  [[nodiscard]] std::unique_ptr<Node> buildPreviewPane(float scale);
  void syncPreview();
  void rebuildCategoryFilter(const std::vector<LauncherCategory>& categories);
  void setCategoryFilterVisible(bool visible);
  void setActiveCategorySlot(std::size_t slotIndex);
  void applyActiveCategory();
  void syncLauncherListStyle();
  void syncLauncherViewLayout(Renderer* renderer = nullptr);
  [[nodiscard]] bool shouldUseAppGrid() const;
  void refreshLauncherAppIconColorization();
  void updateLauncherGridMetrics(Renderer& renderer);
  void updatePinnedApplicationState();
  void applyPinnedApplicationOrder();
  void reorderPinnedApplication(std::string_view sourcePath, std::string_view targetPath);
  [[nodiscard]] bool shouldTrackUsage() const;

  std::vector<std::unique_ptr<LauncherProvider>> m_providers;
  AliasStore m_aliases;
  // Set while the field is taking an alias for this result ("Set Alias…").
  std::optional<LauncherResult> m_aliasTarget;
  // Create/Edit forms: the search field edits m_form->fields[m_formField].
  std::optional<LauncherForm> m_form;
  LauncherProvider* m_formProvider = nullptr;
  std::size_t m_formField = 0;
  std::string m_formError;
  std::string m_formReturnQuery;
  // The prefixed provider currently shown, when it asks for a preview pane.
  LauncherProvider* m_previewProvider = nullptr;
  std::vector<LauncherResult> m_results;
  std::vector<LauncherResult> m_allResults;
  UsageTracker m_usageTracker;
  IconResolver m_iconResolver;

  Flex* m_container = nullptr;
  Input* m_input = nullptr;
  Segmented* m_categoryFilter = nullptr;
  Flex* m_body = nullptr;
  Flex* m_listColumn = nullptr;
  Flex* m_previewPane = nullptr;
  Separator* m_previewDivider = nullptr;
  Image* m_previewImage = nullptr;
  Label* m_previewBadge = nullptr;
  Label* m_previewTitle = nullptr;
  Label* m_previewBody = nullptr;
  Separator* m_previewMetaDivider = nullptr;
  std::vector<Flex*> m_previewMetaRows;
  std::vector<Label*> m_previewMetaLabels;
  std::vector<Label*> m_previewMetaValues;
  std::string m_previewKey;
  std::string m_pendingPreviewImagePath;
  std::vector<std::uint8_t> m_pendingPreviewImageBytes;
  bool m_previewImageDirty = false;
  VirtualGridView* m_grid = nullptr;
  ScrollView* m_detailScroll = nullptr;
  Label* m_detailSubtitle = nullptr;
  Label* m_detailBody = nullptr;
  Label* m_emptyLabel = nullptr;
  Flex* m_footer = nullptr;
  Label* m_footerKind = nullptr;
  Label* m_footerPrimary = nullptr;
  Flex* m_footerActions = nullptr;
  Separator* m_footerActionsSeparator = nullptr;
  // True when the results mix sources (no prefix or scope), so they are grouped into sections.
  bool m_mixedResults = false;
  bool m_anyProviderLoading = false;
  std::unique_ptr<LauncherResultAdapter> m_listAdapter;
  std::unique_ptr<LauncherAppGridAdapter> m_gridAdapter;

  std::string m_query;
  // What the launcher opened with (a shortcut's "/clip"); Esc closes rather than pops back from it.
  std::string m_openContext;
  std::string m_scopedProviderId;
  std::string m_scopedPlaceholder;
  ActiveCategoryType m_activeCategoryType = All;
  std::string m_activeCategory;
  std::vector<LauncherCategory> m_currentCategories;
  std::vector<CategoryFilterSlot> m_categoryFilterSlots;
  bool m_hasRecentlyUsed = false;
  std::size_t m_selectedIndex = 0;
  bool m_categoryFilterVisible = true;
  bool m_launcherShowIcons = true;
  bool m_launcherShowAppOriginIndicator = true;
  bool m_launcherCompact = false;
  bool m_launcherAppGrid = false;
  bool m_usingAppGrid = false;
  float m_launcherRowHeight = 0.0F;
  std::uint64_t m_desktopEntriesVersion = 0;
  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::unique_ptr<ContextMenuPopup> m_actionsMenu;
  Signal<>::ScopedConnection m_appIconColorizeConn;
  std::function<void(const LauncherProvider&)> m_onCopiedActivation;
  std::function<void(std::string)> m_copyText;
};
