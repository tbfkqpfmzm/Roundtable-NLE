#version 450
// Text vertex shader — SDF text rendering.
// Step 20: GPU Text Renderer.
//
// Each glyph is rendered as a textured quad (6 vertices = 2 triangles).
// Vertex attributes:
//   - position (x, y) in screen/pixel space
//   - texcoord (u, v) into the SDF glyph atlas
//   - color    (packed RGBA, auto-converted via R8G8B8A8_UNORM)

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec4 inColor;    // R8G8B8A8_UNORM → vec4

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;                // Orthographic projection
    vec4 outlineColor;
    float outlineWidth;
    float smoothing;
    float shadowOffsetX;
    float shadowOffsetY;
    vec4 shadowColor;
} pc;

void main()
{
    gl_Position = pc.mvp * vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
}
