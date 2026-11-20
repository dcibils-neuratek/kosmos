// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// A model's surface (`k3d_vk.c`): its texture times its tint, lit by the
// sun and the ambient share and hazed by distance exactly as a mesh's
// colour is (`k3d_mesh.frag`), so an aircraft sits in the same light as
// the ground under it.
#version 450

layout(push_constant) uniform Model {
    mat4 clip;
    vec4 eye;
    vec4 sun;
    vec4 haze;
    vec4 range;
    mat4 model;
    vec4 tint;
} v;

layout(set = 0, binding = 0) uniform sampler2D skin;

layout(location = 0) in vec3 world;
layout(location = 1) in vec3 facing;
layout(location = 2) in vec2 across;

layout(location = 0) out vec4 colour;

void main()
{
    vec4 albedo = texture(skin, across) * v.tint;

    // Nothing is blended yet, so a texel mostly transparent is a hole -
    // glTF's MASK at its default cutoff - rather than an opaque pane.
    if (albedo.a < 0.5) {
        discard;
    }
    // Nothing is culled, and a model's thin parts - a wing's skin, a
    // control surface - are seen from behind: there the surface faces the
    // other way, and is lit as the side the eye is on.
    vec3 n = normalize(facing);
    if (!gl_FrontFacing) {
        n = -n;
    }
    float lit = v.sun.w + (1.0 - v.sun.w) * max(dot(n, v.sun.xyz), 0.0);
    float far = clamp((distance(world, v.eye.xyz) - v.range.x) / (v.range.y - v.range.x),
                      0.0, 1.0);

    colour = vec4(mix(albedo.rgb * lit, v.haze.rgb, far), 1.0);
}
