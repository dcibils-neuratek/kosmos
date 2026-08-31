-- A software 3D engine. The part that is not pixels.
--
-- `CLAUDE.md` names this milestone as the way to decide what belongs in C:
-- write it in Lua, measure, and move only what is slow. So everything that
-- happens once per vertex or once per face is here, and the only thing in C
-- is `surface:triangle`, which happens once per *pixel* and was never in
-- question - `gfx.md` §19.1 forbids a pixel loop in Lua outright.
--
-- The arithmetic to keep in mind: a cube is 12 triangles and a vertex
-- transform is about 30 multiplies, so a frame of scene maths is on the
-- order of a thousand operations. One 200x200 triangle is 20,000 pixels.
-- The ratio is why this split is not a close call, and it is also why
-- moving *this* to C would buy almost nothing.
--
-- Row-vector convention: a point is a row and `p * M` transforms it, so
-- matrices multiply left to right in the order the operations happen -
-- `model * view * projection` reads as what it does. Column vectors would
-- reverse that and every composition would have to be read backwards.

local g3d = {}

-- ------------------------------------------------------------------
-- Matrices, as flat 16-element arrays in row-major order.
--
-- Flat rather than nested: a table of four tables is five allocations and
-- five headers the collector walks, per matrix, per frame. `gfx.md` makes
-- the same argument about pixels, one size down.

local function m_identity()
  return { 1, 0, 0, 0,
           0, 1, 0, 0,
           0, 0, 1, 0,
           0, 0, 0, 1 }
end

g3d.identity = m_identity

-- a * b, written out rather than looped. Sixteen dot products of four terms
-- is the hottest thing here after the rasteriser, and the loop version
-- spends more time on index arithmetic than on the multiplies.
function g3d.multiply(a, b)
  local a1,  a2,  a3,  a4  = a[1],  a[2],  a[3],  a[4]
  local a5,  a6,  a7,  a8  = a[5],  a[6],  a[7],  a[8]
  local a9,  a10, a11, a12 = a[9],  a[10], a[11], a[12]
  local a13, a14, a15, a16 = a[13], a[14], a[15], a[16]

  local b1,  b2,  b3,  b4  = b[1],  b[2],  b[3],  b[4]
  local b5,  b6,  b7,  b8  = b[5],  b[6],  b[7],  b[8]
  local b9,  b10, b11, b12 = b[9],  b[10], b[11], b[12]
  local b13, b14, b15, b16 = b[13], b[14], b[15], b[16]

  return {
    a1*b1 + a2*b5 + a3*b9  + a4*b13,
    a1*b2 + a2*b6 + a3*b10 + a4*b14,
    a1*b3 + a2*b7 + a3*b11 + a4*b15,
    a1*b4 + a2*b8 + a3*b12 + a4*b16,

    a5*b1 + a6*b5 + a7*b9  + a8*b13,
    a5*b2 + a6*b6 + a7*b10 + a8*b14,
    a5*b3 + a6*b7 + a7*b11 + a8*b15,
    a5*b4 + a6*b8 + a7*b12 + a8*b16,

    a9*b1 + a10*b5 + a11*b9  + a12*b13,
    a9*b2 + a10*b6 + a11*b10 + a12*b14,
    a9*b3 + a10*b7 + a11*b11 + a12*b15,
    a9*b4 + a10*b8 + a11*b12 + a12*b16,

    a13*b1 + a14*b5 + a15*b9  + a16*b13,
    a13*b2 + a14*b6 + a15*b10 + a16*b14,
    a13*b3 + a14*b7 + a15*b11 + a16*b15,
    a13*b4 + a14*b8 + a15*b12 + a16*b16,
  }
end

function g3d.translation(x, y, z)
  return { 1, 0, 0, 0,
           0, 1, 0, 0,
           0, 0, 1, 0,
           x, y, z, 1 }
end

function g3d.rotation_x(a)
  local c, s = math.cos(a), math.sin(a)
  return { 1, 0, 0, 0,
           0, c, s, 0,
           0, -s, c, 0,
           0, 0, 0, 1 }
end

function g3d.rotation_y(a)
  local c, s = math.cos(a), math.sin(a)
  return { c, 0, -s, 0,
           0, 1, 0, 0,
           s, 0, c, 0,
           0, 0, 0, 1 }
end

function g3d.rotation_z(a)
  local c, s = math.cos(a), math.sin(a)
  return { c, s, 0, 0,
           -s, c, 0, 0,
           0, 0, 1, 0,
           0, 0, 0, 1 }
end

