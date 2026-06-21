#version 330 core

in vec2 UV;
out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform vec3 TintColor;

void main()
{
    ColorOUT = texture(TextureSampler, UV) * vec4(TintColor, 1.0);
}