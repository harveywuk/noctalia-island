#pragma once

// Analytic glass, flowing light and a small face, rendered in one GLES2 pass.
inline constexpr char kAssistantOrbFragment[] = R"(
uniform vec3 u_orb_face;
uniform vec4 u_orb_motion;
uniform vec3 u_orb_color0;
uniform vec3 u_orb_color1;
uniform vec3 u_orb_color2;
uniform vec3 u_orb_color3;

float orbHash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float orbNoise(vec2 p) {
    vec2 cell = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(orbHash(cell), orbHash(cell + vec2(1.0, 0.0)), f.x),
        mix(orbHash(cell + vec2(0.0, 1.0)), orbHash(cell + vec2(1.0)), f.x), f.y);
}

float orbSmoke(vec2 p) {
    float smoke = 0.0;
    float weight = 0.57;
    for (int i = 0; i < 3; ++i) {
        smoke += orbNoise(p) * weight;
        p = mat2(0.80, -0.60, 0.60, 0.80) * p * 2.03 + vec2(3.1, 7.7);
        weight *= 0.5;
    }
    return smoke;
}

float orbStrokeDistance(vec2 p, vec2 a, vec2 b, float radius) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
    return length(p - a - ab * t) - radius;
}

float orbQuestionDistance(vec2 p) {
    vec2 bowl = p - vec2(0.0, -0.095);
    float arc = max(abs(length(bowl) - 0.115) - 0.029, min(bowl.y, -bowl.x));
    float cap = length(p - vec2(-0.115, -0.095)) - 0.029;
    float stem = orbStrokeDistance(p, vec2(0.0, 0.02), vec2(0.0, 0.065), 0.029);
    float dot = length(p - vec2(0.0, 0.16)) - 0.033;
    return min(min(arc, cap), min(stem, dot));
}

float orbEyeDistance(vec2 p, float open, float happy) {
    vec2 size = vec2(0.064, max(0.012, 0.230 * open));
    float distance = roundedBoxSDF(p, size, min(size.x, size.y));
    float arc = max(abs(p.y + 0.06 * (1.0 - pow(p.x / 0.095, 2.0))) - 0.019,
        abs(p.x) - 0.095);
    return mix(distance, arc, happy);
}

