// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The triangle's corners, in pixels, from the caller (`vulkantest.c`).
#version 450

layout(push_constant) uniform Triangle {
    vec4 colour;
    vec2 corner[3];
    vec2 size;
} t;

void main()
{
    gl_Position = vec4(t.corner[gl_VertexIndex] / t.size * 2.0 - 1.0, 0.0, 1.0);
}
