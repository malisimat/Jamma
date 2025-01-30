#version 330 core

out vec4 ColorOUT;

uniform float Highlight;

void main()
{
    ColorOUT = vec4(Highlight);
}