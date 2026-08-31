# builds

Images you can download and run without building anything.

```sh
./run-kosmos.sh                 # the largest build here
./run-kosmos.sh -r 1920x1080    # at that size
./run-kosmos.sh -r list         # which sizes are here
./run-kosmos.sh kosmos-0.6-abc1234-1024x768.elf
```

Needs `qemu-system-aarch64` and nothing else. On macOS: `brew install qemu`.

Each file is named by version, by the commit it was built from, and by the
screen size it was built for, so several can sit here at once and it is
always clear which one is running - the version and commit are in the corner
of the desktop and in `About Kosmos`.

The size is in the name because it is compiled in. There is no display
negotiation: the image asks the firmware for a framebuffer of one size and
that is the size it gets, so choosing a resolution means choosing a file.
That is what `-r` does, and it stops being true when a real display driver
arrives.

The image is self-contained. The userland, the Lua interpreter, every
program in `/bin`, every library in `/lib` and the font are inside it. There
is nothing to install and nothing to mount, which is a property of not
having a filesystem yet and will stop being true at M8.

`make release` adds one.
