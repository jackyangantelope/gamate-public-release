# Third-party components

- `deps/chips/w65c02.h`: floooh/chips, revision `ca7d7ddd3ba77b48685d24120cf413ea53786767`, zlib/libpng license (`deps/chips/LICENSE`). Used instead of the previous noncommercial CPU implementation.
- `deps/gamate/emu2149.c` and `.h`: EMU2149, MIT license as stated in their SPDX headers. These files are from xrip/gamate revision `1645e11715a91592cc126c483b273903411b28fe`.
- `deps/gamate/vdp.cpp` and `.h`: from the same xrip/gamate revision; `vdp.cpp` declares BSD-3-Clause. The upstream checkout has no top-level LICENSE, and the header has no file-level notice. Independent provenance/license confirmation is still required before public publication.
- `deps/pico_shared`: PicoPlus-devel/pico_shared revision `df683bf00ff95e1ceda88f132bbf72f8cc7cadb0`, GPL-3.0 as shipped in `deps/pico_shared/LICENSE`.
- `platform/wave1` board/runtime and `cmake/LoaderPartition.cmake`: adapted from the existing jack-green-ports Fruit Jam port, covered by this repository's BSD-3-Clause `LICENSE`; the combined firmware must also satisfy the GPL-3.0 obligations of pico_shared.

No BIOS or game ROM is included. Do not redistribute those assets unless you separately hold the necessary rights.
