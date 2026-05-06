#version 450
// Waveform fragment shader.
// Step 19: GPU Waveform Renderer.
//
// Colors each fragment by amplitude using a 3-stop gradient:
//   low (green) → mid (yellow) → high (red)
// The gradient colors are provided via push constants.

layout(location = 0) in float fragAmplitude;
layout(location = 1) in float fragChannel;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;               // Not used in frag, but must match vert layout
    vec4 colorLow;
    vec4 colorMid;
    vec4 colorHigh;
} pc;

void main()
{
    float amp = clamp(fragAmplitude, 0.0, 1.0);

    // Two-segment gradient: low→mid for [0, 0.5], mid→high for [0.5, 1.0]
    vec4 color;
    if (amp < 0.5) {
        float t = amp * 2.0;
        color = mix(pc.colorLow, pc.colorMid, t);
    } else {
        float t = (amp - 0.5) * 2.0;
        color = mix(pc.colorMid, pc.colorHigh, t);
    }

    outColor = color;
}
