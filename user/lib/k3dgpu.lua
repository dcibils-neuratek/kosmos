-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **Cafesa3D's ray tracer, as a GPU kernel** (`docs/cafesa3d-gpu.md` R5):
-- the 3D Kit's `radiance` (`k3d_trace.c`) in Kosmos's kernel language
-- (`kc.lua`), over the scene `k3d_gpu.c` lays out. One copy, which
-- Cafesa3D compiles and runs and `tools/eu_kernels.lua` holds to the
-- driver's check and, through the emulator, to the processor's picture
-- (`tools/test_k3d_gpu.c`).
--
--   local k3dgpu = use("/Kosmos/Libraries/k3dgpu.lua")
--   local code, info = kc.assemble(k3dgpu.preview, { simd = 8 })
--
-- **preview** is Whitted's, as the processor's Preview: exact mirrors and
-- glass, each lamp's middle for its light and shadow, a little of the sky
-- for what is not bounced. A lane a pixel, SIMD8 - a tracer holds many
-- values at once, and SIMD8 gives each one register rather than two. Its
-- constants are, in this order: the eye, forward, right, up, the focal
-- length, the width and height, the lamps, the top tree's first node, the
-- bounces, the passes so far, the zenith, the horizon, the sky's strength,
-- and the passes this run. Its buffers: nodes, items, objects, triangles,
-- normals and lamps as tables the sampler reads, and the light gathered,
-- three floats a pixel, which each pass adds to. Preview and Final are two
-- kernels on the same helpers (R7a), and both draw the six patterns and
-- their bump (R7b).

local k3dgpu = {}

