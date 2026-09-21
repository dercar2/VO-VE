# Little CMS 2.19.1

- Purpose: ICC-aware conversion to the canonical sRGB thumbnail space inside the preview worker.
- Upstream: <https://github.com/mm2/Little-CMS>
- Imported files: public headers, C sources, internal header and `LICENSE`.
- Pinned upstream commit: `21c582a594fe5279f90c0b93437c398f93bf62b0` (`lcms2.19.1`).
- License: MIT; terms are reproduced in [`LICENSE`](LICENSE).

Little CMS is linked only into the worker-side color library. It is not linked into the UI target.
