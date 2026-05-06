#version 450
// Fullscreen quad vertex shader — used for blitting textures to screen.
// Step 2: Proof-of-life rendering.

layout(location = 0) out vec2 fragTexCoord;

// Fullscreen triangle (no vertex buffer needed)
void main()
{
    // Generate fullscreen triangle from vertex ID
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );

    vec2 texCoords[3] = vec2[](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0)
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragTexCoord = texCoords[gl_VertexIndex];
}
