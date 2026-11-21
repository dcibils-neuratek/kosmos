// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// What every lit surface's shaders share (`k3d_vk.c`): the view's push
// constants, as `struct mesh_push` lays them, and - in a fragment shader
// that defines K3D_LIT first - the sun's light on a surface, its shadow
// looked up in the sun's map (`docs/flight.md`, *Shadows*).

// Included twice by a fragment shader: first for the members, before its
// push block; then, with K3D_LIT defined, for the functions, which read
// the block as `v`.

// The view, 192 bytes. A model's push adds its own matrix and tint after
// it, so this is a list of members rather than a block.
#ifndef K3D_VIEW_MEMBERS
#define K3D_VIEW_MEMBERS \
    mat4 clip;          /* the world to clip space */ \
    vec4 eye;           /* xyz: the eye; w: a shadow-map texel in uv, 0 none */ \
    vec4 sun;           /* xyz: towards the sun; w: the ambient share */ \
    vec4 haze;          /* rgb: the haze's colour */ \
    vec4 range;         /* x, y: haze start, end; z: shadow depth bias; w: its normal offset, m */ \
    vec4 light;         /* rgb: the sun's colour (F6a) */ \
    vec4 shadow[3];     /* the world to the sun's map, three affine rows */
#endif

#if defined(K3D_LIT) && !defined(K3D_LIT_DONE)
#define K3D_LIT_DONE
// The sun's map: depth from the sun, compared as it is read, so a tap is
// 1 where the surface is nearer the sun than what the map holds.
layout(set = 1, binding = 0) uniform sampler2DShadow sunmap;

// How much of the sun reaches a surface at `world` facing `n`, 0 to 1:
// nine taps a texel apart, so a shadow's edge is soft over two texels.
// Outside the map's box everything is in the sun.
float k3d_sunlit(vec3 world, vec3 n)
{
    if (v.eye.w <= 0.0) {
        return 1.0;
    }
    vec4 q = vec4(world + n * v.range.w, 1.0);
    vec3 s = vec3(dot(v.shadow[0], q), dot(v.shadow[1], q), dot(v.shadow[2], q));
    vec2 uv = s.xy * 0.5 + 0.5;

    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || s.z < 0.0 || s.z > 1.0) {
        return 1.0;
    }
    float sum = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            sum += texture(sunmap, vec3(uv + vec2(x, y) * v.eye.w, s.z - v.range.z));
        }
    }
    return sum / 9.0;
}

// The light on a surface: the ambient share white, and the sun's in its
// colour where the sun reaches.
vec3 k3d_light(vec3 world, vec3 n)
{
    float towards = max(dot(n, v.sun.xyz), 0.0);
    float reached = towards > 0.0 ? k3d_sunlit(world, n) : 0.0;

    return vec3(v.sun.w) + (1.0 - v.sun.w) * towards * reached * v.light.rgb;
}

// Hazed by its distance from the eye.
vec3 k3d_hazed(vec3 world, vec3 lit)
{
    float far = clamp((distance(world, v.eye.xyz) - v.range.x) / (v.range.y - v.range.x),
                      0.0, 1.0);

    return mix(lit, v.haze.rgb, far);
}
#endif
