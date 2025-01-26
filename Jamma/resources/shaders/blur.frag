#version 330 core

in vec2 UV;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;

void main()
{
    vec3 texColor = texture(TextureSampler, UV).xyz;

    ColorOUT = vec4(texColor, 1.0);
}