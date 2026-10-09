#version 330 core
in vec2 PixelPosition;
out vec4 ColorOUT;
uniform float Opacity;
uniform vec4 Color;
uniform vec4 Bounds;
uniform bool Panel;
void main()
{
    vec2 halfSize = Bounds.zw * 0.5;
    vec2 local = PixelPosition - Bounds.xy - halfSize;
    float radius = Panel ? 2.0 : 6.0;
    vec2 corner = abs(local) - halfSize + vec2(radius);
    float distance = length(max(corner, vec2(0.0))) + min(max(corner.x, corner.y), 0.0) - radius;
    float mask = 1.0 - smoothstep(-0.7, 0.7, distance);
    float border = 1.0 - smoothstep(0.8, 1.8, -distance);
    float centre = 1.0 - clamp(length(local / halfSize), 0.0, 1.0);
    vec3 fill = Panel ? Color.rgb : Color.rgb * (0.28 + 0.19 * centre);
    vec3 edge = Panel ? vec3(0.48) : Color.rgb;
    ColorOUT = vec4(mix(fill, edge, border), Color.a * Opacity * mask);
}
