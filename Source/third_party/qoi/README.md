# QOI reference codec

- Purpose: compact lossless RGBA8 payloads inside VVT cache artifacts.
- Upstream: <https://github.com/phoboslab/qoi>
- Imported files: `qoi.h` and `LICENSE`.
- Pinned commit: `97bacc86a9c4abf5a2d452102dc26546c4c670b9`.
- `qoi.h` SHA-256: `7DE6FCA1A285B1C20D38F2723DEC8B774EB9F144EDB9710800A95FEEEA09375A`.
- License: MIT terms reproduced in [`LICENSE`](LICENSE).

VO-VE validates dimensions and byte limits before calling the reference decoder.
