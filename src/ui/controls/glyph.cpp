#include "ui/controls/glyph.h"

#include "render/core/renderer.h"
#include "render/scene/glyph_node.h"
#include "render/text/glyph_registry.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <memory>

Glyph::Glyph() {
  auto glyph = std::make_unique<GlyphNode>();
  m_glyphNode = static_cast<GlyphNode*>(addChild(std::move(glyph)));
  m_logicalFontSize = Style::fontSizeBody;
  m_glyphNode->setFontSize(m_logicalFontSize);
  applyPalette();
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
}

bool Glyph::setGlyph(std::string_view name) { return setCodepoint(GlyphRegistry::lookup(name)); }

bool Glyph::setCodepoint(char32_t codepoint) {
  m_baseCodepoint = codepoint;
  return applyCodepoint();
}

void Glyph::setEmphasized(bool emphasized) {
  if (m_emphasized == emphasized)
    return;
  m_emphasized = emphasized;
  applyCodepoint();
}

bool Glyph::applyCodepoint() {
  const auto codepoint = m_emphasized ? GlyphRegistry::emphasized(m_baseCodepoint) : m_baseCodepoint;
  if (codepoint == m_glyphNode->codepoint())
    return false;
  const auto optical = GlyphRegistry::opticalAdjustment(codepoint);
  m_opticalScale = optical.scale;
  m_opticalX = optical.x;
  m_opticalY = optical.y;
  m_glyphNode->setCodepoint(codepoint);
  m_glyphNode->setFontSize(m_logicalFontSize * m_opticalScale);
  m_measureCached = false;
  return true;
}

void Glyph::setGlyphSize(float size) {
  if (size == m_logicalFontSize) {
    return;
  }
  m_logicalFontSize = size;
  m_glyphNode->setFontSize(size * m_opticalScale);
  m_measureCached = false;
}

void Glyph::setColor(const ColorSpec& color) {
  m_color = color;
  applyPalette();
}

void Glyph::setColor(const Color& color) { setColor(fixedColorSpec(color)); }

void Glyph::setShadow(const ColorSpec& color, float offsetX, float offsetY) {
  m_shadowColor = color;
  m_shadowOffsetX = offsetX;
  m_shadowOffsetY = offsetY;
  applyPalette();
}

void Glyph::setShadow(const Color& color, float offsetX, float offsetY) {
  setShadow(fixedColorSpec(color), offsetX, offsetY);
}

void Glyph::clearShadow() {
  m_shadowColor.reset();
  m_glyphNode->clearShadow();
}

void Glyph::applyPalette() {
  m_glyphNode->setColor(resolveColorSpec(m_color));
  if (m_shadowColor.has_value()) {
    m_glyphNode->setShadow(resolveColorSpec(*m_shadowColor), m_shadowOffsetX, m_shadowOffsetY);
  }
}

void Glyph::doLayout(Renderer& renderer) { measure(renderer); }

LayoutSize Glyph::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureWithConstraints(renderer, constraints);
}

void Glyph::doArrange(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  LayoutConstraints constraints;
  constraints.setExactWidth(rect.width);
  if (rect.height > 0.0F) {
    constraints.setExactHeight(rect.height);
  }
  measureWithConstraints(renderer, constraints);
}

void Glyph::measure(Renderer& renderer) {
  LayoutConstraints constraints;
  if (width() > 0.0F && !sizeAssignedByLayout()) {
    constraints.setExactWidth(width());
  }
  measureWithConstraints(renderer, constraints);
}

LayoutSize Glyph::measureWithConstraints(Renderer& renderer, const LayoutConstraints& constraints) {
  const float renderScale = renderer.renderScale();
  if (m_measureCached
      && m_cachedCodepoint == m_glyphNode->codepoint()
      && m_cachedFontSize == m_glyphNode->fontSize()
      && m_cachedLogicalFontSize == m_logicalFontSize
      && m_cachedConstraintMaxWidth == constraints.maxWidth
      && m_cachedConstraintMaxHeight == constraints.maxHeight
      && m_cachedRenderScale == renderScale
      && m_cachedHasConstraintMaxWidth == constraints.hasMaxWidth
      && m_cachedHasConstraintMaxHeight == constraints.hasMaxHeight) {
    return LayoutSize{.width = width(), .height = height()};
  }
  auto metrics = renderer.measureGlyph(m_glyphNode->codepoint(), m_glyphNode->fontSize());

  // Tabler icons are designed on a square viewport. Keep layout stable by
  // exposing that square instead of each icon's ink box; only the internal
  // glyph origin uses measured ink extents for centering.
  const float boxSize = std::round(m_logicalFontSize);
  const float finalWidth = constraints.hasExactWidth() ? constraints.maxWidth : boxSize;
  const float finalHeight = constraints.hasExactHeight() ? constraints.maxHeight : boxSize;
  Node::setSize(std::round(finalWidth), std::round(finalHeight));

  const float glyphCenterX = (metrics.left + metrics.right) * 0.5F;
  const float glyphInkCenter = (metrics.top + metrics.bottom) * 0.5F; // relative to baseline
  m_baselineOffset = height() * 0.5F - glyphInkCenter;
  m_glyphNode->setPosition(
      width() * 0.5F - glyphCenterX + m_opticalX * m_logicalFontSize, m_baselineOffset + m_opticalY * m_logicalFontSize
  );

  m_cachedCodepoint = m_glyphNode->codepoint();
  m_cachedFontSize = m_glyphNode->fontSize();
  m_cachedLogicalFontSize = m_logicalFontSize;
  m_cachedConstraintMaxWidth = constraints.maxWidth;
  m_cachedConstraintMaxHeight = constraints.maxHeight;
  m_cachedRenderScale = renderScale;
  m_cachedHasConstraintMaxWidth = constraints.hasMaxWidth;
  m_cachedHasConstraintMaxHeight = constraints.hasMaxHeight;
  m_measureCached = true;
  return LayoutSize{.width = width(), .height = height()};
}
