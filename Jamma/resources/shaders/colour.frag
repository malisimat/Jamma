#version 330 core

out vec4 ColorOUT;

uniform vec4 Color;

void main()
{
    ColorOUT = vec4(1.0, 0.3, 0.6, 1.0);//Color;
}