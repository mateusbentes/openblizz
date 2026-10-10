# Contributing

OpenBlizz aims to remain an independent interoperability implementation.

- Do not submit Blizzard binaries, game files, private keys, or credentials.
- Do not copy or translate source code from proprietary clients or from projects
  without a license permitting reuse.
- Document public protocol and format references in `SOURCES.md`.
- Add tests for every parser and product-specific behavior.
- Never ask users to paste passwords or MFA codes into OpenBlizz.
- Keep runtime caches and downloaded game data outside the repository.
  Choose an installation `--directory` outside the checkout and set
  `XDG_CACHE_HOME` to an external location if `HOME` is unavailable.
  Do not commit generated files or actual game content as test fixtures.
