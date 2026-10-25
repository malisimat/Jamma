#version 330 core

in vec2 UV;
out vec4 ColorOUT;
uniform float Opacity;
uniform vec3 TextColor;

uniform sampler2D TextureSampler;

void main()
{
    // Atlas is GL_R8; the red channel holds glyph coverage
    float alpha = texture(TextureSampler, UV).r;
    ColorOUT = vec4(TextColor, alpha * Opacity);
}
