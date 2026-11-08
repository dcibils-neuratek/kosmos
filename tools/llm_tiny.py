#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**A tiny phi-2, for tests** (`docs/xavier.md` X3): phi-2's architecture and
phi-2's own tokenizer, at two layers 32 wide, with random weights from a
fixed seed - a few megabytes that llama.cpp loads like any model, so its
answers can be written down (`tools/llm_reference.cpp`) and held to the
LLM Kit in seconds: on the Mac without the 3 GB model, and inside Kosmos
under QEMU, where one token of the real phi-2 is a minute.

It knows nothing - its answers are noise - and that is the point: the
arithmetic is the same arithmetic, every tensor type the kit reads is in
one of its three files, and the noise is the same noise in both engines.

Usage (in build/venv-gguf, which has the `gguf` package llama.cpp ships):
    python -I tools/llm_tiny.py build/models/phi-2.Q4_0.gguf build/models

writes tiny-phi2.Q8_0.gguf, tiny-phi2.Q4_0.gguf and tiny-phi2.F16.gguf.
The tokenizer is copied from the phi-2 file named, so that file is needed
to make them, and not after.
"""

import os
import sys

import numpy as np
import gguf
from gguf import GGUFReader, GGUFValueType, GGUFWriter, GGMLQuantizationType as Q

SEED = 20261108
VOCAB = 51200
EMBD, FF, LAYERS, HEADS, ROT, CTX = 32, 128, 2, 4, 4, 256


def copy_tokenizer(reader, writer):
    """Every tokenizer.* key of the real phi-2, as it is there."""
    for name, field in reader.fields.items():
        if not name.startswith("tokenizer."):
            continue
        kind = field.types[0]
        if kind == GGUFValueType.ARRAY:
            writer.add_array(name, field.contents())
        elif kind == GGUFValueType.STRING:
            writer.add_string(name, field.contents())
        elif kind == GGUFValueType.BOOL:
            writer.add_bool(name, bool(field.contents()))
        else:
            writer.add_uint32(name, int(field.contents()))


def weights(rng):
    """The tensors, as floats, in phi-2's names and ggml's shapes - a
    matrix of `rows` rows of `cols` is numpy (rows, cols)."""
    def mat(rows, cols, scale):
        return (rng.standard_normal((rows, cols)) * scale).astype(np.float32)

    def vec(n, scale, around=0.0):
        return (around + rng.standard_normal(n) * scale).astype(np.float32)

    out = {"token_embd.weight": mat(VOCAB, EMBD, 1.0)}
    for l in range(LAYERS):
        p = f"blk.{l}."
        out[p + "attn_norm.weight"] = vec(EMBD, 0.1, 1.0)
        out[p + "attn_norm.bias"] = vec(EMBD, 0.1)
        for t in ("attn_q", "attn_k", "attn_v", "attn_output"):
            out[p + t + ".weight"] = mat(EMBD, EMBD, 1.0 / np.sqrt(EMBD))
            out[p + t + ".bias"] = vec(EMBD, 0.1)
        out[p + "ffn_up.weight"] = mat(FF, EMBD, 1.0 / np.sqrt(EMBD))
        out[p + "ffn_up.bias"] = vec(FF, 0.1)
        out[p + "ffn_down.weight"] = mat(EMBD, FF, 1.0 / np.sqrt(FF))
        out[p + "ffn_down.bias"] = vec(EMBD, 0.1)
    out["output_norm.weight"] = vec(EMBD, 0.1, 1.0)
    out["output_norm.bias"] = vec(EMBD, 0.1)
    out["output.weight"] = mat(VOCAB, EMBD, 1.0 / np.sqrt(EMBD))
    out["output.bias"] = vec(VOCAB, 0.1)
    return out


def write(path, kind, reader, tensors):
    w = GGUFWriter(path, "phi2")
    w.add_name("tiny phi-2 for tests")
    w.add_context_length(CTX)
    w.add_embedding_length(EMBD)
    w.add_feed_forward_length(FF)
    w.add_block_count(LAYERS)
    w.add_head_count(HEADS)
    w.add_head_count_kv(HEADS)
    w.add_layer_norm_eps(1e-5)
    w.add_rope_dimension_count(ROT)
    w.add_file_type({Q.Q8_0: 7, Q.Q4_0: 2, Q.F16: 1}[kind])
    copy_tokenizer(reader, w)

    for name, t in tensors.items():
        if t.ndim == 1:
            w.add_tensor(name, t)
        elif kind == Q.F16:
            w.add_tensor(name, t.astype(np.float16))
        else:
            w.add_tensor(name, gguf.quants.quantize(t, kind), raw_dtype=kind)

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    reader = GGUFReader(sys.argv[1])
    tensors = weights(np.random.default_rng(SEED))
    os.makedirs(sys.argv[2], exist_ok=True)
    for kind, label in ((Q.Q8_0, "Q8_0"), (Q.Q4_0, "Q4_0"), (Q.F16, "F16")):
        path = os.path.join(sys.argv[2], f"tiny-phi2.{label}.gguf")
        write(path, kind, reader, tensors)
        print(f"llm_tiny: {path}, {os.path.getsize(path)} bytes")


if __name__ == "__main__":
    main()
