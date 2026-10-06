#pragma once

#include "render/animation/animation_manager.h"
#include "render/core/texture_handle.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "shell/desktop/desktop_widget_gallery.h"
#include "shell/desktop/desktop_widget_layouts.h"
#include "shell/desktop/editor/desktop_widgets_editor_types.h"
#include "shell/desktop/editor/desktop_widgets_history.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/layer_surface.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Box;
class Button;
class ConfigService;
class SharedTextureCache;
class WallpaperNode;
class InputArea;
class Input;
class Label;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct WaylandOutput;
struct wl_output;
struct wl_surface;

class DesktopWidgetsEditor {
public:
  explicit DesktopWidgetsEditor(DesktopWidgetsEditorProfile profile);

  void initialize(const DesktopWidgetServices& services);
  void setExitRequestedCallback(std::function<void()> callback);

  void open(const DesktopWidgetsEditorSnapshot& snapshot, const DesktopWidgetsHistory* history = nullptr);
  [[nodiscard]] const DesktopWidgetsHistory& history() const { return m_history; }
  void editWidget(const std::string& id);
  [[nodiscard]] const DesktopWidgetsEditorSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] DesktopWidgetsEditorSnapshot close();
  [[nodiscard]] bool isOpen() const noexcept;

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackPopupParentContext() const;
  void onOutputChange();
  void onSecondTick();
  void requestUpdate();
  void requestLayout();
  void requestRedraw();

  void applySettingChange(const std::string& key, WidgetSettingValue value, bool rebuild = false);
  void configureWidget(const std::string& id);
  [[nodiscard]] std::string settingEditTarget() const {
    return std::to_string(m_setupRevision) + ":" + (m_setupDraft ? m_setupWidgetId : m_selectedWidgetId);
  }
  void resetSelectedWidgetSettings();

