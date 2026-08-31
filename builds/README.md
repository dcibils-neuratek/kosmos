# builds

Images you can download and run without building anything.

```sh
./run-kosmos.sh kosmos-0.6-abc1234.elf
```

Needs `qemu-system-aarch64` and nothing else. On macOS: `brew install qemu`.

Each file is named by version and by the commit it was built from, so
several can sit here at once and it is always clear which one is running -
the same string is in the corner of the desktop and in `About Kosmos`.

The image is self-contained. The userland, the Lua interpreter, every
program in `/bin`, every library in `/lib` and the font are inside it. There
is nothing to install and nothing to mount, which is a property of not
having a filesystem yet and will stop being true at M8.

`make release` adds one.
