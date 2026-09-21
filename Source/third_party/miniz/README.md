# miniz inflate contour

- Upstream: <https://github.com/richgel999/miniz>
- Version: 3.1.2
- Pinned commit: `77d0dce8627735138c51770d1799a1ef48f2117d`
- Purpose: bounded raw-deflate decompression of known embedded CDR preview entries.
- Included implementation: `miniz_tinfl.c` and the headers required to compile it.
- Excluded at compile time: archive APIs, compression, zlib-compatible APIs, stdio and miniz heap
  helpers. VO-VE owns ZIP validation, allocation limits, CRC checks and output buffers.
- License: MIT terms reproduced in `LICENSE`.

The source was imported from the pinned upstream tree. It is linked only into the lazy CDR worker;
the GUI and the document worker do not contain it.