void main() {
    float energy = u_orb_motion.x;
    float aa = 1.65 / max(u_item_width, 1.0);
    vec2 p = (v_uv - 0.5) * 2.3;
    p /= vec2(1.0 + energy * 0.025, 1.0 + energy * 0.018);
    float r = length(p);
    float sphere = 1.0 - smoothstep(0.89 - aa, 0.89 + aa, r);
    float halo = exp(-pow((r - 0.88) / 0.155, 2.0)) * (0.22 + energy * 0.10);
    halo *= 1.0 - smoothstep(1.02, 1.14, r);
    vec3 normal = normalize(vec3(p, sqrt(max(0.0, 0.90 * 0.90 - dot(p, p)))));

    // Refracted, layered vapour with soft curls and darker pockets inside the glass.
    float angle = 0.48 + (0.9 - r) * 1.8 + u_time * 0.12;
    vec2 q = mat2(cos(angle), -sin(angle), sin(angle), cos(angle)) * p;
    vec2 drift = vec2(u_time * 0.13, -u_time * 0.10);
    vec2 warp = vec2(orbSmoke(q * 2.4 + drift),
        orbSmoke(q * 2.4 + vec2(5.2, 1.3) - drift)) - 0.5;
    vec2 flow = q * 2.2 + warp * 1.6;
    float density = orbSmoke(flow + drift * 0.45);
    float wisps = orbSmoke(flow * 1.7 + warp - drift * 0.7);
    float curl = q.y + 0.30 * sin(q.x * 3.8 + warp.x * 3.0 - u_time * 0.22)
        + 0.45 * (density - 0.5);
    float plume = exp(-pow(curl / 0.34, 2.0)) * (0.4 + density * 0.9);
    float veil = exp(-pow((curl + 0.31) / 0.27, 2.0)) * wisps;
    float filaments = pow(1.0 - abs(wisps * 2.0 - 1.0), 4.0) * plume;
    float interior = 1.0 - smoothstep(0.64, 0.87, r);
    float palette = clamp(0.5 + q.x * 0.6 + warp.y * 0.4, 0.0, 1.0);
    vec3 cool = mix(u_orb_color0, u_orb_color1, smoothstep(0.0, 0.6, palette));
    vec3 warm = mix(u_orb_color2, u_orb_color3, smoothstep(0.4, 1.0, palette));
    vec3 light = mix(cool, warm, smoothstep(-0.30, 0.30, curl + warp.x * 0.2));
    vec3 color = vec3(0.004, 0.008, 0.023);
    color += light * (0.035 + plume * 0.64 + veil * 0.42 + filaments * 0.13)
        * interior * (0.85 + energy * 0.30);
    color += mix(u_orb_color1, vec3(1.0), 0.20) * filaments * interior * 0.13;

    // Opposing reflected arcs make the dark shell read as curved, polished glass.
    vec2 direction = p / max(r, 0.001);
    float upper = pow(max(dot(direction, normalize(vec2(0.63, -0.78))), 0.0), 18.0);
    float lower = pow(max(dot(direction, normalize(vec2(-0.62, 0.78))), 0.0), 28.0);
    float rim = exp(-pow((r - 0.822) / 0.023, 2.0));
    float reflection = rim * (upper * 0.75 + lower * 0.55);
    reflection += exp(-pow((r - 0.775) / 0.035, 2.0)) * upper * 0.14;
    // A soft spill follows the state palette around the glass, within the same pass.
    vec2 rimBlend = direction * 0.5 + 0.5;
    vec3 haloColor = mix(mix(u_orb_color0, u_orb_color1, rimBlend.x),
        mix(u_orb_color3, u_orb_color2, rimBlend.x), rimBlend.y);
    float rimGlow = exp(-pow((r - 0.825) / 0.080, 2.0))
        * (0.07 + upper * 0.14 + lower * 0.10);
    color += haloColor * rimGlow * (1.0 + energy * 0.25);
    color += mix(u_orb_color0, vec3(0.87, 0.92, 1.0), 0.7) * reflection;
    color += vec3(0.24, 0.3, 0.42) * pow(max(dot(normal, normalize(vec3(0.45, -0.65, 0.58))), 0.0), 32.0) * 0.22;

    // Tall, pale eyes carry the expression over the moving light at small sizes.
    vec2 face = p - vec2(u_orb_motion.z, u_orb_motion.w);
    float blink = u_orb_motion.y;
    float error = clamp(u_orb_face.z, 0.0, 1.0);
    float happy = smoothstep(0.75, 0.98, u_orb_face.y) * blink * (1.0 - error);
    float eyeOpen = u_orb_face.x * blink;
    float separation = 0.215 * (1.0 - smoothstep(0.0, 0.6, error));
    float left = orbEyeDistance(face - vec2(-separation, -0.025), eyeOpen, happy);
    float right = orbEyeDistance(face - vec2(separation, -0.025), eyeOpen, happy);
    float distance = min(left, right);
    if (error > 0.001) {
        // The two eyes draw together before becoming one larger question mark.
        float question = orbQuestionDistance(face / 1.7) * 1.7;
        distance = mix(distance, question, smoothstep(0.25, 1.0, error));
    }
    float mask = 1.0 - smoothstep(-aa, aa, distance);
    float faceShade = exp(-dot(face * vec2(1.4, 2.0), face * vec2(1.4, 2.0)) * 5.0) * 0.22;
    color *= 1.0 - faceShade;
    color = mix(color, vec3(0.88, 0.95, 1.0), mask * 0.96);

    float alpha = sphere + (1.0 - sphere) * halo;
    vec3 rgb = color * sphere + haloColor * halo * (1.0 - sphere) * 0.68;
    gl_FragColor = vec4(rgb * u_bg_color.a, alpha * u_bg_color.a);
}
)";
