#version 330 core

in vec2 UV;
out vec4 ColorOUT;
uniform int ObjectId;

void main()
{
    float r = ((ObjectId >> 16) & 0xff) / 255.0;
    float g = ((ObjectId >> 8) & 0xff) / 255.0;
    float b = (ObjectId & 0xff) / 255.0;
    ColorOUT = vec4(r, g, b, 1.0);
}