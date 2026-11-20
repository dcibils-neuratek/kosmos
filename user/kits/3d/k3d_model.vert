// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// A model (`k3d_vk.c`, `docs/flight.md` F5d): an aircraft's part, in its
// own coordinates, put in the world by its own matrix - so the whole moves
// with the aircraft and an aileron or a propeller with its hinge - then
// through the view as a mesh is (`k3d_mesh.vert`).
#version 450

layout(push_constant) uniform Model {
    mat4 clip;          // the world to clip space
    vec4 eye;           // xyz: the eye, in the world
    vec4 sun;           // xyz: towards the sun; w: the ambient share
    vec4 haze;          // rgb: the haze's colour
    vec4 range;         // x: where haze starts, y: where it is whole
    mat4 model;         // the world from the model
    vec4 tint;          // what the texture is multiplied by
} v;

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec3 world;
layout(location = 1) out vec3 facing;
layout(location = 2) out vec2 across;

void main()
{
    vec4 placed = v.model * vec4(position, 1.0);

    world = placed.xyz;
    // The matrix's own 3 by 3 turns a normal as it turns the surface,
    // which holds while the matrix only turns, moves and scales evenly -
    // what an aircraft and its parts do. A matrix that stretched one way
    // more than another would want its inverse's transpose here.
    facing = mat3(v.model) * normal;
    across = uv;
    gl_Position = v.clip * placed;
}