private:
  enum class ScaleCorner : std::uint8_t {
    TopLeft = 0,
    TopRight,
    BottomLeft,
    BottomRight,
  };

  enum class DragMode : std::uint8_t {
    None,
    Move,
    Scale,
    Rotate,
    Lasso,
    ToolbarMove,
    InspectorMove,
    StackMember,
    Gallery,
  };

  struct EditorWidgetView {
    std::unique_ptr<DesktopWidget> widget;
    Node* transformNode = nullptr;
    Node* liftNode = nullptr;
    InputArea* bodyArea = nullptr;
    float intrinsicWidth = 0.0F;
    float intrinsicHeight = 0.0F;
  };

  struct SecondarySelectionVisual {
    std::string widgetId;
    Node* transform = nullptr;
    Box* borderShadow = nullptr;
    Box* border = nullptr;
  };

  struct GroupMemberInitial {
    DesktopWidgetState state;
    float intrinsicWidth = 1.0F;
    float intrinsicHeight = 1.0F;
  };

  struct OverlaySurface {
    std::string outputName;
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    std::unique_ptr<Node> sceneRoot;
    bool sceneRebuildRequested = true;
    std::unordered_map<std::string, EditorWidgetView> views;
    std::vector<SecondarySelectionVisual> secondarySelections;
    Node* selectionFrameTransform = nullptr;
    Node* selectionBorderTransform = nullptr;
    Box* selectionBorder = nullptr;
    Box* selectionBorderShadow = nullptr;
    Box* rotationRing = nullptr;
    Box* rotationRingShadow = nullptr;
    InputArea* rotateArea = nullptr;
    std::array<Box*, 4> scaleHandles{};
    std::array<Box*, 4> scaleHandleShadows{};
    std::array<InputArea*, 4> scaleAreas{};
    Box* lassoBox = nullptr;
    std::unique_ptr<DesktopWidget> galleryPreview;
    std::unique_ptr<DesktopWidget> galleryDragWidget;
    Node* galleryDragNode = nullptr;
    Input* gallerySearch = nullptr;
    Node* galleryOverlay = nullptr;
    Box* snapGuideX = nullptr;
    Box* snapGuideY = nullptr;
    Box* stackDropPreview = nullptr;
    Label* stackDropLabel = nullptr;
    std::vector<std::pair<std::string, Node*>> stackMemberRows;
    Node* historyToolbar = nullptr;
    Button* undoButton = nullptr;
    Button* redoButton = nullptr;
    Node* toolbar = nullptr;
    float toolbarX = 0.0F;
    float toolbarY = 0.0F;
    bool toolbarPositionInitialized = false;
    Node* inspector = nullptr;
    float inspectorX = 0.0F;
    float inspectorY = 0.0F;
    bool inspectorPositionInitialized = false;
    std::unique_ptr<SelectDropdownPopup> selectPopup;
    bool pointerInside = false;
    bool wallpaperPreviewActive = false;
    std::string wallpaperPreviewPath;
    std::string wallpaperPreviewLoadedPath;
    TextureHandle wallpaperPreviewTexture;
    WallpaperNode* wallpaperPreview = nullptr;
  };

  struct DragState {
    DragMode mode = DragMode::None;
    std::string widgetId;
    float startSceneX = 0.0F;
    float startSceneY = 0.0F;
    DesktopWidgetState initialState;
    float intrinsicWidth = 0.0F;
    float intrinsicHeight = 0.0F;
    ScaleCorner scaleCorner = ScaleCorner::BottomRight;
    std::string surfaceOutputName;
    std::string moveSourceOutputName;
    float movePointerOffsetX = 0.0F;
    float movePointerOffsetY = 0.0F;
    float initialToolbarX = 0.0F;
    float initialToolbarY = 0.0F;
    float initialInspectorX = 0.0F;
    float initialInspectorY = 0.0F;
    bool rebuildOnFinish = false;
    bool lassoAdditive = false;
    std::string stackTargetId;
    std::string sourceStackId;
    std::optional<std::size_t> stackInsertion;
    bool memberDropOutside = false;
    bool moved = false;
    float dropX = 0.0F;
    float dropY = 0.0F;
    std::unordered_map<std::string, GroupMemberInitial> groupInitialStates;
  };

  void syncSurfaces();
  void createSurface(const WaylandOutput& output);
  void rebuildScene(OverlaySurface& surface);
  void prepareFrame(OverlaySurface& surface, bool needsUpdate, bool needsLayout);
  void releaseWallpaperPreview(OverlaySurface& surface);
  void updateWallpaperPreview(OverlaySurface& surface);
  void applyViewState(EditorWidgetView& view, const DesktopWidgetState& state, bool refreshContent);
  void updateViewTransforms(const std::string* relayoutWidgetId = nullptr);
  // Live resize preview: grow the box (handles/outline) and scale the content on the GPU instead
  // of re-laying out the dragged widget every pointer move. finishDrag() does the crisp re-fit.
  void applyScaleDragPreview(const DesktopWidgetState& state);
  void updateSelectionVisuals(OverlaySurface& surface);
  void addWidget(
      const std::string& outputName, const std::string& type, const std::string& cardSize = {},
      std::optional<std::pair<float, float>> position = std::nullopt
  );
  void openGallery(const std::string& output);
  void buildGallery(OverlaySurface& surface, Node& root, std::unique_ptr<Node> search);
  void closeGallery(bool animated = false);
  void toggleGalleryFavorite(const std::string& type);
  void addGallerySelection(const std::string& output);
  void startGalleryDrag(const std::string& output);
  void updateGalleryDrag();
  void finishGalleryDrag();
  void cancelGalleryDrag();
  void startSetup(const std::string& output, const std::string& type, const std::string& size);
  void buildSetup(OverlaySurface& surface, Node& root);
  void closeSetup();
  void saveSetup(bool skip = false);
  void stackSelection();
  void unstackSelection();
  void buildStackDropPreview(OverlaySurface& surface, Node& root);
  void hideStackDropPreviews();
  void updateStackTarget(OverlaySurface& surface, float pointerX, float pointerY);
  void startStackMemberDrag(const std::string& stack, const std::string& member, const std::string& output);
  void updateStackMemberDrag();
  void finishStackDrop();
  void cancelStackDrag();
  void hideSnapGuides();
  void removeSelectedWidget();
  void toggleSelectedWidgetEnabled();
  void sendSelectedWidgetToBack();
  void bringSelectedWidgetToFront();
  void flipSelectedWidgetHorizontal();
  void flipSelectedWidgetVertical();
  void cloneSelectedWidgets();
  void copySelectedWidgets();
  void pasteWidgets();
  void startToolbarDrag(const std::string& outputName);
  void startInspectorDrag(const std::string& outputName);
  void clampToolbarPosition(OverlaySurface& surface, float toolbarWidth, float toolbarHeight);
  void clampInspectorPosition(OverlaySurface& surface, float inspectorWidth, float inspectorHeight);
  void buildInspector(OverlaySurface& surface, Node& root, const DesktopWidgetState& selectedState);
  void deferEditorMutation(std::function<void()> action, std::string historyGroup = {});
  void recordHistory(const std::string& group = {});
  void travelHistory(bool redo);
  void buildHistoryToolbar(OverlaySurface& surface, Node& root);
  void positionHistoryToolbar(OverlaySurface& surface);
  void loadLayouts();
  void openLayouts(const std::string& output);
  void closeLayouts();
  void buildLayouts(OverlaySurface& surface, Node& root);
  void saveLayout(std::optional<std::size_t> replace = std::nullopt);
  void applyLayout(std::size_t index);
  void deleteLayout(std::size_t index);
  bool storeLayouts(std::vector<desktop_layouts::Layout> layouts);
  void requestExit();
  void startDrag(
      DragMode mode, const std::string& widgetId, bool rebuildOnFinish,
      ScaleCorner scaleCorner = ScaleCorner::BottomRight
  );
  void startLassoDrag(const std::string& outputName);
  void finishLassoSelection();
  void populateGroupInitialStates(const std::string& anchorWidgetId);
  void updateDrag();
  void finishDrag();
  void updateLassoVisual(OverlaySurface& surface);
  [[nodiscard]] OverlaySurface* findSurface(wl_surface* surface);
  [[nodiscard]] const OverlaySurface* findSurface(wl_surface* surface) const;
  [[nodiscard]] std::optional<LayerPopupParentContext> overlayPopupParentContext(const OverlaySurface& surface) const;
  [[nodiscard]] OverlaySurface* findSurface(const std::string& outputName);
  [[nodiscard]] OverlaySurface* findSurfaceForWidget(const std::string& widgetId);
  [[nodiscard]] EditorWidgetView* findView(const std::string& id);
  [[nodiscard]] DesktopWidgetState* findWidgetState(const std::string& id);
  [[nodiscard]] const DesktopWidgetState* findWidgetState(const std::string& id) const;
  [[nodiscard]] std::string effectiveOutputName(const DesktopWidgetState& state) const;
  [[nodiscard]] bool shouldSnap() const;
  [[nodiscard]] float widgetContentScale() const;
  [[nodiscard]] std::string nextWidgetId() const;
  [[nodiscard]] float duplicateOffset() const;
  [[nodiscard]] std::vector<DesktopWidgetState> selectedWidgetTemplates() const;
  std::vector<std::string> insertWidgetCopies(
      const std::vector<DesktopWidgetState>& templates, float offsetX, float offsetY, bool selectInserted,
      const std::string& targetOutputName = {}
  );
  [[nodiscard]] std::string currentPointerOutputName() const;
  [[nodiscard]] bool isWidgetSelected(const std::string& id) const;
  void clearSelection();
  void setSingleSelection(const std::string& id);
  void handleWidgetPress(const std::string& id);

  DesktopWidgetsEditorProfile m_profile;
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  std::unique_ptr<DesktopWidgetFactory> m_factory;
  std::string m_galleryOutputName;
  std::string m_galleryWidgetType = "weather";
  std::string m_galleryCardSize = "small";
  std::string m_galleryQuery;
  desktop_gallery::Category m_galleryCategory = desktop_gallery::Category::All;
  std::unordered_set<std::string> m_galleryFavorites;
  bool m_galleryFocusSearch = false;
  bool m_gallerySaveFailed = false;
  bool m_galleryRevealPending = false;
  bool m_galleryClosing = false;
  ScrollViewState m_galleryScroll;
  std::uint64_t m_setupRevision = 0;
  std::optional<DesktopWidgetState> m_setupDraft;
  std::string m_setupWidgetId;
  std::string m_setupOutputName;
  std::string m_setupError;
  ScrollViewState m_setupScroll;
  std::optional<std::pair<float, float>> m_setupPosition;
  std::string m_setupStackTarget;
  std::function<void()> m_exitRequestedCallback;
  DesktopWidgetsEditorSnapshot m_snapshot;
  DesktopWidgetsHistory m_history;
  std::vector<desktop_layouts::Layout> m_layouts;
  bool m_layoutsReadable = true;
  std::string m_layoutOutput;
  std::string m_layoutName;
  std::string m_layoutError;
  bool m_layoutMonitorOnly = false;
  ScrollViewState m_layoutScroll;
  std::vector<std::unique_ptr<OverlaySurface>> m_surfaces;
  std::string m_selectedWidgetId;
  std::unordered_set<std::string> m_selectedWidgetIds;
  std::vector<DesktopWidgetState> m_widgetClipboard;
  std::size_t m_pasteCount = 0;
  DragState m_drag;
  bool m_open = false;
  bool m_shiftHeld = false;
  bool m_leftShiftHeld = false;
  bool m_rightShiftHeld = false;
  bool m_ctrlHeld = false;
  bool m_leftCtrlHeld = false;
  bool m_rightCtrlHeld = false;
  bool m_altHeld = false;
  bool m_leftAltHeld = false;
  bool m_rightAltHeld = false;
  float m_currentEventSceneX = 0.0F;
  float m_currentEventSceneY = 0.0F;
  std::string m_currentEventOutputName;
  bool m_inspectorOpen = false;
};
