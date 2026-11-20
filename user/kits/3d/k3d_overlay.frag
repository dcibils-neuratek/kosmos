// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// An overlay's picture, as it is: its alpha is how much of it covers the
// frame, which the pipeline's blending does with it.
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;

layout(location = 0) in vec2 across;
layout(location = 0) out vec4 colour;

void main()
{
    colour = texture(picture, across);
}
