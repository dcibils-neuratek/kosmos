// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The cube's faces, from its texture and nothing else (`vulkantest.c`).
#version 450

layout(set = 0, binding = 0) uniform sampler2D checks;

layout(location = 0) in vec2 texel;
layout(location = 0) out vec4 colour;

void main()
{
    colour = texture(checks, texel);
}
