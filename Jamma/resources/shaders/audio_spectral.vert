#version 330 core

layout(location = 0) in vec2 GridUv;

out vec3 SurfacePosition;
out vec2 SurfaceUv;
out float SurfaceHeight;
out float SurfaceMorph;

uniform mat4 MVP;
uniform sampler2D SpectralHeightSampler;
uniform float SpectralRadius;
uniform float SpectralHeightScale;
uniform float SpectralDepthScale;
uniform float SpectralBaseY;
uniform float EditorMorph;
uniform float EditorPlayFrac;

void main()
{
    ivec2 dimensions = textureSize(SpectralHeightSampler, 0);
    ivec2 vertex = ivec2(round(GridUv * vec2(dimensions - ivec2(1))));
    // Heights belong to exact grid vertices; rasterized triangles supply the
    // interpolation used by the CPU picking mirror. No half-texel/filter drift.
    float height = texelFetch(SpectralHeightSampler, vertex, 0).r;
    float angle = 6.283185307179586 * GridUv.x;
    float ringRadius = SpectralRadius * (1.0 + (GridUv.y - 0.5) * 0.10);
    vec3 ring = vec3(sin(angle) * ringRadius, SpectralBaseY, cos(angle) * ringRadius);
    vec3 rectangle = vec3((GridUv.x - 0.5) * 2.0 * SpectralRadius,
        SpectralBaseY + height * SpectralRadius * SpectralHeightScale,
        -(GridUv.y * 2.0 - 1.0) * SpectralRadius * SpectralDepthScale);
    vec3 point = mix(ring, rectangle, EditorMorph);
    // Match the selected waveform ring phase; rotation vanishes at the rectangle.
    float rotation = 6.283185307179586 * (1.0 - EditorPlayFrac) * (1.0 - EditorMorph);
    point = vec3(cos(rotation) * point.x + sin(rotation) * point.z, point.y,
        -sin(rotation) * point.x + cos(rotation) * point.z);
    SurfacePosition = point;
    gl_Position = MVP * vec4(point, 1.0);
    SurfaceUv = GridUv;
    SurfaceHeight = height;
    SurfaceMorph = EditorMorph;
}
