#version 330 core

in float Velocity;
in float Diff;

out vec4 ColorOUT;

uniform int ObjectId;
uniform float Highlight;
uniform float LoopHover;
uniform int RenderMode;

void main()
{
    if (RenderMode == 1)
    {
        float r = ((ObjectId >> 16) & 0xff) / 255.0;
        float g = ((ObjectId >> 8) & 0xff) / 255.0;
        float b = (ObjectId & 0xff) / 255.0;
        ColorOUT = vec4(r, g, b, 1.0);
        return;
    }

    if (RenderMode == 2)
    {
        ColorOUT = vec4(Highlight);
        return;
    }

    vec3 low = vec3(0.1, 0.45, 0.95);
    vec3 mid = vec3(0.1, 0.9, 0.55);
    vec3 high = vec3(1.0, 0.72, 0.18);
    float upper = smoothstep(0.45, 1.0, Velocity);
    vec3 baseColor = mix(mix(low, mid, smoothstep(0.0, 0.55, Velocity)), high, upper);
    ColorOUT = vec4((baseColor * Diff) + (LoopHover * vec3(0.18, 0.18, 0.12)), 0.88);
}