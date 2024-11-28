#version 330 core

in vec2 UV;
out vec4 ColorOUT;
uniform uint ObjectId;

void main()
{
    int id = int(ObjectId);
    float a = (id >> 24) & 0xff;
    float r = (id >> 16) & 0xff;
    float g = (id >> 8) & 0xff;
    float b = id & 0xff;
    ColorOUT = vec4(r, g, b, a);
}