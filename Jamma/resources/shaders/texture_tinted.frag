#version 330 core

in vec2 UV;
out vec4 ColorOUT;
uniform float Opacity;

uniform sampler2D TextureSampler;
uniform vec3 TintColor;

void main()
{
    ColorOUT = texture(TextureSampler, UV) * vec4(TintColor, Opacity);
}