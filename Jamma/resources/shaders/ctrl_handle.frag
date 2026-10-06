#version 330 core

out vec4 ColorOUT;
uniform float Opacity;

uniform vec4 Color;

void main()
{
    ColorOUT = vec4(Color.rgb, Color.a * Opacity);
}
