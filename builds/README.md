# builds

Images you can download and run without building anything.

The short way, which fetches the newest of these and the runner and boots
them - nothing to clone and nothing to download first:

```sh
sh -c "$(curl -fsSL https://raw.githubusercontent.com/dcibils-neuratek/kosmos/main/get-and-run-kosmos.sh)" -- -b "wm"
```

Or keep a copy, which is quicker to type afterwards:

```sh
./get-and-run-kosmos.sh -b "wm"
./get-and-run-kosmos.sh --list            # what is published, and what it has in it
./get-and-run-kosmos.sh --plain           # the small image, no browser
./get-and-run-kosmos.sh --update          # take the published copy of itself
```

```sh
./run-kosmos.sh                 # the newest build here
./run-kosmos.sh -r 1920x1080    # at that size
./run-kosmos.sh -r list         # which sizes are here
./run-kosmos.sh kosmos-0.6-abc1234-1024x768.elf
```

**You never have to type the file name.** It has a version and a commit in
it and both change on every release, so nothing here hardcodes one and
nothing asks you to know one: with no arguments the script takes the newest
version it can find, and the largest build of that version. `-r` picks by
size instead. The name is for telling two of them apart afterwards, not for
starting one.

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

## `-full`, and why there is only one of it

A file whose name ends `-full` carries everything: the browser (hubbub,
libdom, libcss and the two libraries under them) and Doom. That takes the
image from 1.7 MB to 6.2 MB, and stripping saves a quarter of a megabyte
because the bulk is the userland compiled in rather than symbols - so there
is **one** size of it, 1920x1080, which is what `make qemu` defaults to. A
lean image still comes in all three sizes, because three of those is nearly
free.

The suffix is in the name because the images are not interchangeable. On a
lean one the browser opens and says the build has no web kit, which reads
like a broken browser rather than the wrong file.

```sh
./run-kosmos.sh -r 1280x800 -b "wm browser"
```

**Nothing has to be running anywhere.** `wm browser` opens on a page inside
the image, parsed and painted by the same engine a fetched one is. Starting
a web server on the computer running QEMU to look at a new build of an
operating system is a thing this should never have asked for, and briefly
did.

`-r` finds the image wherever it is, which is the point of `-r`: a path is
only right relative to where you are standing, and these two files are meant
to be copied somewhere else together. `./run-kosmos.sh -r list` says what is
there, and so does the error if you name a file that is not.

Somewhere else to go, once it is up - anything beginning with a slash is
read from this machine rather than the network:

```
  10.0.2.2:8000/            a server on the computer running QEMU
  188.184.67.127/           somewhere on the internet, by number
  /home/notes.html          a file on this machine
```

**No names and no https.** There is no resolver, so a remote address is four
numbers; and there is no TLS, so most of the web refuses to speak. QEMU's
NAT does give the guest real outbound internet, so a plain-HTTP host that
serves by address does work.

**There is no DNS**, so an address is four numbers and a path. QEMU's own
NAT maps this computer as `10.0.2.2`, which is why serving a directory here
is enough and no packet leaves the machine. `run-kosmos.sh` passes the two
flags that make that work; without them `ping`, `fetch` and the browser all
run and find nothing.

`make release` builds one; `make FULL=0 release` builds the lean three.
