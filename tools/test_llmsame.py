#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The LLM Kit gives one answer on this Mac and on an x86, to the byte.

    test_llmsame.py MODEL LLM_ANSWER LLM_ANSWER_X86

Xavier's box was held to the kit's answer on this Mac, and once in a few
gates it answered like the kit for a thousand bytes and then not
(`testing.md` 18.984). The x86 kit, under Rosetta, answered like the guest
to the byte: it was the Mac's that differed, because clang on ARM fuses a
multiply and an add into one instruction rounded once, and an ulp in a
long answer changed which word came next. The kit's host builds are made
with `-ffp-contract=off` (`LLM_HOST_FLAGS`), and this is the question that
showed it - the box's, drawn from the seed it was drawn from - asked of
both. Its control is the same builds without the flag: they part at byte
1038 of 1137.
"""
import subprocess
import sys

SEED = 275214463
QUESTION = (
    'You are Xavier, a helpful assistant. You run on Kosmos, on a'
    ' QEMU Standard PC (Q35 + ICH9, 2009) pc-q35-11.1. Today is S'
    'aturday, 21 November 2026. Answer any question as well as yo'
    'u can, in the language it was asked in.\nInstruct: what is a '
    'kernel\nOutput:'
)
STOPS = ["\nInstruct:", "\nOutput:"]


def answer(binary, model):
    out = subprocess.run([binary, "--chain", "--stops",
                          ",".join(s.encode().hex() for s in STOPS),
                          model, "512", "%d/%s" % (SEED, QUESTION.encode().hex())],
                         check=True, capture_output=True, text=True).stdout
    return out.strip()


def main():
    model, here, x86 = sys.argv[1:4]
    a, b = answer(here, model), answer(x86, model)

    if a != b or not a:
        n = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
        print("FAIL: the kit on this Mac and on an x86 answered apart from character "
              "%d: %r against %r" % (n, a[n:n + 60], b[n:n + 60]))
        return 1

    print("PASS: the kit's answer on this Mac and on an x86 is the same, %d characters"
          % len(a))
    return 0


if __name__ == "__main__":
    sys.exit(main())
