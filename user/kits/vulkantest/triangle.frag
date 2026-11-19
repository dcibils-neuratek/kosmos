// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The triangle's colour, from the caller (`vulkantest.c`).
#version 450

layout(push_constant) uniform Triangle {
    vec4 colour;
    vec2 corner[3];
    vec2 size;
} t;

layout(location = 0) out vec4 colour;

void main()
{
    colour = t.colour;
}