local COMMON = [[
// Cafesa3D's ray tracer on the GPU: Preview, Whitted's (`docs/cafesa3d-gpu.md`
// R5). The processor's `radiance` in `k3d_trace.c`, step for step, over the
// scene `k3d_gpu.c` lays out - a lane a pixel, the trees walked by their
// skips. The words' places are `k3d_gpu.h`'s.

const vec3 eye, forward, right, up;
const float focal;
const int width, height, nlamps, root, bounces, passes;
const vec3 zenith, horizon;
const float strength;
const int runs;          // passes this run, from `passes` on (18.705)

// The scene in tables, read through the sampler: every ray reads the
// root, the objects and the lamps, and the data port served one thread at
// a time what the sampler's caches serve at once - a hundred times the
// loads a second (`testing.md` 18.705).
table nodes, items, objects, tris, normals, lamps;
buffer acc;

// --- Words ---------------------------------------------------------------

float node_f(int n, int k) { return load(nodes, n * 32 + k * 4); }
int node_i(int n, int k) { return loadi(nodes, n * 32 + k * 4); }
float obj_f(int o, int k) { return load(objects, o * 256 + k * 4); }
int obj_i(int o, int k) { return loadi(objects, o * 256 + k * 4); }
vec3 obj_v(int o, int k) { return load3(objects, o * 256 + k * 4); }
vec3 tri_v(int t, int k) { return load3(tris, t * 48 + k * 4); }

// A point through an affine 3x4 at word `k` of an object's: I, from the world.
// Each takes its object's place once and adds to it, rather than an
// accessor working it out again for every word (`testing.md` 18.702).
vec3 to_own(int o, vec3 p) {
  int at = o * 256;
  vec3 r0 = load3(objects, at + 16); vec3 r1 = load3(objects, at + 32); vec3 r2 = load3(objects, at + 48);
  return vec3(r0.x * p.x + r0.y * p.y + r0.z * p.z + load(objects, at + 28),
              r1.x * p.x + r1.y * p.y + r1.z * p.z + load(objects, at + 44),
              r2.x * p.x + r2.y * p.y + r2.z * p.z + load(objects, at + 60));
}

vec3 to_own_dir(int o, vec3 d) {
  int at = o * 256;
  vec3 r0 = load3(objects, at + 16); vec3 r1 = load3(objects, at + 32); vec3 r2 = load3(objects, at + 48);
  return vec3(r0.x * d.x + r0.y * d.y + r0.z * d.z,
              r1.x * d.x + r1.y * d.y + r1.z * d.z,
              r2.x * d.x + r2.y * d.y + r2.z * d.z);
}

// --- A box of a tree, met before `best` ----------------------------------

bool met_box(int n, vec3 o, vec3 inv, float best) {
  int at = n * 32;
  vec3 lo = load3(nodes, at);
  vec3 hi = load3(nodes, at + 12);
  float x0 = (lo.x - o.x) * inv.x; float x1 = (hi.x - o.x) * inv.x;
  float y0 = (lo.y - o.y) * inv.y; float y1 = (hi.y - o.y) * inv.y;
  float z0 = (lo.z - o.z) * inv.z; float z1 = (hi.z - o.z) * inv.z;
  float tn = max(max(min(x0, x1), min(y0, y1)), max(min(z0, z1), 0.0));
  float tf = min(min(max(x0, x1), max(y0, y1)), min(max(z0, z1), best));
  return tn <= tf;
}

vec3 inverse(vec3 d) {
  float x = abs(d.x) < 1e-20 ? (d.x < 0.0 ? -1e-20 : 1e-20) : d.x;
  float y = abs(d.y) < 1e-20 ? (d.y < 0.0 ? -1e-20 : 1e-20) : d.y;
  float z = abs(d.z) < 1e-20 ? (d.z < 0.0 ? -1e-20 : 1e-20) : d.z;
  return vec3(1.0 / x, 1.0 / y, 1.0 / z);
}

// --- Shapes, in their own space: `shape_hit` -----------------------------

// A mesh: its tree by skips, Moller and Trumbore at each triangle.
void mesh_hit(int o, vec3 ro, vec3 rd, bool any, inout float best, inout bool got,
              inout int tri, inout float bu, inout float bv) {
  vec3 inv = inverse(rd);
  int node = obj_i(o, 2);
  while (node != -1) {
    if (!met_box(node, ro, inv, best)) {
      node = node_i(node, 6);
    } else {
      int leaf = node_i(node, 7);
      if (leaf == 0) {
        node += 1;
      } else {
        int count = loadi(items, leaf * 4 - 4);
        for (int k = 0; k < count; k += 1) {
          int t = loadi(items, (leaf + k) * 4);
          vec3 e1 = tri_v(t, 3); vec3 e2 = tri_v(t, 6);
          vec3 pv = cross(rd, e2);
          float idet = 1.0 / dot(e1, pv);
          vec3 tv = ro - tri_v(t, 0);
          float u = dot(tv, pv) * idet;
          vec3 qv = cross(tv, e1);
          float v = dot(rd, qv) * idet;
          float tt = dot(e2, qv) * idet;
          if (u >= 0.0 && v >= 0.0 && u + v <= 1.0 && tt > 0.0001 && tt < best) {
            best = tt; got = true; tri = t; bu = u; bv = v;
          }
        }
        node = (any && got) ? -1 : node_i(node, 6);
      }
    }
  }
}

// --- The scene: `trace` ------------------------------------------------

void trace(vec3 o, vec3 d, float tmax, bool with_lamps, bool any,
           inout float best, inout int hit, inout int lamp, inout vec3 n,
           inout int tri, inout float bu, inout float bv) {
  best = tmax; hit = -1; lamp = -1;
  vec3 inv = inverse(d);
  int node = root;
  while (node != -1) {
    if (!met_box(node, o, inv, best)) {
      node = node_i(node, 6);
    } else {
      int leaf = node_i(node, 7);
      if (leaf == 0) {
        node += 1;
      } else {
        int count = loadi(items, leaf * 4 - 4);
        for (int k = 0; k < count; k += 1) {
          int ob = loadi(items, (leaf + k) * 4);
          if (!(any && obj_f(ob, 33) > 0.5)) {
            vec3 ro = to_own(ob, o);
            vec3 rd = to_own_dir(ob, d);
            int shape = obj_i(ob, 0);
            float a = obj_f(ob, 25); float b = obj_f(ob, 26); float c = obj_f(ob, 27);
            bool got = false;
            float t = best;
            vec3 nn = vec3(0.0, 0.0, 0.0);
            int ttri = -1; float tu = 0.0; float tv = 0.0;
            if (shape == 1) {                                      // a sphere
              float qa = dot(rd, rd); float qb = dot(ro, rd); float qc = dot(ro, ro) - a * a;
              float disc = qb * qb - qa * qc;
              if (disc >= 0.0) {
                float s = sqrt(disc);
                float tt = (-qb - s) / qa;
                if (tt < 0.0001) { tt = (-qb + s) / qa; }
                if (tt >= 0.0001 && tt < best) { t = tt; nn = ro + rd * tt; got = true; }
              }
            } else if (shape == 2) {                               // a box
              vec3 half = vec3(a, b, c);
              float t0 = -1e30; float t1 = 1e30; int a0 = 0; int a1 = 0; bool out = false;
              for (int ax = 0; ax < 3; ax += 1) {
                float rdk = ax == 0 ? rd.x : (ax == 1 ? rd.y : rd.z);
                float rok = ax == 0 ? ro.x : (ax == 1 ? ro.y : ro.z);
                float hk = ax == 0 ? half.x : (ax == 1 ? half.y : half.z);
                if (abs(rdk) < 1e-12) {
                  if (abs(rok) > hk) { out = true; }
                } else {
                  float ta = (-hk - rok) / rdk; float tb = (hk - rok) / rdk;
                  if (ta > tb) { float sw = ta; ta = tb; tb = sw; }
                  if (ta > t0) { t0 = ta; a0 = ax; }
                  if (tb < t1) { t1 = tb; a1 = ax; }
                }
              }
              if (!out && !(t0 > t1 || t1 < 0.0001)) {
                bool inside = t0 < 0.0001;
                float tt = inside ? t1 : t0;
                if (tt < best) {
                  int kk = inside ? a1 : a0;
                  float rdk = kk == 0 ? rd.x : (kk == 1 ? rd.y : rd.z);
                  float sgn = ((rdk > 0.0) == inside) ? 1.0 : -1.0;
                  t = tt; got = true;
                  nn = vec3(kk == 0 ? sgn : 0.0, kk == 1 ? sgn : 0.0, kk == 2 ? sgn : 0.0);
                }
              }
            } else if (shape == 3) {                               // a cylinder
              float A = rd.x * rd.x + rd.y * rd.y; float B = ro.x * rd.x + ro.y * rd.y;
              float C = ro.x * ro.x + ro.y * ro.y - a * a;
              if (A > 1e-12) {
                float D = B * B - A * C;
                if (D >= 0.0) {
                  float s = sqrt(D);
                  float ta = (-B - s) / A; float tb = (-B + s) / A;
                  float za = ro.z + rd.z * ta; float zb = ro.z + rd.z * tb;
                  if (ta > 0.0001 && ta < t && za >= -b && za <= b) {
                    t = ta; nn = vec3(ro.x + rd.x * ta, ro.y + rd.y * ta, 0.0); got = true;
                  } else if (tb > 0.0001 && tb < t && zb >= -b && zb <= b) {
                    t = tb; nn = vec3(ro.x + rd.x * tb, ro.y + rd.y * tb, 0.0); got = true;
                  }
                }
              }
              if (abs(rd.z) > 1e-12) {
                for (int k2 = 0; k2 < 2; k2 += 1) {
                  float zc = k2 == 1 ? b : -b;
                  float tt = (zc - ro.z) / rd.z;
                  float x = ro.x + rd.x * tt; float y = ro.y + rd.y * tt;
                  if (tt > 0.0001 && tt < t && x * x + y * y <= a * a) {
                    t = tt; nn = vec3(0.0, 0.0, k2 == 1 ? 1.0 : -1.0); got = true;
                  }
                }
              }
            } else if (shape == 4) {                               // a plane
              if (abs(rd.z) >= 1e-12) {
                float tt = -ro.z / rd.z;
                float x = ro.x + rd.x * tt; float y = ro.y + rd.y * tt;
                if (tt >= 0.0001 && tt < best && abs(x) <= a && abs(y) <= a) {
                  t = tt; nn = vec3(0.0, 0.0, 1.0); got = true;
                }
              }
            } else {                                               // a mesh
              mesh_hit(ob, ro, rd, any, t, got, ttri, tu, tv);
              if (got) { nn = cross(tri_v(ttri, 3), tri_v(ttri, 6)); }
            }
            if (got && t < best) {
              best = t; hit = ob; n = nn; tri = ttri; bu = tu; bv = tv;
            }
          }
        }
        node = (any && hit != -1) ? -1 : node_i(node, 6);
      }
    }
  }

  if (with_lamps) {
    for (int i = 0; i < nlamps; i += 1) {
      vec3 lp = load3(lamps, i * 32);
      float lr = load(lamps, i * 32 + 12);
      vec3 oc = o - lp;
      float qa = dot(d, d); float qb = dot(oc, d); float qc = dot(oc, oc) - lr * lr;
      float disc = qb * qb - qa * qc;
      if (disc >= 0.0) {
        float tt = (-qb - sqrt(disc)) / qa;
        if (tt > 0.0001 && tt < best) { best = tt; hit = -1; lamp = i; }
      }
    }
  }
}

// --- Light ---------------------------------------------------------------

vec3 sky(vec3 d) {
  float z = d.z / sqrt(dot(d, d));
  if (z < 0.0) { return horizon * (strength * 0.6); }
  return (horizon + (zenith - horizon) * (z > 0.0 ? pow(z, 0.55) : 0.0)) * strength;
}

vec3 lamp_radiance(int i) {
  float r = load(lamps, i * 32 + 12);
  float rr = r > 0.001 ? r : 0.001;
  return load3(lamps, i * 32 + 16) * (load(lamps, i * 32 + 28) / (4.0 * 3.14159265358979 * 3.14159265358979 * rr * rr));
}

// The world's unit normal at a hit: smooth or flat, out through N.
vec3 world_normal(int ob, vec3 n, int tri, float bu, float bv) {
  vec3 m = n;
  if (obj_i(ob, 0) == 0 && obj_i(ob, 1) == 1) {
    int at = tri * 48;
    int c0 = loadi(tris, at + 36);
    int c1 = loadi(tris, at + 40);
    int c2 = loadi(tris, at + 44);
    vec3 s = load3(normals, c0 * 12) * (1.0 - bu - bv) + load3(normals, c1 * 12) * bu
             + load3(normals, c2 * 12) * bv;
    if (dot(s, n) > 0.0) { m = s; }
  }
  vec3 r0 = obj_v(ob, 16); vec3 r1 = obj_v(ob, 19); vec3 r2 = obj_v(ob, 22);
  return normalize(vec3(r0.x * m.x + r0.y * m.y + r0.z * m.z,
                        r1.x * m.x + r1.y * m.y + r1.z * m.z,
                        r2.x * m.x + r2.y * m.y + r2.z * m.z));
}

// An integer hash, Chris Wellons's lowbias32: a pass's place in its pixel.
// `>>` keeps the sign here, so each shift is masked to what a logical one keeps.
int mix(int v) {
  int x = v;
  x = x ^ ((x >> 16) & 0xffff);
  x = x * 0x7feb352d;
  x = x ^ ((x >> 15) & 0x1ffff);
  x = x * 0x846ca68b;
  x = x ^ ((x >> 16) & 0xffff);
  return x;
}


// Two directions square to `n` and each other, as `basis` makes them.
void basis(vec3 n, inout vec3 a, inout vec3 b) {
  float sg = n.z >= 0.0 ? 1.0 : -1.0;
  float k = -1.0 / (sg + n.z);
  float m = n.x * n.y * k;
  a = vec3(1.0 + sg * n.x * n.x * k, sg * m, -sg * n.x);
  b = vec3(m, sg + n.y * n.y * k, -n.y);
}

// --- Textures: k3d_texture.c's patterns ---------------------------------
//
// R7b (`cafesa3d-gpu.md`): the six patterns and their bump, arithmetic for
// arithmetic as the processor works them, so textured Preview is the
// processor's picture pixel for pixel. An object's pattern is word 36, its
// parameters 37 to 46 and its scale 47 to 49 (`k3d_gpu.h`).

// `hash3`, on 32-bit words: `>>` keeps the sign here, so each shift is
// masked to what a logical one keeps.
int hash3(int x, int y, int z) {
  int h = (x * 0x8da6b343) ^ (y * 0xd8163841) ^ (z * 0xcb1ab31f);
  h = h ^ ((h >> 13) & 0x7ffff);
  h = h * 0x5bd1e995;
  return h ^ ((h >> 15) & 0x1ffff);
}

// `h % 12` of an unsigned word: kc divides by powers of two only, so by its
// halves - 65536 is four more than a multiple of twelve - and in floats,
// which hold the sum exactly.
int mod12(int h) {
  float v = float(((h >> 16) & 0xffff) * 4 + (h & 0xffff));
  return int(v - 12.0 * floor(v / 12.0));
}

// Perlin's twelve directions, the edges of a cube, as `grad`'s cases.
float grad(int h, float x, float y, float z) {
  int k = mod12(h);
  float u = k < 8 ? x : y;
  float v = k < 4 ? y : z;
  return ((k & 1) == 1 ? -u : u) + ((k & 2) == 2 ? -v : v);
}

float fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }

float noise1(float x, float y, float z) {
  int X = int(floor(x)); int Y = int(floor(y)); int Z = int(floor(z));
  float fx = x - float(X); float fy = y - float(Y); float fz = z - float(Z);
  // A corner a statement: kc holds a call's arguments to its statement's
  // end, and the eight in one expression were more than a thread holds.
  float g000 = grad(hash3(X, Y, Z), fx, fy, fz);
  float g100 = grad(hash3(X + 1, Y, Z), fx - 1.0, fy, fz);
  float g010 = grad(hash3(X, Y + 1, Z), fx, fy - 1.0, fz);
  float g110 = grad(hash3(X + 1, Y + 1, Z), fx - 1.0, fy - 1.0, fz);
  float g001 = grad(hash3(X, Y, Z + 1), fx, fy, fz - 1.0);
  float g101 = grad(hash3(X + 1, Y, Z + 1), fx - 1.0, fy, fz - 1.0);
  float g011 = grad(hash3(X, Y + 1, Z + 1), fx, fy - 1.0, fz - 1.0);
  float g111 = grad(hash3(X + 1, Y + 1, Z + 1), fx - 1.0, fy - 1.0, fz - 1.0);
  float u = fade(fx); float v = fade(fy); float w = fade(fz);
  return lerp(lerp(lerp(g000, g100, u), lerp(g010, g110, u), v),
              lerp(lerp(g001, g101, u), lerp(g011, g111, u), v), w);
}

// Octaves, each twice as fine and half as strong: Blender's Detail.
float noise(vec3 p, float detail) {
  float sum = 0.0; float amp = 1.0; float nrm = 0.0; float f = 1.0;
  int octaves = 1 + int(min(max(detail, 0.0), 8.0));
  for (int o = 0; o < octaves; o += 1) {
    sum = sum + amp * noise1(p.x * f, p.y * f, p.z * f);
    nrm = nrm + amp;
    amp = amp * 0.5;
    f = f * 2.0;
  }
  return sum / nrm;
}

float clamp01(float x) { return min(max(x, 0.0), 1.0); }
float frac1(float x) { return x - floor(x); }

// The pattern at `q`, in the object's own metres, on a surface facing `nl`
// in its own space: how far towards the second colour, and how high.
void pattern(int oat, vec3 q, vec3 nl, inout float fac, inout float height) {
  int kind = loadi(objects, oat + 144);
  float scale = load(objects, oat + 160);
  float detail = load(objects, oat + 164);
  float distortion = load(objects, oat + 168);
  float s = scale > 0.0 ? scale : 1.0;
  fac = 0.0;
  height = 0.0;
  if (kind == 1) {                                          // checker
    int c = int(floor(q.x * s)) + int(floor(q.y * s)) + int(floor(q.z * s));
    fac = float(c & 1);
    height = fac;
  } else if (kind == 2 || kind == 3) {                      // brick, shingles
    float H = 1.0 / max(scale, 0.001);
    float W = H * max(load(objects, oat + 180), 0.1);
    bool flr = abs(nl.z) > 0.7;
    float u = flr ? q.x : q.x + q.y;
    float v = flr ? q.y : q.z;
    float row = floor(v / H);
    float fv = frac1(v / H);
    float along = u / W + row * load(objects, oat + 184);
    float col = floor(along);
    float fu = frac1(along);
    float du = min(fu, 1.0 - fu) * W;
    float dv = min(fv, 1.0 - fv) * H;
    float m = load(objects, oat + 176) * H / 2.0;
    float edge = min(du, dv);
    int id = hash3(int(col), int(row), 7);
    if (kind == 3) {
      fac = du < m ? 1.0 : float(id & 255) / 255.0 * 0.35;
      height = du < m ? 0.0 : 1.0 - fv;
    } else if (edge < m) {
      fac = 1.0;
      height = 0.0;
    } else {
      fac = float(id & 255) / 255.0 * 0.3 - 0.15;
      float t = clamp01((edge - m) / (H * 0.12));
      height = t * t * (3.0 - 2.0 * t);
    }
  } else if (kind >= 4) {                                   // noise, wood, marble
    // One noise for the three, at the place each takes it.
    vec3 at = kind == 5 ? vec3(q.x * 2.0, q.y * 2.0, q.z * 0.25) : q * s;
    float nz = noise(at, detail);
    if (kind == 4) {
      fac = clamp01(0.5 + nz * (1.0 + distortion));
      height = fac;
    } else if (kind == 5) {
      float r = sqrt(q.x * q.x + q.y * q.y) * s + distortion * nz;
      float ring = 0.5 + 0.5 * sin(6.28318530 * r);
      fac = ring * ring * ring;
      height = 1.0 - fac;
    } else {
      float vein = 0.5 + 0.5 * sin(q.x * s * 1.5 + distortion * 6.0 * nz);
      fac = pow(vein, 6.0);
      height = 0.0;
    }
  }
}

// A textured surface's colour at `p`, and `n` bent by the pattern's height:
// `textured`, with the four looks two millimetres either side taken in the
// one loop as the first, so the pattern is written out once.
vec3 textured(int oat, vec3 p, inout vec3 n) {
  // Held as little as can be, since this is written out where a hit is
  // shaded with everything else alive (18.707): the object's rows and
  // scale read again where they are used, the bump's directions made
  // again, and its four looks kept as two differences.
  vec3 sc = load3(objects, oat + 188);
  vec3 nl = normalize(vec3(dot(load3(objects, oat + 16), n) * sc.x * sc.x,
                           dot(load3(objects, oat + 32), n) * sc.y * sc.y,
                           dot(load3(objects, oat + 48), n) * sc.z * sc.z));
  float fac = 0.0; float gx = 0.0; float gy = 0.0;
  int looks = load(objects, oat + 172) > 0.0 ? 5 : 1;
  for (int k = 0; k < looks; k += 1) {
    vec3 at = p;
    if (k > 0) {
      vec3 ba = vec3(0.0, 0.0, 0.0); vec3 bb = vec3(0.0, 0.0, 0.0);
      basis(n, ba, bb);
      at = k == 1 ? p + ba * 0.002 : (k == 2 ? p - ba * 0.002 : (k == 3 ? p + bb * 0.002 : p - bb * 0.002));
    }
    vec3 q = vec3(dot(load3(objects, oat + 16), at) + load(objects, oat + 28),
                  dot(load3(objects, oat + 32), at) + load(objects, oat + 44),
                  dot(load3(objects, oat + 48), at) + load(objects, oat + 60)) * load3(objects, oat + 188);
    float f = 0.0; float h = 0.0;
    pattern(oat, q, nl, f, h);
    if (k == 0) { fac = f; }
    if (k == 1) { gx = h; }
    if (k == 2) { gx = gx - h; }
    if (k == 3) { gy = h; }
    if (k == 4) { gy = gy - h; }
  }
  if (looks > 1) {
    vec3 ba = vec3(0.0, 0.0, 0.0); vec3 bb = vec3(0.0, 0.0, 0.0);
    basis(n, ba, bb);
    vec3 g = ba * (gx / 0.004) + bb * (gy / 0.004);
    n = normalize(n - g * load(objects, oat + 172));
  }
  vec3 base = load3(objects, oat + 112);
  vec3 c = base + (load3(objects, oat + 148) - base) * fac;
  return vec3(max(c.x, 0.0), max(c.y, 0.0), max(c.z, 0.0));
}
]]

