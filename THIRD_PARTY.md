# Third-party software and assets

The repository contains project-owned code plus a modified vendored copy of
PsyCross. Release packages also dynamically link several libraries installed by
the vcpkg manifest.

| Component | Use | License source |
| --- | --- | --- |
| PsyCross | PS1-compatible platform/rendering backend | `external/PsyCross/LICENSE` |
| SDL2 | Window, input and platform services | vcpkg package copyright file |
| OpenAL Soft | Native audio output | vcpkg package copyright file |
| FFmpeg | STR/FM​​V demux/decode and audio conversion | vcpkg package copyright file |
| fmt | Formatting support used by the backend | vcpkg package copyright file |
| Industry Bold (RUS by Slavchansky) | User-supplied source face for the generated Russian font atlas; the TTF is not redistributed | `tools/fonts/industry/COPYRIGHT.txt` |
| Microsoft Visual C++ Runtime | Windows runtime libraries in binary releases | Applicable Microsoft license terms |

The release packager copies the exact dependency notices into its `licenses/`
directory and writes `THIRD_PARTY_NOTICES.txt` at the package root.

PsyCross is vendored rather than referenced as a submodule because this port
requires local GPU, GTE, PGXP, framebuffer, filtering, input and crash-handling
changes. Its original upstream is <https://github.com/OpenDriver2/PsyCross> and
the vendored base revision is `e56e4cd`.

The `assets/dossiers/screens` images and launcher icon are presentation assets
for this fan project. They do not grant any rights to *Syphon Filter*, its
characters, artwork or trademarks. Do not reuse or redistribute original game
data outside the rights granted by its owner.
