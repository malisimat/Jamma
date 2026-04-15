#version 330 core

layout(location = 0) in vec3 PositionIN;

out vec3 TexCoords;

uniform mat4 MVP;

void main()
{
    TexCoords = PositionIN;
    // Force fragment depth to 1.0 (far plane) so skybox renders behind everything
    vec4 pos = MVP * vec4(PositionIN, 1.0);
    gl_Position = pos.xyww;
}