k3dgpu.preview = COMMON .. [[
// --- A pixel -------------------------------------------------------------

void main() {
  int x = group_x * simd + lane;
  int y = group_y;
  if (x < width) {
   // Several passes a run, summed here and added to the light once: a run
   // is a wait for the caller's next look, and one pass a run left the GPU
   // idle three quarters of the time (`testing.md` 18.705).
   vec3 total = vec3(0.0, 0.0, 0.0);
   for (int q = 0; q < runs; q += 1) {
    int pass = passes + q;
    // The pixel's middle on the first pass, anywhere in it after - which is
    // what smooths the edges - from a hash of the pixel and the pass.
    float jx = 0.5;
    float jy = 0.5;
    if (pass > 0) {
      int h = mix(x * 1973 + y * 9277 + pass * 26699);
      jx = float((h >> 8) & 0xffffff) * (1.0 / 16777216.0);
      h = mix(h);
      jy = float((h >> 8) & 0xffffff) * (1.0 / 16777216.0);
    }
    vec3 o = eye;
    vec3 d = normalize(forward * focal + right * (float(x) + jx - float(width) / 2.0)
                       - up * (float(y) + jy - float(height) / 2.0));
    vec3 col = vec3(0.0, 0.0, 0.0);
    vec3 thr = vec3(1.0, 1.0, 1.0);
    bool specular = true;
    bool going = true;
    for (int bounce = 0; bounce <= bounces && going; bounce += 1) {
      float best = 0.0; int hit = -1; int lamp = -1; vec3 hn = vec3(0.0, 0.0, 0.0);
      int tri = -1; float bu = 0.0; float bv = 0.0;
      trace(o, d, 1e30, bounce > 0, false, best, hit, lamp, hn, tri, bu, bv);
      if (hit == -1 && lamp == -1) {
        col = col + thr * sky(d);
        going = false;
      } else if (lamp != -1) {
        if (specular) { col = col + thr * lamp_radiance(lamp); }
        going = false;
      } else {
        vec3 p = o + d * best;
        vec3 n = world_normal(hit, hn, tri, bu, bv);
        vec3 base = obj_v(hit, 28);
        if (obj_i(hit, 36) != 0) { base = textured(hit * 256, p, n); }
        float emit = obj_f(hit, 35);
        if (emit > 0.0) {
          col = col + thr * base * emit;
          going = false;
        } else if (obj_f(hit, 33) > 0.5) {
          float ior = obj_f(hit, 34);
          bool into = dot(d, n) < 0.0;
          vec3 nn = into ? n : -n;
          float eta = into ? 1.0 / ior : ior;
          float cosi = -dot(d, nn);
          float k = 1.0 - eta * eta * (1.0 - cosi * cosi);
          float r0 = (1.0 - ior) / (1.0 + ior);
          r0 = r0 * r0;
          float R = k < 0.0 ? 1.0 : r0 + (1.0 - r0) * pow(1.0 - cosi, 5.0);
          if (R >= 0.5) {
            d = d - nn * (2.0 * dot(d, nn));
            o = p + nn * 0.001;
          } else {
            d = normalize(d * eta + nn * (eta * cosi - sqrt(k)));
            o = p - nn * 0.001;
            thr = thr * base;
          }
          specular = true;
        } else {
          if (dot(n, d) > 0.0) { n = -n; }
          if (obj_f(hit, 31) > 0.5) {
            vec3 refl = d - n * (2.0 * dot(d, n));
            d = refl;
            if (dot(d, n) <= 0.0) {
              going = false;
            } else {
              thr = thr * base;
              o = p + n * 0.001;
              specular = true;
            }
          } else {
            // Whitted's direct light: each lamp's middle, if nothing is in
            // the way - the shadow rays this pass's hit's own variables.
            vec3 sum = vec3(0.0, 0.0, 0.0);
            vec3 start = p + n * 0.001;
            for (int i = 0; i < nlamps; i += 1) {
              vec3 to = load3(lamps, i * 32) - p;
              float rr = load(lamps, i * 32 + 12);
              float d2 = dot(to, to);
              float dist = sqrt(d2);
              vec3 w = to * (1.0 / dist);
              float c = dot(w, n);
              if (c > 0.0) {
                trace(start, w, dist - rr, false, true, best, hit, lamp, hn, tri, bu, bv);
                if (hit == -1) {
                  sum = sum + lamp_radiance(i) * (c * 3.14159265358979 * rr * rr / d2);
                }
              }
            }
            col = col + thr * base * (sum * (1.0 / 3.14159265358979));
            col = col + thr * base * sky(vec3(0.0, 0.0, 1.0)) * 0.35;
            going = false;
          }
        }
      }
    }
    total = total + vec3(min(col.x, 40.0), min(col.y, 40.0), min(col.z, 40.0));
   }
    int at = (y * width + x) * 12;
    vec3 had = load3(acc, at);
    store3(acc, at, had + total);
  }
}
]]

