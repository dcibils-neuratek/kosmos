#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""llama.cpp's compute shaders, compiled for the LLM Kit (`docs/xavier.md`
X12, V2).

The shaders are llama.cpp's own (`ggml/src/ggml-vulkan/vulkan-shaders`,
MIT), from the release `tools/upstream.py` fetches, unedited. What this
does is what `vulkan-shaders-gen.cpp` does for the variants the LLM Kit
uses, and only those: each `.comp` compiled with the defines the generator
gives that variant, by `glslangValidator` rather than `glslc` - whose one
difference here is that it turns on `#include` by itself, so the build adds
that line after `#version` in a copy - into one C header of SPIR-V words.

Usage: vkshaders.py SHADERS_DIR OUT.h
"""

import os
import subprocess
import sys

import scratch

# The generator's `base_dict` for matrix-vector products, and the rest of
# each variant's defines, as `vulkan-shaders-gen.cpp` gives them.
MMV = {"FLOAT_TYPE": "float", "FLOAT_TYPEV2": "vec2"}
MMV_F32 = {"B_TYPE": "float", "B_TYPEV2": "vec2", "B_TYPEV4": "vec4", "D_TYPE": "float"}


# The question's products (V5): `mul_mm.comp` as the generator's
# `matmul_shaders(fp16=false, ...)` gives it - floats throughout, an F32
# accumulator, and B read four floats at a time - with the quantised types
# one shader, the type a specialisation constant (`MmTypeA`).
MM = {"FLOAT_TYPE": "float", "FLOAT_TYPEV2": "vec2", "FLOAT_TYPEV4": "vec4",
      "FLOAT_TYPEV8": "mat2x4", "ACC_TYPE": "float", "ACC_TYPEV2": "vec2",
      "ACC_TYPE_VEC4": "vec4", "LOAD_VEC_B": "4", "B_TYPE": "vec4",
      "B_TYPE_SCALAR": "float", "B_TYPEV4": "vec4", "D_TYPE": "float"}


def mmv(t, summed=None):
    shader = "mul_mat_vec_%s.comp" % t if t.endswith("_k") else "mul_mat_vec.comp"
    d = dict(MMV, **MMV_F32, **{"DATA_A_" + t.upper(): "1"})
    if summed:                      # how a workgroup's lanes are summed (V6)
        d[summed] = "1"
    return shader, d


VARIANTS = {
    "mul_mat_vec_q4_0_f32_f32": mmv("q4_0"),
    "mul_mat_vec_q8_0_f32_f32": mmv("q8_0"),
    "mul_mat_vec_q6_k_f32_f32": mmv("q6_k"),
    "mul_mat_vec_f32_f32_f32": mmv("f32"),
    "mul_mat_vec_f16_f32_f32": mmv("f16"),
    # Q4_0's summed by `subgroupAdd`, then through shared memory across a
    # workgroup's subgroups (ggml's hybrid): the HD 530's fastest (V6).
    "mul_mat_vec_q4_0_add": mmv("q4_0", "USE_SUBGROUP_ADD"),
    # Attention's two products, over the cache: the keys a head at a time
    # and the values transposed, rows a stride apart.
    "mul_mat_vec_nc_f16_f32": ("mul_mat_vec_nc.comp", dict(A_TYPE="float16_t", A_TYPEV4="f16vec4",
                                                          B_TYPE="float", B_TYPEV4="vec4",
                                                          D_TYPE="float")),
    "matmul_quant_f32_fp32": ("mul_mm.comp", dict(MM, MULMAT_QUANT="1")),
    "matmul_f16_f32_fp32": ("mul_mm.comp", dict(MM, DATA_A_F16="1", LOAD_VEC_A="4")),
    "matmul_f32_f32_fp32": ("mul_mm.comp", dict(MM, DATA_A_F32="1", LOAD_VEC_A="4")),
    # A layer's other steps.
    "norm_f32": ("norm.comp", dict(MMV, A_TYPE="float", D_TYPE="float")),
    "rms_norm_f32": ("rms_norm.comp", dict(MMV, A_TYPE="float", B_TYPE="float", D_TYPE="float",
                                          USE_SUBGROUP_ADD="0")),
    "add_f32_f32_f32": ("add.comp", dict(A_TYPE="float", B_TYPE="float", D_TYPE="float",
                                         FLOAT_TYPE="float", ADD_RMS="0")),
    "mul_f32_f32_f32": ("mul.comp", dict(A_TYPE="float", B_TYPE="float", D_TYPE="float",
                                         FLOAT_TYPE="float", ADD_RMS="0")),
    "scale_f32": ("scale.comp", dict(A_TYPE="float", D_TYPE="float", FLOAT_TYPE="float")),
    "gelu_f32": ("unary.comp", dict(A_TYPE="float", D_TYPE="float", OP="op_gelu")),
    "silu_f32": ("unary.comp", dict(A_TYPE="float", D_TYPE="float", OP="op_silu")),
    "swiglu_f32": ("swiglu.comp", dict(A_TYPE="float", D_TYPE="float")),
    "soft_max_f32": ("soft_max.comp", dict(MMV, A_TYPE="float", B_TYPE="float", D_TYPE="float")),
    "rope_neox_f32": ("rope_neox.comp", dict(A_TYPE="float", ROPE_D_TYPE="float")),
    "cpy_f32_f16": ("copy.comp", dict(A_TYPE="float", D_TYPE="float16_t")),
    "cpy_f32_f32": ("copy.comp", dict(A_TYPE="float", D_TYPE="float")),
}


def compile_one(src, name, shader, defines, work):
    """The SPIR-V words of one variant."""
    with open(os.path.join(src, shader)) as f:
        text = f.read()
    first, rest = text.split("\n", 1)
    if not first.startswith("#version"):
        raise SystemExit("%s: does not open with #version" % shader)
    copy = os.path.join(work, shader)
    with open(copy, "w") as f:
        f.write(first + "\n#extension GL_GOOGLE_include_directive : require\n" + rest)
    out = os.path.join(work, name + ".spv")
    cmd = ["glslangValidator", "-V", "-S", "comp", "--target-env", "vulkan1.2",
           "-I" + src, "-o", out] + ["-D%s=%s" % kv for kv in sorted(defines.items())] + [copy]
    run = subprocess.run(cmd, capture_output=True, text=True)
    if run.returncode != 0:
        raise SystemExit("%s: %s\n%s" % (name, " ".join(cmd), run.stdout + run.stderr))
    with open(out, "rb") as f:
        data = f.read()
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


def main():
    src, header = sys.argv[1], sys.argv[2]
    lines = ["/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */",
             "/* Generated by tools/vkshaders.py from llama.cpp's shaders (MIT,",
             " * Copyright (c) 2023-2026 The ggml authors); do not edit. */", ""]
    work = scratch.directory("vkshaders")
    for name, (shader, defines) in VARIANTS.items():
        words = compile_one(src, name, shader, defines, work)
        lines.append("static const uint32_t llmvk_%s[%d] = {" % (name, len(words)))
        for i in range(0, len(words), 8):
            lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
        lines.append("};")
    with open(header, "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
