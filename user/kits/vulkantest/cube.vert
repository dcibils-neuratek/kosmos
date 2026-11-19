// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The cube's corners through the caller's transform (`vulkantest.c`).
#version 450

layout(push_constant) uniform Transform {
    mat4 clip;
} t;

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;

layout(location = 0) out vec2 texel;

void main()
{
    texel = uv;
    gl_Position = t.clip * vec4(position, 1.0);
}
