#version 330 core

in vec3 TexCoords;
out vec4 ColorOUT;

uniform samplerCube SkyboxSampler;

void main()
{
    ColorOUT = texture(SkyboxSampler, TexCoords);
}
