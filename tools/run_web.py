#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The NetSurf libraries, running on the machine.

Five libraries compiling and linking says nothing about whether they work: a
freestanding libc can satisfy every symbol and still hand back a null on the
first allocation. So this boots an image built with `WEB=1` and asks it to
parse things.

What each check is really for:

  * the title proves hubbub parsed, libdom built a tree, and text content
    came back out of it;
  * counting `p` proves a *walk* rather than a token count, because one of
    them is nested inside a div;
  * the entity proves `entities.inc` - the table a perl script generates
    during the build - is real and is being consulted;
  * the stylesheet proves libcss parsed, and the long one proves the 119
    property parsers `gen_parser` emits are present and doing their job.
    Missing them is a link error, but a *wrong* one would look like a sheet
    that parses and understands nothing.

Not run by `make test`, because `WEB=1` is an optional variant like `DOOM=1`
and the ordinary image carries none of this. `make web` is the entry point.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot


class Failure(Exception):
    pass


# One line, so it can be typed at the prompt. `sys.kit` rather than `use`,
# because `use` is a *program's* global and this is the shell.
PROBE = (
    'local w = sys.kit("web") '
    'local d = w.parse("<html><head><title>Hello Kosmos</title></head>'
    '<body><p>one</p><p>two</p><div><p>three</p></div></body></html>") '
    'local e = w.parse("<p>a &amp; b</p>") '
    'local m = w.parse("<p>unclosed<div>and nested") '
    'local s1 = w.stylesheet("p { color: red } .x { margin: 1px }") '
    'local s2 = w.stylesheet("p{color:blue;margin:1px;padding:2px;'
    'display:block;float:left;font-size:12px;line-height:1.5;width:10px}") '
    'print("<<".."WEB>>", d and d:title(), d and d:count("p"), '
    'd and d:count("div"), e and e:text("p"), m and m:count("div"), '
    's1, s2, (e and e.count) and "ok")'
)

#
# Selection, which is a different claim from parsing.
#
# Four styles, and the fourth is the one that matters: a rule that should
# *not* match must not match. A handler that answered `true` to everything
# would pass the first three and fail only that one, and it is exactly the
# mistake thirty-six hand-written callbacks invite.
#
SELECT = (
    'local w = sys.kit("web") '
    'local d = w.parse("<html><body><div><p class=\\"warn\\">x</p></div>'
    '<span>y</span></body></html>") '
    'print("<<".."SEL>>", d:style("p{color:#ff0000}", "p"), '
    'd:style(".warn{color:#00ff00}", "p"), '
    'd:style("div p{color:#0000ff}", "p"), '
    'd:style("p{color:#ff0000}", "span"))'
)


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0

    guest = run_screenshot.Guest(image, 120)

    try:
        guest.wait_for(run_screenshot.PROMPT, "the prompt")
        guest.seen = ""
        guest.type(PROBE)
        guest.timeout = 60
        guest.wait_for("<<WEB>>", "the libraries to answer")

        line = ""
        for text in guest.seen.replace("\r", "").splitlines():
            if text.startswith("<<WEB>>"):
                line = text
                break

        got = line.replace("<<WEB>>", "").split("\t")
        got = [f for f in (g.strip() for g in got) if f != ""]

        if len(got) < 7:
            raise Failure(f"the probe answered with {got!r}\n{guest.seen[-800:]}")

        title, npara, ndiv, entity, mdiv, sheet1, sheet2 = got[:7]

        if title != "Hello Kosmos":
            raise Failure(f"the title came back as {title!r}")

        checks += 1

        # Three, and the third is inside the div - so this walked the tree.
        if npara != "3":
            raise Failure(f"counted {npara} p elements, expected 3")

        checks += 1

        if ndiv != "1":
            raise Failure(f"counted {ndiv} div elements, expected 1")

        checks += 1

        # `&amp;` decoded to `&`, which it can only do by consulting the
        # entity table a perl script generates during the build. A broken
        # generation step still links and still parses; this is what notices.
        if entity != "a & b":
            raise Failure(
                f"the entity came back as {entity!r}, expected 'a & b' - "
                "which is what a wrong or empty entities.inc looks like"
            )

        checks += 1

        # Unclosed tags are what HTML5 parsing is *for*: hubbub has to
        # recover and still produce a tree.
        if mdiv != "1":
            raise Failure(
                f"malformed html produced {mdiv} divs, expected 1 - the "
                "parser did not recover"
            )

        checks += 1

        if sheet1 != "true":
            raise Failure(f"a simple stylesheet answered {sheet1!r}")

        checks += 1

        if sheet2 != "true":
            raise Failure(
                f"a stylesheet using eight properties answered {sheet2!r}, "
                "which is what a missing generated property parser looks like"
            )

        checks += 1

        # ---- and the cascade ----
        guest.seen = ""
        guest.type(SELECT)
        guest.wait_for("<<SEL>>", "the cascade to answer")

        line = ""
        for text in guest.seen.replace("\r", "").splitlines():
            if text.startswith("<<SEL>>"):
                line = text
                break

        got = [f for f in (g.strip() for g in
                           line.replace("<<SEL>>", "").split("\t")) if f != ""]

        if got != ["#ff0000", "#00ff00", "#0000ff", "#000000"]:
            raise Failure(
                f"the cascade answered {got!r}, expected a type selector, a "
                "class selector and a descendant combinator to match, and a "
                "rule for `p` asked about a `span` NOT to - falling back to "
                "the UA default black"
            )

        # Four assertions in one line, and each fails for its own reason:
        # the name lookup, the class list, the ancestor walk, and a handler
        # that says no when it should.
        checks += 4

        print(
            f"PASS: {checks} checks on the web libraries: a document parsed, "
            "its tree walked, a stylesheet understood, and the cascade run."
        )
        return 0
    except (Failure, Exception) as e:      # noqa: BLE001 - it is the result
        print(f"FAIL: {e}")
        return 1
    finally:
        guest.close()


if __name__ == "__main__":
    sys.exit(main())