-- A right-handed look-at, which is the one place a sign error produces a
-- picture that is merely inside out rather than absent, so it is worth
-- reading slowly. `forward` points from the eye toward the target.
function g3d.look_at(eye, target, up)
  local fx, fy, fz = target[1] - eye[1], target[2] - eye[2], target[3] - eye[3]
  local fl = math.sqrt(fx*fx + fy*fy + fz*fz)
  fx, fy, fz = fx / fl, fy / fl, fz / fl

  -- right = up x forward
  local rx = up[2]*fz - up[3]*fy
  local ry = up[3]*fx - up[1]*fz
  local rz = up[1]*fy - up[2]*fx
  local rl = math.sqrt(rx*rx + ry*ry + rz*rz)
  rx, ry, rz = rx / rl, ry / rl, rz / rl

  -- true up = forward x right, recomputed rather than trusted: the `up`
  -- passed in is a hint about roll and is usually not perpendicular.
  local ux = fy*rz - fz*ry
  local uy = fz*rx - fx*rz
  local uz = fx*ry - fy*rx

  local ex, ey, ez = eye[1], eye[2], eye[3]

  return { rx, ux, fx, 0,
           ry, uy, fy, 0,
           rz, uz, fz, 0,
           -(rx*ex + ry*ey + rz*ez),
           -(ux*ex + uy*ey + uz*ez),
           -(fx*ex + fy*ey + fz*ez), 1 }
end

-- Perspective. `fov` is the vertical field of view in radians.
--
-- The w row is what makes it a projection rather than a scale: it copies z
-- into w so the divide that follows shrinks distant things. Everything
-- interesting about perspective is that one 1.
function g3d.perspective(fov, aspect, near, far)
  local f = 1 / math.tan(fov / 2)
  local range = far - near

  return { f / aspect, 0, 0, 0,
           0, f, 0, 0,
           0, 0, far / range, 1,
           0, 0, -(far * near) / range, 0 }
end

-- ------------------------------------------------------------------
-- Meshes.
--
-- Vertices are a flat array of x, y, z triples and faces a flat array of
-- index triples, for the reason the matrices are flat: one table each
-- instead of one per vertex. A 500-triangle mesh as tables-of-tables is
-- 2,000 objects for the collector to walk every cycle, and `gc_pause_max`
-- is baselined and would say so.

-- Winding, fixed by construction rather than by hand.
--
-- A face is drawn when its screen-space cross product is negative, which is
-- the test in `render` and is nothing but "this face is turned toward the
-- camera". For that to mean anything, every face of a mesh has to be wound
-- the same way round, and writing twelve triangles out by hand and getting
-- all of them consistent is a coin flip - the first version of the cube
-- below had four of the six backwards, which shows on screen as a fourth
-- face appearing at angles where a cube can only have three.
--
-- So it is computed. For each triangle, the right-handed normal
-- (p2-p1) x (p3-p1) either points away from the centre of the mesh or
-- toward it, and the two cases are exactly the two windings. Flipping the
-- ones that point the wrong way makes the whole mesh consistent whatever
-- order its faces were written in.
--
-- Which of the two is "outward" here is a convention that has to be pinned
-- down once, and it is pinned down by the near face: the cube's z = -h face
-- is the one nearest a camera on the -z axis, it must be drawn, and its
-- winding gives a normal pointing *toward* the origin. So: keep the normal
-- pointing inward. That reads backwards and is correct, because screen y
-- grows downward while clip y grows upward and the projection has already
-- flipped the handedness once.
--
-- Exact only for a mesh that is convex and centred on the origin, which is
-- what "away from the centre" assumes. A teapot is neither, and gets its
-- winding from whoever exported it.
function g3d.orient(mesh)
  local v, f = mesh.vertices, mesh.faces

  for i = 1, #f, 3 do
    local a, b, c = (f[i] - 1) * 3, (f[i + 1] - 1) * 3, (f[i + 2] - 1) * 3

    local ax, ay, az = v[a + 1], v[a + 2], v[a + 3]
    local e1x, e1y, e1z = v[b + 1] - ax, v[b + 2] - ay, v[b + 3] - az
    local e2x, e2y, e2z = v[c + 1] - ax, v[c + 2] - ay, v[c + 3] - az

    local nx = e1y * e2z - e1z * e2y
    local ny = e1z * e2x - e1x * e2z
    local nz = e1x * e2y - e1y * e2x

    -- The triangle's own centre stands in for "which way is out".
    local cx = (ax + v[b + 1] + v[c + 1]) / 3
    local cy = (ay + v[b + 2] + v[c + 2]) / 3
    local cz = (az + v[b + 3] + v[c + 3]) / 3

    if nx * cx + ny * cy + nz * cz > 0 then
      f[i + 1], f[i + 2] = f[i + 2], f[i + 1]
    end
  end

  return mesh
end

