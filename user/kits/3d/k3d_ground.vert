// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The ground (`k3d_vk.c`, `docs/flight.md` F4b): a terrain chunk's corners
// through the view, where each is in the world, and where on the chunk's
// imagery.
#version 450

layout(push_constant) uniform View {
    mat4 clip;          // the world to clip space
    vec4 eye;           // xyz: the eye, in the world
    vec4 sun;           // xyz: towards the sun; w: the ambient share
    vec4 haze;          // rgb: the haze's colour
    vec4 range;         // x: where haze starts, y: where it is whole
    vec4 light;         // rgb: the sun's colour (F6a)
} v;

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec3 world;
layout(location = 1) out vec3 facing;
layout(location = 2) out vec2 across;

void main()
{
    world = position;
    facing = normal;
    across = uv;
    gl_Position = v.clip * vec4(position, 1.0);
}
