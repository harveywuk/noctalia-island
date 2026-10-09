#include "render/programs/countdown_ring_program.h"

#include "render/core/render_styles.h"

#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform vec2 u_rect_origin;
uniform vec2 u_rect_size;
uniform mat3 u_transform;
varying vec2 v_pixel;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_pixel = local - u_rect_origin;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform vec2 u_rect_size;
uniform vec4 u_color;
uniform float u_thickness;
uniform float u_progress;
uniform float u_corner_radius;
uniform float u_start_offset;
uniform bool u_has_colors;
uniform vec4 u_colors[17];
varying vec2 v_pixel;

const float PI = 3.14159265359;

void main() {
    vec2 center = u_rect_size * 0.5;
    float radius = min(u_rect_size.x, u_rect_size.y) * 0.5 - u_thickness * 0.5;
    vec2 p = v_pixel - center;
    float dist = length(p);

    float ring = abs(dist - radius) - u_thickness * 0.5;
    float aa = max(1.0, u_thickness * 0.18);
    float ringMask = 1.0 - smoothstep(-aa, aa, ring);

    float theta = atan(p.y, p.x);
    float start = -PI * 0.5;
    float rel = mod(theta - start + 2.0 * PI, 2.0 * PI);
    float along = rel / (2.0 * PI);
    float arcLen = 2.0 * PI * clamp(u_progress, 0.0, 1.0);
    float arcMask = 1.0 - smoothstep(arcLen - 0.06, arcLen + 0.06, rel);

    if (u_corner_radius >= 0.0) {
        vec2 halfSize = max(center - u_thickness * 0.5, vec2(0.001));
        float r = clamp(u_corner_radius - u_thickness * 0.5, 0.001, min(halfSize.x, halfSize.y));
        vec2 straight = halfSize - r;
        vec2 q = abs(p);
        vec2 d = q - straight;
        float edge = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
        ringMask = 1.0 - smoothstep(-0.75, 0.75, abs(edge) - u_thickness * 0.5);
        // Arc length from top centre, clockwise, including each straight side.
        float quarter = straight.x + straight.y + PI * r * 0.5;
        float distance;
        if (q.x <= straight.x) distance = q.x;
        else if (q.y <= straight.y) distance = straight.x + PI * r * 0.5 + straight.y - q.y;
        else distance = straight.x + atan(q.x - straight.x, q.y - straight.y) * r;
        if (p.y >= 0.0) distance = 2.0 * quarter - distance;
        if (p.x < 0.0) distance = 4.0 * quarter - distance;
        float perimeter = 4.0 * quarter;
        along = mod(distance / perimeter - u_start_offset + 1.0, 1.0);
        float feather = 0.75 / perimeter;
        arcMask = u_progress >= 1.0 ? 1.0 : u_progress <= 0.0 ? 0.0
            : 1.0 - smoothstep(u_progress - feather, u_progress + feather, along);
    }

    vec4 tint = vec4(u_color.rgb, 1.0);
    if (u_has_colors) {
        // Blend adjacent lights along the perimeter, without inventing a total
        // or creating an animation separate from the publisher's pattern.
        float position = clamp(along * 17.0 - 0.5, 0.0, 16.0);
        tint = vec4(0.0);
        for (int i = 0; i < 17; ++i)
            tint += u_colors[i] * max(0.0, 1.0 - abs(position - float(i)));
        arcMask = 1.0;
    }
    float alpha = ringMask * arcMask * u_color.a;
    if (alpha <= 0.0) {
        discard;
    }

    gl_FragColor = tint * alpha;
}
)";

} // namespace

void CountdownRingProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_quadSizeLocation = glGetUniformLocation(m_program.id(), "u_quad_size");
  m_rectOriginLocation = glGetUniformLocation(m_program.id(), "u_rect_origin");
  m_rectSizeLocation = glGetUniformLocation(m_program.id(), "u_rect_size");
  m_colorLocation = glGetUniformLocation(m_program.id(), "u_color");
  m_thicknessLocation = glGetUniformLocation(m_program.id(), "u_thickness");
  m_progressLocation = glGetUniformLocation(m_program.id(), "u_progress");
  m_cornerRadiusLocation = glGetUniformLocation(m_program.id(), "u_corner_radius");
  m_startOffsetLocation = glGetUniformLocation(m_program.id(), "u_start_offset");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");
  m_hasColorsLocation = glGetUniformLocation(m_program.id(), "u_has_colors");
  m_colorsLocation = glGetUniformLocation(m_program.id(), "u_colors[0]");

  if (m_positionLocation < 0
      || m_surfaceSizeLocation < 0
      || m_quadSizeLocation < 0
      || m_rectOriginLocation < 0
      || m_rectSizeLocation < 0
      || m_colorLocation < 0
      || m_thicknessLocation < 0
      || m_cornerRadiusLocation < 0
      || m_startOffsetLocation < 0
      || m_progressLocation < 0
      || m_hasColorsLocation < 0
      || m_colorsLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query countdown ring shader locations");
  }
}

void CountdownRingProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_quadSizeLocation = -1;
  m_rectOriginLocation = -1;
  m_rectSizeLocation = -1;
  m_colorLocation = -1;
  m_thicknessLocation = -1;
  m_progressLocation = -1;
  m_cornerRadiusLocation = -1;
  m_startOffsetLocation = -1;
  m_transformLocation = -1;
  m_hasColorsLocation = -1;
  m_colorsLocation = -1;
}

void CountdownRingProgram::abandon() noexcept { m_program.abandon(); }

void CountdownRingProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const CountdownRingStyle& style,
    const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0F || height <= 0.0F) {
    return;
  }

  const std::array<GLfloat, 12> vertices = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  const float padding = style.thickness + 2.0F;
  const float quadWidth = width + padding * 2.0F;
  const float quadHeight = height + padding * 2.0F;
  const Mat3 quadTransform = transform * Mat3::translation(-padding, -padding);

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLocation, quadWidth, quadHeight);
  glUniform2f(m_rectOriginLocation, padding, padding);
  glUniform2f(m_rectSizeLocation, width, height);
  glUniform4f(m_colorLocation, style.color.r, style.color.g, style.color.b, style.color.a);
  glUniform1f(m_thicknessLocation, style.thickness);
  glUniform1f(m_progressLocation, style.progress);
  glUniform1f(m_cornerRadiusLocation, style.cornerRadius);
  glUniform1f(m_startOffsetLocation, style.startOffset);
  glUniform1i(m_hasColorsLocation, style.colors.has_value());
  if (style.colors) {
    std::array<GLfloat, 17 * 4> colors{};
    for (std::size_t i = 0; i < style.colors->size(); ++i) {
      const auto& color = (*style.colors)[i];
      colors[i * 4] = color.r * color.a;
      colors[i * 4 + 1] = color.g * color.a;
      colors[i * 4 + 2] = color.b * color.a;
      colors[i * 4 + 3] = color.a;
    }
    glUniform4fv(m_colorsLocation, 17, colors.data());
  }
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, quadTransform.m.data());
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
