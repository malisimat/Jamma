#version 330 core

layout(location = 0) in vec3 GridPoint; // exact loop-local u, pitch-row v, weight

out float Weight;
out vec2 PreviewUv;

uniform mat4 MVP;
uniform float EditorGridRadius;
uniform int EditorPreviewFirstVertex;

void main()
{
    float r = EditorGridRadius;
    gl_Position = MVP * vec4((GridPoint.x - 0.5) * 2.0 * r,
        GridPoint.z < 0.0 ? 8.0 : GridPoint.z > 1.0 ? 1.6 : 2.0,
        -(GridPoint.y * 2.0 - 1.0) * 0.78 * r, 1.0);
    Weight = GridPoint.z;
    // The two preview triangles have the same corner order for every held span.
    int corner = (gl_VertexID - EditorPreviewFirstVertex) % 6;
    PreviewUv = vec2(corner == 1 || corner == 4 || corner == 5 ? 1.0 : 0.0,
        corner == 2 || corner == 3 || corner == 5 ? 1.0 : 0.0);
}
