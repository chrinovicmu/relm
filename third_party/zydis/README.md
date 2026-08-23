# Zydis decoder sources

This directory vendors decoder-only files from the official Zydis **v4.1.0**
source release and its required Zycore **v1.5.0** headers:

- upstream: <https://github.com/zyantific/zydis>
- release: <https://github.com/zyantific/zydis/releases/tag/v4.1.0>
- Zycore: <https://github.com/zyantific/zycore-c/releases/tag/v1.5.0>
- licenses: MIT; see `LICENSE` and `LICENSE-ZYCORE`

The next Zydis release, v4.1.1, contains only a Meson build-system change. RELM
pins v4.1.0 so the decoder implementation and generated tables stay tied to the
same upstream release that was validated by the host-side vectors.

The vendored layout contains:

- Zydis public/internal headers under `include/Zydis/`;
- Zycore headers under `include/Zycore/`;
- upstream generated instruction data under `src/Generated/`;
- the common and decoder `.c` files named by `ZYDIS_C_NAMES` in the root
  `Makefile`.

Encoder, formatter, disassembler, and segment source files are intentionally
not vendored or linked. RELM compiles the selected decoder directly into the
kernel module with libc, AVX-512, and KNC support disabled. Minimal mode must
remain disabled because it omits operand details required by MMIO emulation.

Do not edit generated Zydis headers or tables by hand. To upgrade, replace the
two header trees, `src/Generated/`, and the selected `.c` files from one pinned
upstream release; then update this file, `VERSION`, and the decoder vectors.