-- **Final**, path tracing, on the same helpers (`cafesa3d-gpu.md` R7a).
local FINALH = [[
// --- Final: random numbers, and the lobes ------------------------------
//
// R7a (`cafesa3d-gpu.md`): the processor's Final, lobe for lobe, with
// random numbers of the GPU's own - a counter through `mix` - so the two
// pictures agree within their noise and not draw for draw.

// A number in [0, 1): the lane's counter moved on and hashed.
float rnd(inout int s) {
  s = s + 0x9e3779b9;
  return float((mix(s) >> 8) & 0xffffff) * (1.0 / 16777216.0);
}

// atan on any number: a minimax polynomial on [0, 1], about 1e-5 out,
// and pi/2 less it beyond one. The glossy lobe's sizes want no better.
float atan1(float x) {
  float a = abs(x);
  float t = a > 1.0 ? 1.0 / a : a;
  float t2 = t * t;
  float r = t * (0.99997726 + t2 * (-0.33262347 + t2 * (0.19354346 + t2 * (-0.11643287
            + t2 * (0.05265332 + t2 * -0.0117212)))));
  float q = a > 1.0 ? 1.5707963268 - r : r;
  return x < 0.0 ? -q : q;
}

float asin1(float x) {
  float c = sqrt(max(0.0, 1.0 - x * x));
  float a = abs(x);
  float r = a <= c ? atan1(a / c) : 1.5707963268 - atan1(c / max(a, 1e-30));
  return x < 0.0 ? -r : r;
}

float acos1(float x) { return 1.5707963268 - asin1(x); }

// A direction near `dir`, spread by `amount`: the processor's point in a
// ball, here drawn straight rather than by rejection, which lanes would
// wait on.
vec3 fuzz(inout int s, vec3 dir, float amount) {
  float z = rnd(s) * 2.0 - 1.0;
  float ph = 6.28318530718 * rnd(s);
  float r = pow(rnd(s), 0.33333333);
  float sz = sqrt(max(0.0, 1.0 - z * z)) * r;
  vec3 pt = vec3(sz * cos(ph), sz * sin(ph), z * r);
  return amount > 0.0 ? normalize(dir + pt * amount) : dir;
}

// Light from a path that has bounced off something rough, held to four:
// Cycles' indirect clamp, as `held` does it.
vec3 held(vec3 c, bool bounced) {
  float most = max(c.x, max(c.y, c.z));
  return bounced && most > 4.0 ? c * (4.0 / most) : c;
}

]]