function g3d.cube(size)
  local h = (size or 1) / 2

  -- 1 to 4 are the z = -h face, 5 to 8 the z = +h face, both counted
  -- anticlockwise from the bottom-left as seen down the -z axis.
  return g3d.orient {
    vertices = {
      -h, -h, -h,   h, -h, -h,   h,  h, -h,  -h,  h, -h,
      -h, -h,  h,   h, -h,  h,   h,  h,  h,  -h,  h,  h,
    },
    -- Written in whatever order reads clearly. `orient` above makes the
    -- winding consistent, so these only have to name the right corners.
    faces = {
      1, 2, 3,  1, 3, 4,      -- z = -h, toward a camera on the -z axis
      5, 6, 7,  5, 7, 8,      -- z = +h
      1, 2, 6,  1, 6, 5,      -- y = -h
      4, 3, 7,  4, 7, 8,      -- y = +h
      1, 4, 8,  1, 8, 5,      -- x = -h
      2, 3, 7,  2, 7, 6,      -- x = +h
    },
    colours = {
      0xff3a5f8f, 0xff3a5f8f,
      0xff5a7fbf, 0xff5a7fbf,
      0xff2a4f7f, 0xff2a4f7f,
      0xff6a8fcf, 0xff6a8fcf,
      0xff4a6f9f, 0xff4a6f9f,
      0xff7a9fdf, 0xff7a9fdf,
    },
  }
end

-- ------------------------------------------------------------------
-- Drawing.
--
-- One pass: transform every vertex once into screen space, then walk the
-- faces. Transforming per face instead would do the work three times over,
-- because a vertex in a closed mesh belongs to several faces.
--
-- `scratch` is passed in and reused across frames. Allocating two arrays
-- per frame is what makes a renderer's frame time sawtooth as the collector
-- catches up, and the fix is to not allocate.

function g3d.render(surface, mesh, mvp, w, h, scratch)
  local sx, sy, sz = scratch.x, scratch.y, scratch.z
  local vertices, faces, colours = mesh.vertices, mesh.faces, mesh.colours

  local m1,  m2,  m3,  m4  = mvp[1],  mvp[2],  mvp[3],  mvp[4]
  local m5,  m6,  m7,  m8  = mvp[5],  mvp[6],  mvp[7],  mvp[8]
  local m9,  m10, m11, m12 = mvp[9],  mvp[10], mvp[11], mvp[12]
  local m13, m14, m15, m16 = mvp[13], mvp[14], mvp[15], mvp[16]

  local hw, hh = w / 2, h / 2
  local n = #vertices // 3

  for i = 1, n do
    local j = (i - 1) * 3
    local x, y, z = vertices[j + 1], vertices[j + 2], vertices[j + 3]

    local cw = x*m4 + y*m8 + z*m12 + m16

    if cw > 0.0001 then
      -- The perspective divide, and the only division per vertex. Screen y
      -- grows downward and clip y grows upward, hence the minus.
      sx[i] = hw + (x*m1 + y*m5 + z*m9  + m13) / cw * hw
      sy[i] = hh - (x*m2 + y*m6 + z*m10 + m14) / cw * hh
      sz[i] = cw
    else
      -- Behind the eye. Marked rather than clipped: proper near-plane
      -- clipping splits a triangle into two and is worth writing when
      -- something actually flies through the camera. Until then a face
      -- with a vertex behind the eye is dropped, which is wrong only for
      -- geometry that surrounds the viewer.
      sz[i] = false
    end
  end

  -- Painter's algorithm: sort the faces back to front and draw in that
  -- order. A z-buffer is the right answer for a mesh that self-intersects
  -- and it is one more C primitive away, but it costs a full-screen buffer
  -- to clear every frame, and for a convex object it changes nothing. So:
  -- measure with this first.
  local order, depth = scratch.order, scratch.depth
  local count = 0

  for f = 1, #faces, 3 do
    local a, b, c = faces[f], faces[f + 1], faces[f + 2]
    local za, zb, zc = sz[a], sz[b], sz[c]

    if za and zb and zc then
      local ax, ay = sx[a], sy[a]
      local bx, by = sx[b], sy[b]
      local cx, cy = sx[c], sy[c]

      -- Back-face culling, in screen space, which is the whole of it: the
      -- sign of this cross product is the winding, and the winding flipped
      -- exactly when the face turned away. No normals and no dot product
      -- with a view vector - the projection already did that work.
      if (bx - ax) * (cy - ay) - (by - ay) * (cx - ax) < 0 then
        count = count + 1
        order[count] = f
        depth[count] = za + zb + zc
      end
    end
  end

  -- Insertion sort, deliberately. The face count is tens, the list is
  -- nearly sorted from one frame to the next because the object barely
  -- moves, and insertion sort is linear on nearly-sorted input where
  -- table.sort pays a function call per comparison.
  for i = 2, count do
    local o, d = order[i], depth[i]
    local j = i - 1

    while j >= 1 and depth[j] < d do
      order[j + 1], depth[j + 1] = order[j], depth[j]
      j = j - 1
    end

    order[j + 1], depth[j + 1] = o, d
  end

  local drawn = 0

  for i = 1, count do
    local f = order[i]
    local a, b, c = faces[f], faces[f + 1], faces[f + 2]

    surface:triangle(sx[a], sy[a], sx[b], sy[b], sx[c], sy[c],
                     colours[(f + 2) // 3])
    drawn = drawn + 1
  end

  return drawn
end

-- The arrays `render` reuses. One per renderer, not one per frame.
function g3d.scratch()
  return { x = {}, y = {}, z = {}, order = {}, depth = {} }
end

return g3d
