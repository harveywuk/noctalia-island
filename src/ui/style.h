#pragma once

#include "ui/signal.h"

namespace Style {

  inline constexpr int barThicknessDefault = 34;

  inline constexpr int animFast = 100;
  inline constexpr int animNormal = 200;
  inline constexpr int animSlow = 400;

  inline constexpr float radiusSm = 5.0F;
  inline constexpr float radiusMd = 8.0F;
  inline constexpr float radiusLg = 12.0F;
  inline constexpr float radiusXl = 18.0F;

  inline constexpr float borderWidth = 1.0F;
  inline constexpr float emphasizedBorderWidth = 3.0F;
  inline constexpr float focusRingWidth = 2.0F;
  // Quiet chrome keeps accent colours for selection, focus and active controls.
  inline constexpr float hairlineAlpha = 0.32F;
  inline constexpr float controlBorderAlpha = 0.5F;
  inline constexpr float hoverFillAlpha = 0.08F;
  inline constexpr float pressedFillAlpha = 0.14F;
  inline constexpr float disabledOutlineAlpha = 0.2F;

  inline constexpr float spaceXs = 4.0F;
  inline constexpr float spaceSm = 8.0F;
  inline constexpr float spaceMd = 12.0F;
  inline constexpr float spaceLg = 16.0F;

  inline constexpr float cardPadding = 14.0F;
  inline constexpr float panelPadding = 14.0F;

  // Default inner inset for bar widget capsules (logical px, before bar content scale).
  inline constexpr float barCapsulePadding = 6.0F;
  inline constexpr float baseGlyphSize = 16.0F;

  inline constexpr float fontSizeMini = 11.0F;
  inline constexpr float fontSizeCaption = 13.0F;
  inline constexpr float fontSizeBody = 14.0F;
  inline constexpr float fontSizeTitle = 16.0F;
  inline constexpr float fontSizeHeader = 20.0F;

  // Closer to macOS control density: sidebar rows and small buttons 28, push buttons, pop-up
  // buttons and fields 32, prominent actions 40.
  inline constexpr float controlHeightSm = 28.0F;
  inline constexpr float controlHeight = 32.0F;
  inline constexpr float controlHeightLg = 40.0F;
  inline constexpr float scrollWheelStep = 56.0F;
  // Pointer distance in logical px before an armed drag becomes active.
  inline constexpr float dragStartThreshold = 6.0F;

  // Base scrollbar thickness at rest, and while the pointer is over it. The bar overlays the
  // content as it expands, so hovering never reflows the scroll view.
  inline constexpr float scrollbarWidth = 6.0F;
  inline constexpr float scrollbarHoverWidth = 12.0F;
  inline constexpr float scrollbarGap = spaceSm;
  // Shortest the thumb gets on a long document; it must stay a usable drag target and read as a
  // bar rather than a lozenge next to the hovered thickness.
  inline constexpr float scrollbarMinThumbHeight = 32.0F;
  // Pointer margin on the content side of the bar, so a thin bar stays an easy grab target.
  inline constexpr float scrollbarHitSlop = 6.0F;

  // Growth cap (logical px, before content scale) for menus/dropdowns that size to their content.
  inline constexpr float menuAutoMaxWidth = 420.0F;

  // Window switcher carousel geometry and depth treatment.
  inline constexpr float windowSwitcherDimOpacity = 0.3F;
  inline constexpr float windowSwitcherSelectedCardWidth = 500.0F;
  inline constexpr float windowSwitcherNearCardWidth = 380.0F;
  inline constexpr float windowSwitcherFarCardWidth = 280.0F;
  inline constexpr float windowSwitcherCompactCardWidth = 220.0F;
  inline constexpr float windowSwitcherPreviewAspect = 1.6F;
  inline constexpr float windowSwitcherCardOverlap = 44.0F + spaceLg;
  inline constexpr float windowSwitcherNarrowLayoutThreshold = 0.72F;
  inline constexpr float windowSwitcherPreviewIconScale = 0.12F;
  inline constexpr float windowSwitcherFallbackIconScale = 0.28F;
  inline constexpr float windowSwitcherCaptionHeight = 44.0F;
  inline constexpr float windowSwitcherCaptionLineGap = -spaceXs;
  inline constexpr float windowSwitcherIncomingCardScale = 0.9F;
  inline constexpr float windowSwitcherIncomingCardSlide = 0.25F;
  inline constexpr float windowSwitcherOutgoingCardSlide = 0.3F;
  inline constexpr float windowSwitcherRevealScale = 0.95F;

