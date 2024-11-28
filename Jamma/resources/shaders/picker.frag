#version 330 core

in vec2 UV;
out vec4 ColorOUT;
uniform uint ObjectId;

void main()
{
    float a = (data >> 24) & 0xff;
    float r = (data >> 16) & 0xff;
    float g = (data >> 8) & 0xff;
    float b = data & 0xff;
    ColorOUT = vec4(r, g, b, a);
}