k3dgpu.final = COMMON .. FINALH .. [[
// --- A pixel, Final --------------------------------------------------------

void main() {
  int x = group_x * simd + lane;
  int y = group_y;
  if (x < width) {
   vec3 total = vec3(0.0, 0.0, 0.0);
   for (int q = 0; q < runs; q += 1) {
    int pass = passes + q;
    float jx = 0.5;
    float jy = 0.5;
    if (pass > 0) {
      int h = mix(x * 1973 + y * 9277 + pass * 26699);
      jx = float((h >> 8) & 0xffffff) * (1.0 / 16777216.0);
      h = mix(h);
      jy = float((h >> 8) & 0xffffff) * (1.0 / 16777216.0);
    }
    int s = mix(x * 7919 + y * 104729 + pass * 1299709 + 0x2545f491);
    vec3 o = eye;
    vec3 d = normalize(forward * focal + right * (float(x) + jx - float(width) / 2.0)
                       - up * (float(y) + jy - float(height) / 2.0));
    vec3 col = vec3(0.0, 0.0, 0.0);
    vec3 thr = vec3(1.0, 1.0, 1.0);
    bool specular = true;
    bool bounced = false;
    bool going = true;
    for (int bounce = 0; bounce <= bounces && going; bounce += 1) {
      float best = 0.0; int hit = -1; int lamp = -1; vec3 hn = vec3(0.0, 0.0, 0.0);
      int tri = -1; float bu = 0.0; float bv = 0.0;
      trace(o, d, 1e30, bounce > 0, false, best, hit, lamp, hn, tri, bu, bv);
      if (hit == -1 && lamp == -1) {
        col = col + held(thr * sky(d), bounced);
        going = false;
      } else if (lamp != -1) {
        if (specular) { col = col + held(thr * lamp_radiance(lamp), bounced); }
        going = false;
      } else {
        // What is kept across the shadow rays is only what is used after
        // them, and the rest is worked out in blocks that end first, since
        // kc keeps a variable until its block ends (18.706). So `o` becomes
        // the point hit, a lamp's light is worked out after its shadow ray,
        // and the base colour is read again where it is wanted.
        vec3 n = world_normal(hit, hn, tri, bu, bv);
        int oat = hit * 256;
        float amount = load(objects, oat + 128);
        amount = amount * amount * 2.0;
        o = o + d * best;
        // The colour here: the base, or the pattern's (R7b), which bends n.
        vec3 base = load3(objects, oat + 112);
        if (loadi(objects, oat + 144) != 0) { base = textured(oat, o, n); }
        if (load(objects, oat + 140) > 0.0) {
          col = col + held(thr * base * load(objects, oat + 140), bounced);
          going = false;
        } else if (load(objects, oat + 132) > 0.5) {
          // Glass: reflected or refracted by Fresnel's odds.
          float ior = load(objects, oat + 136);
          bool into = dot(d, n) < 0.0;
          vec3 nn = into ? n : -n;
          float eta = into ? 1.0 / ior : ior;
          float cosi = -dot(d, nn);
          float k = 1.0 - eta * eta * (1.0 - cosi * cosi);
          float r0 = (1.0 - ior) / (1.0 + ior);
          r0 = r0 * r0;
          float R = k < 0.0 ? 1.0 : r0 + (1.0 - r0) * pow(1.0 - cosi, 5.0);
          if (rnd(s) < R) {
            d = d - nn * (2.0 * dot(d, nn));
            o = o + nn * 0.001;
          } else {
            d = normalize(d * eta + nn * (eta * cosi - sqrt(max(k, 0.0))));
            o = o - nn * 0.001;
            thr = thr * base;
          }
          d = fuzz(s, d, amount);
          specular = true;
        } else {
          if (dot(n, d) > 0.0) { n = -n; }
          o = o + n * 0.001;
          bool metal = load(objects, oat + 124) > 0.5;
          // amount >= 1.62 is a roughness of 0.9 or more: no clear coat.
          float F = metal ? 1.0 : (amount >= 1.62 ? 0.0 : 0.04 + 0.96 * pow(max(0.0, 1.0 + dot(d, n)), 5.0));
          vec3 light = vec3(0.0, 0.0, 0.0);
          // Every lamp twice through one shadow ray: the matte lobe's sample
          // (`direct`), then the glossy lobe's share (`glossy`) - one trace
          // for both, as kc writes each call out in full.
          for (int qn = 0; qn < nlamps * 2; qn += 1) {
            vec3 dir = vec3(0.0, 0.0, 1.0);
            float tmax = 0.0;
            bool want = false;
            {
              int i = qn >> 1;
              vec3 to = load3(lamps, i * 32) - o;
              float rr = load(lamps, i * 32 + 12);
              float dist = sqrt(dot(to, to));
              vec3 w = to * (1.0 / dist);
              tmax = dist - rr;
              dir = w;
              if ((qn & 1) == 0) {
                if (!metal) {
                  if (rr <= 0.0 || rr >= dist) {
                    want = dot(w, n) > 0.0;
                  } else {
                    float cosmax = sqrt(max(0.0, 1.0 - rr * rr / (dist * dist)));
                    float ct = 1.0 - rnd(s) * (1.0 - cosmax);
                    float st = sqrt(max(0.0, 1.0 - ct * ct));
                    float ph = 6.28318530718 * rnd(s);
                    vec3 ba = vec3(0.0, 0.0, 0.0); vec3 bb = vec3(0.0, 0.0, 0.0);
                    basis(w, ba, bb);
                    dir = ba * (cos(ph) * st) + bb * (sin(ph) * st) + w * ct;
                    want = dot(dir, n) > 0.0;
                  }
                }
              } else {
                want = amount >= 0.02 && F > 0.0 && dot(w, n) > 0.0 && rr < dist;
              }
            }
            if (want) {
              trace(o, dir, tmax, false, true, best, hit, lamp, hn, tri, bu, bv);
              if (hit == -1) {
                // The light this ray found, worked out now it is known
                // to arrive.
                int i = qn >> 1;
                vec3 to = load3(lamps, i * 32) - o;
                float rr = load(lamps, i * 32 + 12);
                float d2 = dot(to, to);
                float dist = sqrt(d2);
                vec3 w = to * (1.0 / dist);
                if ((qn & 1) == 0) {
                  float c = dot(dir, n);
                  float cosmax = sqrt(max(0.0, 1.0 - rr * rr / d2));
                  float solid = (rr <= 0.0 || rr >= dist) ? 3.14159265358979 * rr * rr / d2
                                                          : 6.28318530718 * (1.0 - cosmax);
                  light = light + lamp_radiance(i) * base
                          * (c * solid * (1.0 - F) * (1.0 / 3.14159265358979));
                } else {
                  float alpha = atan1(amount);
                  float lobe = 6.28318530718 * (1.0 - cos(alpha));
                  float beta = asin1(rr / dist);
                  float disc = 6.28318530718 * (1.0 - cos(beta));
                  float phi = acos1(min(1.0, max(-1.0, dot(w, d - n * (2.0 * dot(d, n))))));
                  if (phi < alpha + beta) {
                    float share = min(disc, lobe) / lobe;
                    float lo = abs(alpha - beta);
                    if (phi > lo) { share = share * (alpha + beta - phi) / (alpha + beta - lo); }
                    light = light + lamp_radiance(i) * share
                            * (metal ? base : vec3(F, F, F));
                  }
                }
              }
            }
          }
          col = col + held(thr * light, bounced);
          if (metal || rnd(s) < F) {
            // On along the reflection, spread by the roughness.
            d = fuzz(s, d - n * (2.0 * dot(d, n)), amount);
            if (dot(d, n) <= 0.0) {
              going = false;
            } else {
              if (metal) { thr = thr * base; }
              specular = amount < 0.02;
              bounced = bounced || !specular;
            }
          } else {
            // The matte bounce, by the cosine.
            float ph = 6.28318530718 * rnd(s);
            float r2 = rnd(s);
            float sr = sqrt(r2);
            vec3 ba = vec3(0.0, 0.0, 0.0); vec3 bb = vec3(0.0, 0.0, 0.0);
            basis(n, ba, bb);
            d = ba * (cos(ph) * sr) + bb * (sin(ph) * sr) + n * sqrt(1.0 - r2);
            thr = thr * base;
            specular = false;
            bounced = true;
            if (bounce >= 2) {
              float qq = min(0.95, max(0.08, max(thr.x, max(thr.y, thr.z))));
              if (rnd(s) > qq) { going = false; } else { thr = thr * (1.0 / qq); }
            }
          }
        }
      }
    }
    total = total + vec3(min(col.x, 40.0), min(col.y, 40.0), min(col.z, 40.0));
   }
    int at = (y * width + x) * 12;
    vec3 had = load3(acc, at);
    store3(acc, at, had + total);
  }
}
]]

