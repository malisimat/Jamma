#version 330 core

layout(location = 0) in vec3 GridPoint; // exact loop-local u, pitch-row v, weight

out float Weight;

uniform mat4 MVP;
uniform float EditorGridRadius;

void main()
{
    float r = EditorGridRadius;
    gl_Position = MVP * vec4((GridPoint.x - 0.5) * 2.0 * r,
        GridPoint.z < 0.0 ? 8.0 : 2.0,
        -(GridPoint.y * 2.0 - 1.0) * 0.78 * r, 1.0);
    Weight = GridPoint.z;
}
