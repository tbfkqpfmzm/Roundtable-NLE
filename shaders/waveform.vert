#version 450
// Waveform vertex shader.
// Step 19: GPU Waveform Renderer.
//
// Receives per-vertex peak data:
//   - (x, y)    : screen-space position in pixels
//   - amplitude  : normalized [0,1] for gradient color selection
//   - channel    : channel index for multi-channel coloring
//
// Push constants provide MVP and gradient colors.
// Typical usage: draw as VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
// where each pair of vertices is (min, max) of a peak column.

layout(location = 0) in float inX;
layout(location = 1) in float inY;
layout(location = 2) in float inAmplitude;
layout(location = 3) in float inChannel;

layout(location = 0) out float fragAmplitude;
layout(location = 1) out float fragChannel;

layout(push_constant) uniform PushConstants {
    mat4 mvp;               // Orthographic projection
    vec4 colorLow;          // Green (quiet)
    vec4 colorMid;          // Yellow (moderate)
    vec4 colorHigh;         // Red (loud)
} pc;

void main()
{
    gl_Position = pc.mvp * vec4(inX, inY, 0.0, 1.0);
    fragAmplitude = inAmplitude;
    fragChannel = inChannel;
}