--
-- **paint** turns the light a render has gathered into the picture's
-- colours, as the processor's `k3d_render_paint` does: the light over the
-- passes, the same filmic curve, then the paint's own sRGB table, looked up
-- rather than worked out, so a colour is the processor's to the byte where
-- the light is. Constants: the width, height, the picture's pitch in
-- pixels, the passes. Buffers: the light, the table, the picture as bytes.
--
k3dgpu.paint = [[
const int width, height, pitch, count;

buffer acc;
table curve;
buffer picture;

int level(float v) {
  float c = min(max(v, 0.0), 1.0);
  return loadi(curve, int(c * 8191.0 + 0.5) * 4);
}

void main() {
  int x = group_x * simd + lane;
  int y = group_y;
  if (x < width) {
    vec3 c = load3(acc, (y * width + x) * 12) * (1.0 / float(count));
    vec3 v = c * 0.9;
    vec3 t = (v * (v * 2.51 + 0.03)) / (v * (v * 2.43 + 0.59) + 0.14);
    store(picture, (y * pitch + x) * 4,
          0xff000000 | (level(t.x) << 16) | (level(t.y) << 8) | level(t.z));
  }
}
]]

--------------------------------------------------------------------------
-- **A render on the GPU**, with the processor's render's door - `paint`,
-- `passes`, `rays`, `workers`, `stop` - so Cafesa3D asks it the same
-- questions (`docs/cafesa3d-gpu.md` R6): Preview, Whitted's, or Final, path
-- tracing (R7a), as the integrator asks; `gpu` says whose it is.
--
--   local job, why = k3dgpu.render(k3, scene, how)
--
-- Nothing here waits for the GPU: each question takes a step - a pass
-- finished is painted into the picture, the next pass started - and
-- answers from what has already happened, on the caller's clock.
--------------------------------------------------------------------------

