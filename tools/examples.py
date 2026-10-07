#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The IDE's examples, put together for the image (`/Kosmos/Examples`;
`roadmap.md`, the IDE's examples, 7 October).

Nothing here is a second copy kept in the tree. Each GL demo is TinyGL's own
file from `runtime/upstream/tinygl/examples/`, as released, with a window
written for it - `window.c`, the GL Kit's door (`kosmos_gl.h`) - and the line
of Lua that starts it, which says the defines the demo needs: its `main`,
which the runtime has already, and `mech.c`'s `display`, GLUT's name for
`draw`. Cube in Lua is the system's own cube3d; Cube in C is
`user/examples/CubeC/`.

Usage: examples.py OUT_DIR    - and the files written, one a line
"""

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UPSTREAM = os.path.join(ROOT, "runtime", "upstream", "tinygl", "examples")

# Folder, demo, title, what it shows - as `user/kits/gl/gl_demos.c` lists them.
GL = [
    ("GLGears",   "gears",   "GL Gears",   "Brian Paul's gears, the oldest OpenGL demo there is"),
    ("GLTeapot",  "teapot",  "GL Teapot",  "the Utah teapot, lit"),
    ("GLSpin",    "spin",    "GL Spin",    "two spinning shapes"),
    ("GLBounce",  "bounce",  "GL Bounce",  "a bouncing ball"),
    ("GLCube",    "cube",    "GL Cube",    "a textured cube"),
    ("GLMorph3D", "morph3d", "GL Morph3D", "morphing platonic solids"),
    ("GLMech",    "mech",    "GL Mech",    "a walking mech, and the largest of them"),
    ("GLTexObj",  "texobj",  "GL Texture Objects", "texture objects"),
]

HEADER = "/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */\n"

WINDOW = HEADER + """/*
 * {title}, in a window of its own. {demo}.c is TinyGL's own demo, unchanged:
 * it is written to TinyGL's `ui.h` - init, draw, idle, reshape and key -
 * and this is all Kosmos asks besides it. The GL Kit's door opens the
 * window, makes the GL context, and calls them (`kosmos_gl.h`).
 */

#include <GL/gl.h>

#include "ui.h"
#include "kosmos_kit.h"
#include "kosmos_gl.h"

static int l_main(lua_State *L)
{{
    const char *why = kosmos_gl_run("{title}", 400, 300, init, draw, idle, reshape, key);

    lua_pushstring(L, why ? why : "{demo}: closed");
    return 1;
}}

KOSMOS_KIT({demo})
{{
    lua_newtable(L);
    lua_pushcfunction(L, l_main);
    lua_setfield(L, -2, "main");
}}
"""

STARTER = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: name {title}
-- kosmos: image build/{demo}.elf
-- kosmos: define {defines}
--
-- {title}: {what}. TinyGL's own demo in C, built by the IDE - F6 builds
-- {demo}.c and window.c into build/{demo}.elf, F5 runs it. Arrows turn it,
-- where the demo answers them; Escape or the close box ends it. The define
-- above renames the demo's `main`, which every program here has already.
print(use("{demo}.elf").main())
"""


def write(path, text, out):
    os.makedirs(os.path.dirname(path), exist_ok=True)

    with open(path, "w") as f:
        f.write(text)

    out.append(path)


def main():
    out_dir = sys.argv[1]
    made = []

    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)

    for folder, demo, title, what in GL:
        d = os.path.join(out_dir, folder)
        defines = "main=demo_main" + (" display=draw" if demo == "mech" else "")

        os.makedirs(d)

        for name in [demo + ".c"] + (["teapot.h"] if demo == "teapot" else []):
            shutil.copyfile(os.path.join(UPSTREAM, name), os.path.join(d, name))
            made.append(os.path.join(d, name))

        write(os.path.join(d, "window.c"), WINDOW.format(title=title, demo=demo), made)
        write(os.path.join(d, demo + ".lua"),
              STARTER.format(title=title, demo=demo, what=what, defines=defines), made)

    # Cube twice: the system's own in Lua, and the same in C.
    d = os.path.join(out_dir, "CubeLua")
    os.makedirs(d)
    shutil.copyfile(os.path.join(ROOT, "user", "bin", "apps", "cube3d.lua"),
                    os.path.join(d, "cube3d.lua"))
    made.append(os.path.join(d, "cube3d.lua"))

    src = os.path.join(ROOT, "user", "examples", "CubeC")
    d = os.path.join(out_dir, "CubeC")
    os.makedirs(d)

    for name in sorted(os.listdir(src)):
        shutil.copyfile(os.path.join(src, name), os.path.join(d, name))
        made.append(os.path.join(d, name))

    print("\n".join(made))


if __name__ == "__main__":
    main()
