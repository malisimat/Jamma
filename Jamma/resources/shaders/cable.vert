#version 330 core
layout(location = 0) in vec2 PositionIN;
out vec4 ColorFRAG;
uniform mat4 MVP;
uniform vec4 CableControlPoints[64];
uniform vec4 CableColors[64];
uniform int CableCount;
uniform int SegmentCount;

vec2 EvalBezier(float t, vec2 p0, vec2 p1, vec2 p2, vec2 p3)
{
    float omt = 1.0 - t;
    float omt2 = omt * omt;
    float omt3 = omt2 * omt;
    float t2 = t * t;
    float t3 = t2 * t;
    return vec2(
        omt3 * p0.x + 3.0 * omt2 * t * p1.x + 3.0 * omt * t2 * p2.x + t3 * p3.x,
        omt3 * p0.y + 3.0 * omt2 * t * p1.y + 3.0 * omt * t2 * p2.y + t3 * p3.y
    );
}

void main()
{
    int cable = gl_InstanceID;
    if (cable < 0 || cable >= CableCount)
        return;

    int base = cable * 4;
    vec2 p0 = CableControlPoints[base + 0].xy;
    vec2 p1 = CableControlPoints[base + 1].xy;
    vec2 p2 = CableControlPoints[base + 2].xy;
    vec2 p3 = CableControlPoints[base + 3].xy;

    float t = float(gl_VertexID) / max(1, SegmentCount - 1);
    vec2 pos = EvalBezier(t, p0, p1, p2, p3);
    ColorFRAG = CableColors[cable];
    gl_Position = MVP * vec4(pos, 0.0, 1.0);
}
