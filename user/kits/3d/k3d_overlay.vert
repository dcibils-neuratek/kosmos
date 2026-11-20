// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// An overlay (`k3d_vk.c`): a picture laid over the finished frame - a
// game's strip of instruments, a menu - as two triangles over the rectangle
// its push constant gives in clip space, nearest the eye and untested.
#version 450

layout(push_constant) uniform Place {
    vec4 rect;                      // left, top, right, bottom, in clip space
} place;

layout(location = 0) out vec2 across;

void main()
{
    const vec2 corners[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                   vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    vec2 c = corners[gl_VertexIndex];

    across = c;
    gl_Position = vec4(mix(place.rect.xy, place.rect.zw, c), 0.0, 1.0);
}
