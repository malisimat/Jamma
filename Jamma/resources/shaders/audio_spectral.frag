#version 330 core

in vec3 SurfacePosition;
in vec2 SurfaceUv;
in float SurfaceHeight;
in float SurfaceMorph;
out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform float EditorPlayFrac;
uniform float EditorTime;
uniform float SceneDim;
uniform vec4 SpectralBrush;
uniform int SpectralBrushMode;

// Ordered, continuous blue -> green -> yellow -> orange -> red ramp.
vec3 spectralColour(float level)
{
    vec3 blue = vec3(0.06, 0.18, 1.0);
    vec3 green = vec3(0.08, 0.90, 0.12);
    vec3 yellow = vec3(1.0, 0.94, 0.04);
    vec3 orange = vec3(1.0, 0.40, 0.02);
    vec3 red = vec3(1.0, 0.04, 0.02);
    if (level < 0.25) return mix(blue, green, level * 4.0);
    if (level < 0.50) return mix(green, yellow, (level - 0.25) * 4.0);
    if (level < 0.75) return mix(yellow, orange, (level - 0.50) * 4.0);
    return mix(orange, red, (level - 0.75) * 4.0);
}
void main()
{
    float height = clamp(SurfaceHeight, 0.0, 1.0);
    // Height retains +24 dB headroom; normal full-scale peaks already reach red.
    float paletteLevel = clamp(height * (114.0 / 90.0), 0.0, 1.0);
    // Emissive colour: only the very bottom fades through dark blue from black.
    vec3 colour = height > 0.0
        ? spectralColour(paletteLevel) * smoothstep(0.0, 0.12, paletteLevel)
        : vec3(0.0);
    // Triangle derivatives use the same interpolated geometry as picking.
    // A restrained fixed local light cue preserves the emissive level palette.
    vec3 normal = normalize(cross(dFdx(SurfacePosition), dFdy(SurfacePosition)));
    float highlight = pow(abs(dot(normal, normalize(vec3(-0.25, 1.0, 0.35)))), 24.0);
    colour += vec3(0.08) * highlight * smoothstep(0.0, 0.12, paletteLevel) * SurfaceMorph;
    float ahead = fract(SurfaceUv.x - EditorPlayFrac + 1.0);
    float distanceU = min(ahead, 1.0 - ahead);
    float pulse = 0.92 + 0.08 * sin(EditorTime * 3.0);
    float core = distanceU < 0.015 ? exp(-pow(distanceU / 0.0025, 2.0)) * pulse : 0.0;
    float trail = ahead < 0.15 ? exp(-ahead / 0.035) * (1.0 - smoothstep(0.12, 0.15, ahead)) : 0.0;
    // A fragment stripe follows the displayed surface even over raised peaks.
    colour += SurfaceMorph * (core * vec3(0.35, 0.85, 1.0)
        + trail * vec3(0.03, 0.12, 0.18));
    if (SpectralBrushMode > 0 && SpectralBrush.z > 0.0)
    {
        float du = abs(SurfaceUv.x - SpectralBrush.x);
        du = min(du, 1.0 - du);
        float x = du / SpectralBrush.z;
        float y = abs(SurfaceUv.y - SpectralBrush.y) / max(SpectralBrush.w, 0.00001);
        float radius = SpectralBrushMode == 3 ? x : length(vec2(x, y));
        float width = max(fwidth(radius) * 1.5, 0.04);
        float outline = 1.0 - smoothstep(width * 0.35, width, abs(radius - 1.0));
        vec3 brushColour = SpectralBrushMode == 2 ? vec3(1.0, 0.35, 0.18) : vec3(0.3, 1.0, 0.55);
        colour += outline * brushColour * SurfaceMorph;
    }
    colour *= mix(SceneDim, 1.0, SurfaceMorph);
    ColorOUT = vec4(colour, SurfaceMorph);
    gl_FragDepth = gl_FragCoord.z * mix(1.0, 0.05, SurfaceMorph);
}
