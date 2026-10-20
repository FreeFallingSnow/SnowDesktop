# libwebp

Official libwebp 1.6.0 source archive, used as a statically linked decoder.

- Source: https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz
- SHA256: e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
- License: upstream COPYING and PATENTS in this directory.
- The application links only the decoder; command-line utilities and upstream tests are disabled.
- The native image regression target links the encoder to create independent lossy/lossless fixtures.
- Scoped Git whitespace attributes preserve the official archive and license files unchanged.
