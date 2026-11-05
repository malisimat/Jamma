#version 330 core

out vec4 ColorOUT;
uniform float Opacity;

uniform vec4 Color;
uniform vec3 RectSize;
uniform float CornerRadius;
in vec2 RectPosition;

void main()
{
    float coverage = 1.0;
    if (CornerRadius > 0.0)
    {
        float radius = min(CornerRadius, min(RectSize.x, RectSize.y) * 0.5);
        vec2 q = abs((RectPosition - 0.5) * RectSize.xy) - RectSize.xy * 0.5 + radius;
        float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        float edgeWidth = max(fwidth(distance), 0.001);
        coverage = 1.0 - smoothstep(-edgeWidth * 0.5, edgeWidth * 0.5, distance);
    }
    ColorOUT = vec4(Color.rgb, Color.a * Opacity * coverage);
}