  // Toggle preset geometry. Track height = thumb + 2 * inset; track width = thumb + 2 * inset + travel.
  // Matches the macOS switch sizes: mini 26x15, small 32x18, regular 38x22, with the knob nearly
  // filling the track.
  inline constexpr float toggleThumbSizeSm = 13.0F;
  inline constexpr float toggleInsetSm = 1.0F;
  inline constexpr float toggleTravelSm = 11.0F;
  inline constexpr float toggleThumbSizeMd = 16.0F;
  inline constexpr float toggleInsetMd = 1.0F;
  inline constexpr float toggleTravelMd = 14.0F;
  inline constexpr float toggleThumbSizeLg = 20.0F;
  inline constexpr float toggleInsetLg = 1.0F;
  inline constexpr float toggleTravelLg = 16.0F;

  // Slider geometry: a thin macOS track with a round white knob.
  inline constexpr float sliderDefaultWidth = 180.0F;
  inline constexpr float sliderTrackHeight = 4.0F;
  inline constexpr float sliderThumbSize = 20.0F;
  inline constexpr float sliderHorizontalPadding = 2.0F;

  // Checkbox box and radio circle, as on macOS: 14px at body text size, scaled up a touch for the
  // shell's larger default text. The checked radio shows a small centred dot.
  inline constexpr float checkboxSize = 16.0F;
  inline constexpr float checkboxRadius = 4.0F;
  inline constexpr float radioDotSize = 6.0F;

  // Soft drop shadow under white knobs (switch thumbs, slider knobs), which macOS uses instead of
  // an outline to lift the knob off the track.
  inline constexpr float knobShadowAlpha = 0.4F;
  inline constexpr float knobShadowSoftness = 2.0F;
  inline constexpr float knobShadowOffsetY = 1.0F;
  // A faint edge keeps a white knob crisp against a light track.
  inline constexpr float knobEdgeAlpha = 0.08F;
  inline constexpr float knobEdgeWidth = 0.5F;

  // Dark-mode push button bezel over the panel surface, and the brighter pressed bezel.
  inline constexpr float pushButtonFillAlpha = 0.24F;
  inline constexpr float pushButtonPressedFillAlpha = 0.34F;
  // Filled (accent, destructive) buttons dim slightly while pressed.
  inline constexpr float filledButtonPressedAlpha = 0.82F;
  // Selected segment of a segmented control in dark mode; light mode uses a white segment.
  inline constexpr float segmentSelectedFillAlpha = 0.24F;
  // Light-mode track behind a segmented control, so the white selected segment reads as raised.
  inline constexpr float segmentTrackLightAlpha = 0.07F;

  [[nodiscard]] float cornerRadiusScale() noexcept;
  void setCornerRadiusScale(float scale) noexcept;

  [[nodiscard]] bool buttonBordersEnabled() noexcept;
  void setButtonBordersEnabled(bool enabled);
  Signal<>& buttonBordersChanged();

  [[nodiscard]] bool inputBordersEnabled() noexcept;
  void setInputBordersEnabled(bool enabled);
  Signal<>& inputBordersChanged();

  [[nodiscard]] bool popupBordersEnabled() noexcept;
  void setPopupBordersEnabled(bool enabled);

  [[nodiscard]] bool rtl() noexcept;
  void setRtl(bool rtl) noexcept;

  [[nodiscard]] bool cardBordersEnabled() noexcept;
  void setCardBordersEnabled(bool enabled);

  [[nodiscard]] bool popupShadowsEnabled() noexcept;
  void setPopupShadowsEnabled(bool enabled);

  [[nodiscard]] float scaledRadius(float radius, float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusSm(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusMd(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusLg(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusXl(float localScale = 1.0F) noexcept;

} // namespace Style
