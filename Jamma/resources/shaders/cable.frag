#version 330 core

in vec4 ColorFRAG;
out vec4 ColorOUT;
uniform float Opacity;

void main()
{
    ColorOUT = vec4(ColorFRAG.rgb, ColorFRAG.a * Opacity);
}