local compiled = {}

local function kernel(compute, kc, name)
  if not compiled[name] then
    local text, info = kc.compile(k3dgpu[name], { simd = 8 })

    if not text then return nil, info end

    local k, why = compute.kernel(text)

    if not k then return nil, why end

    compiled[name] = { kernel = k, info = info }
  end

  return compiled[name]
end

local job = {}
job.__index = job

local PARTS = { "nodes", "items", "objects", "tris", "normals", "lamps" }

function k3dgpu.render(k3, scene, how)
  local compute = use("/Kosmos/Kits/compute")
  local kc = use("/Kosmos/Libraries/kc.lua")
  local ok, why = compute.available()

  if not ok then return nil, why end

  -- Preview's kernel or Final's, as asked (`cafesa3d-gpu.md` R7a).
  local tracer, twhy = kernel(compute, kc, how.preview and "preview" or "final")
  if not tracer then return nil, "the tracer's kernel: " .. tostring(twhy) end

  local paint, pwhy = kernel(compute, kc, "paint")
  if not paint then return nil, "the paint's kernel: " .. tostring(pwhy) end

  local wanted = {}
  for k, v in pairs(how) do wanted[k] = v end

  local g, gwhy = k3.gpu_scene(scene, wanted)
  if not g then return nil, gwhy end

  local sizes = g:sizes()
  local self = setmetatable({ gpu = true, w = how.w, h = how.h,
                              target = how.passes or 256, count = 0, painted = 0,
                              gpu_ns = 0, trace_ns = 0, paint_ns = 0, buffers = {},
                              camera = g:camera(),
                              tracer = tracer, paint_k = paint, compute = compute, kc = kc }, job)

  for _, part in ipairs(PARTS) do
    local b, bwhy = compute.buffer(math.max(4, sizes[part]))

    if not b then self:stop() return nil, bwhy end
    g:copy(part, b)
    self.buffers[#self.buffers + 1] = b
  end

  local curve, cwhy = compute.buffer(sizes.curve)
  if not curve then self:stop() return nil, cwhy end
  g:copy("curve", curve)
  self.curve = curve

  local acc, awhy = compute.buffer(how.w * how.h * 12)
  if not acc then self:stop() return nil, awhy end
  self.acc = acc

  local pic, iwhy = compute.picture(how.w, how.h)
  if not pic then self:stop() return nil, iwhy end
  self.picture, self.surface = pic, pic:surface()

  self.started = sys.ticks()
  self.hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1
  self:step()
  return self
end

-- One step: a run finished is counted, the next begun.
function job:step()
  if self.stopped then return end

  if self.ticket then
    if not self.ticket:done() then return end

    local ns = self.ticket:time() or 0

    self.gpu_ns = self.gpu_ns + ns

    if self.phase == "trace" then
      self.trace_ns = self.trace_ns + ns
    else
      self.paint_ns = self.paint_ns + ns
    end

    if self.phase == "trace" then
      self.count = self.count + self.runs
      self.ticket, self.phase = nil, nil

      -- As many passes next as fill about 100 ms of the GPU's time, judged
      -- by this run's: one pass a run left it idle three quarters of the
      -- time between the caller's looks, and 25 ms a run still half
      -- (18.705). The run waits on nobody, so its length costs the desktop
      -- nothing; the picture is painted between runs.
      local per = ns / self.runs / 1e6

      self.next_runs = math.max(1, math.min(64, per > 0 and math.floor(100 / per) or 1))

      -- Painted a few times a second and at the end, the traces between
      -- run back to back: a paint each pass was a run more and a wait for
      -- the caller's next look, 55 ms a pass on the wall (18.701).
      local hz = self.hz

      if self.count >= self.target or (sys.ticks() - (self.painted_at or 0)) / hz >= 0.25 then
        self:launch_paint()
      else
        self:launch_trace()
      end

      return
    end

    self.ticket, self.phase = nil, nil
    self.fresh = true
    self.painted = self.count
    self.painted_at = sys.ticks()

    -- Where a pass's time went, said once: the GPU's own time for a trace
    -- and a paint, and the wall's for a whole pass.
    if self.painted >= self.target then
      local hz = self.hz
      local n = self.painted

      print(("k3dgpu: %d passes, %dx%d: a trace %.2f ms and a paint %.2f ms on the GPU, "
             .. "a pass %.2f ms on the wall"):format(n, self.w, self.h, self.trace_ns / n / 1e6,
        self.paint_ns / n / 1e6, (sys.ticks() - self.started) / hz / n * 1e3))
    end
  end

  if self.count < self.target then self:launch_trace() end
end

function job:launch_trace()
  local c = self.camera

  self.runs = math.max(1, math.min(self.next_runs or 1, self.target - self.count))
  local constants = self.kc.pack(self.tracer.info, {
    eye = c.eye, forward = c.forward, right = c.right, up = c.up, focal = c.focal,
    width = c.width, height = c.height, nlamps = c.nlamps, root = c.root,
    bounces = c.bounces, passes = self.count, runs = self.runs, zenith = c.zenith, horizon = c.horizon,
    strength = c.strength })
  local b = self.buffers
  local t, why = self.compute.run{ kernel = self.tracer.kernel, simd = 8,
                                   grid = { (self.w + 7) // 8, self.h },
                                   buffers = { { b[1], "table" }, { b[2], "table" }, { b[3], "table" },
                                                { b[4], "table" }, { b[5], "table" }, { b[6], "table" },
                                                self.acc },
                                   constants = constants }

  if t then self.ticket, self.phase = t, "trace" else self.why = why end
end

function job:launch_paint()
  local constants = self.kc.pack(self.paint_k.info, { width = self.w, height = self.h,
                                                      pitch = self.picture.pitch // 4,
                                                      count = self.count })
  local t, why = self.compute.run{ kernel = self.paint_k.kernel, simd = 8,
                                   grid = { (self.w + 7) // 8, self.h },
                                   buffers = { self.acc, { self.curve, "table" }, { self.picture, "bytes" } },
                                   constants = constants }

  if t then self.ticket, self.phase = t, "paint" else self.why = why end
end

-- The picture so far onto `surface` at `x`, `y`: 1 when something new was
-- painted, as the processor's paint counts its tiles.
function job:paint(surface, x, y, all)
  self:step()

  if self.painted == 0 or not (self.fresh or all) then return 0 end

  surface:blit(self.surface, 0, 0, self.w, self.h, x, y)
  self.fresh = false
  return 1
end

function job:passes()
  self:step()
  return self.painted
end

-- Rays from the eye, a pixel's a pass; the shadow rays are not counted.
function job:rays() return self.painted * self.w * self.h end
function job:workers() return 0 end

-- The GPU's own time for every run so far, in seconds.
function job:gpu_seconds() return self.gpu_ns / 1e9 end

function job:stop()
  self.stopped = true

  if self.ticket then self.ticket:wait(2000) end

  for _, b in ipairs(self.buffers or {}) do b:free() end
  if self.curve then self.curve:free() end
  if self.acc then self.acc:free() end
  if self.picture then self.picture:free() end
  self.buffers, self.curve, self.acc, self.picture = {}, nil, nil, nil
  self.ticket = nil
end

return k3dgpu
