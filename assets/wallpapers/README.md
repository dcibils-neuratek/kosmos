# assets/wallpapers

The desktop's pictures, carried in the image: 24 photographs from
Unsplash, chosen by Diego on 18 September 2026 ("can we also add
/Users/diego/Downloads/kosmos-wallpapers to kosmos"). Appearance lists them
beside any picture in `/home`, and the window manager draws them as it draws
those.

**In `FULL=1` images only** - `make qemu`, the ThinkPad's stick, `make
test`'s machines - and not in `FULL=0` or the test and bench images, the way
Doom is: nine megabytes of photographs are the desktop's, and a lean image
has no desktop to show them on.

**Under the Unsplash License**, beside them as `LICENSE`. It lets anybody
copy, modify and distribute them without asking and without attribution; it
does not let anybody compile them into a service like Unsplash's. They are
credited here all the same, each by the photographer's name as Unsplash
gives it in the file and the page the photograph came from.

**Not as the photographers released them, and this is a departure.**
`CLAUDE.md` keeps vendored data byte for byte, with anything done to it a
build step somebody can read. These are the downloads scaled to cover
1920x1080, cropped to their middle and saved as JPEG at quality 80 - 9.2 MB
for all of them, where the originals are 65 MB - because Diego asked for
exactly that ("convert them to jpg at 1920x1080 as much as possible so they
save space before adding them to the image"), and because 65 MB of
photographs in the repository's history to keep a build step honest is a
cost the step does not repay. So the step is kept rather than run:
`tools/wallpapers.sh` is what made these files from the downloads, and
running it again on the same downloads makes them again. The window manager
centres a picture rather than scaling it, so a 1920x1080 screen shows each
one exactly as it is here.

**What was left out, and why.** The download folder also had Lenovo's own
ThinkPad wallpapers. Nothing licenses those for redistribution, and this
repository is public, so they are not here; they can go on a stick's `/home`
the way the Doom WAD does, from the machine that has them. And three flat
colours, which the desktop colour already is.

| File | Photographer | Photograph |
| ---- | ------------ | ---------- |
| `alexander-slattery-LI748t0BK8w.jpg` | Alexander Slattery | https://unsplash.com/photos/LI748t0BK8w |
| `anders-jilden-uwbajDCODj4.jpg` | Anders Jilden | https://unsplash.com/photos/uwbajDCODj4 |
| `arto-marttinen-fHXP17AxOEk.jpg` | Arto Marttinen | https://unsplash.com/photos/fHXP17AxOEk |
| `benjamin-voros-phIFdC6lA4E.jpg` | Benjamin Voros | https://unsplash.com/photos/phIFdC6lA4E |
| `clark-tibbs-oqStl2L5oxI.jpg` | Clark Tibbs | https://unsplash.com/photos/oqStl2L5oxI |
| `clem-onojeghuo-zlABb6Gke24.jpg` | Clem Onojeghuo | https://unsplash.com/photos/zlABb6Gke24 |
| `cristina-gottardi-CSpjU6hYo_0.jpg` | Cristina Gottardi | https://unsplash.com/photos/CSpjU6hYo_0 |
| `denys-nevozhai-D68ADLeMh5Q.jpg` | Denys Nevozhai | https://unsplash.com/photos/D68ADLeMh5Q |
| `garrett-parker-DlkF4-dbCOU.jpg` | Garrett Parker | https://unsplash.com/photos/DlkF4-dbCOU |
| `graham-holtshausen-63JKK67yGUE.jpg` | Graham Holtshausen | https://unsplash.com/photos/63JKK67yGUE |
| `javen-yang-MWZi4XTIsKA.jpg` | Javen Yang | https://unsplash.com/photos/MWZi4XTIsKA |
| `jeremy-thomas-jh2KTqHLMjE.jpg` | Jeremy Thomas | https://unsplash.com/photos/jh2KTqHLMjE |
| `kalen-emsley-Bkci_8qcdvQ.jpg` | Kalen Emsley | https://unsplash.com/photos/Bkci_8qcdvQ |
| `khamkeo-myZqfQhh9QU.jpg` | Khamkeo | https://unsplash.com/photos/myZqfQhh9QU |
| `lucas-k-R79qkPYvrcM.jpg` | Lucas K | https://unsplash.com/photos/R79qkPYvrcM |
| `martin-martz-W0NRebXbsjM.jpg` | Martin Martz | https://unsplash.com/photos/W0NRebXbsjM |
| `patrick-tomasso-n-vxsHr9jZA.jpg` | Patrick Tomasso | https://unsplash.com/photos/n-vxsHr9jZA |
| `riccardo-chiarini-gYCMiZp-A7E.jpg` | Riccardo Chiarini | https://unsplash.com/photos/gYCMiZp-A7E |
| `richard-horvath-_nWaeTF6qo0.jpg` | Richard Horvath | https://unsplash.com/photos/_nWaeTF6qo0 |
| `robert-lukeman-zNN6ubHmruI.jpg` | Robert Lukeman | https://unsplash.com/photos/zNN6ubHmruI |
| `tim-foster-o4mP43oPGHk.jpg` | Tim Foster | https://unsplash.com/photos/o4mP43oPGHk |
| `v2osk-eKTUtA74uN0.jpg` | v2osk | https://unsplash.com/photos/eKTUtA74uN0 |
| `wexor-tmg-L-2p8fapOA8.jpg` | Wexor Tmg | https://unsplash.com/photos/L-2p8fapOA8 |
| `wil-stewart-2aCuwSh4RRk.jpg` | Wil Stewart | https://unsplash.com/photos/2aCuwSh4RRk |
