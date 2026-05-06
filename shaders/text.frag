#version 450
// Text fragment shader — SDF text rendering with outline and shadow.
// Step 20: GPU Text Renderer.
//
// Reads from a single-channel SDF atlas texture.
// SDF value interpretation:
//   0   = far outside the glyph
//   128 = exactly on the glyph edge (configurable via sdfOnEdge)
//   255 = well inside the glyph
//
// Features:
//   - Anti-aliased edges via SDF smoothstep
//   - Configurable outline (dual-threshold on SDF)
//   - Drop shadow with configurable offset and color

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sdfAtlas;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 outlineColor;
    float outlineWidth;      // 0 = no outline
    float smoothing;         // SDF anti-aliasing width (~1/16 for 32px dist)
    float shadowOffsetX;     // 0 = no shadow
    float shadowOffsetY;
    vec4 shadowColor;
} pc;

void main()
{
    float distance = texture(sdfAtlas, fragTexCoord).r;

    // Edge threshold (SDF value at exactly the glyph boundary)
    float edgeCenter = 0.5;  // Normalized: 128/255 ≈ 0.5

    // ── Shadow pass ──────────────────────────────────────────────────────
    float shadowAlpha = 0.0;
    if (pc.shadowOffsetX != 0.0 || pc.shadowOffsetY != 0.0) {
        vec2 shadowUV = fragTexCoord - vec2(pc.shadowOffsetX, pc.shadowOffsetY);
        float shadowDist = texture(sdfAtlas, shadowUV).r;
        shadowAlpha = smoothstep(edgeCenter - pc.smoothing,
                                 edgeCenter + pc.smoothing,
                                 shadowDist);
    }

    // ── Outline pass ─────────────────────────────────────────────────────
    float outlineEdge = edgeCenter - pc.outlineWidth;
    float outlineAlpha = smoothstep(outlineEdge - pc.smoothing,
                                    outlineEdge + pc.smoothing,
                                    distance);

    // ── Fill pass ────────────────────────────────────────────────────────
    float fillAlpha = smoothstep(edgeCenter - pc.smoothing,
                                 edgeCenter + pc.smoothing,
                                 distance);

    // Composite: shadow → outline → fill
    vec4 color = vec4(0.0);

    // Shadow layer
    if (shadowAlpha > 0.0) {
        color = vec4(pc.shadowColor.rgb, pc.shadowColor.a * shadowAlpha);
    }

    // Outline layer (over shadow)
    if (pc.outlineWidth > 0.0 && outlineAlpha > 0.0) {
        vec4 oColor = vec4(pc.outlineColor.rgb, pc.outlineColor.a * outlineAlpha);
        color = mix(color, oColor, oColor.a);
    }

    // Fill layer (over outline)
    if (fillAlpha > 0.0) {
        vec4 fColor = vec4(fragColor.rgb, fragColor.a * fillAlpha);
        color = mix(color, fColor, fColor.a);
    }

    if (color.a < 0.004) discard;  // ~1/255

    outColor = color;
}
