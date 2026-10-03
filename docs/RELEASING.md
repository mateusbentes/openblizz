# Publishing a Linux Release

The prebuilt installer in `scripts/install-openblizz.sh` deliberately accepts
only a GitHub Release asset whose SHA-256 checksum is published beside it. The
workflow now builds and runs CTest on pushes to `main`, Pull Requests targeting
`main`, and manual dispatches; it publishes downloadable assets only for a
version tag beginning with `v`.

## First release

After the source changes are merged into `main` and pushed:

```bash
cd ~/Develop/openblizz
git pull --ff-only origin main
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DOPENBLIZZ_BUILD_TESTS=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure

git tag -a v0.1.0 -m "OpenBlizz v0.1.0"
git push origin v0.1.0
```

The tag starts `.github/workflows/release-linux.yml` on GitHub. It runs two
parallel jobs: x86_64 on `ubuntu-24.04` and AArch64 on `ubuntu-24.04-arm`.
Each runner installs the CMake dependencies, builds with GCC and runs CTest.
A final publish job collects both binaries, creates `SHA256SUMS`, and
publishes the two architecture-specific assets to the Release associated with
the tag. GitHub documents `ubuntu-24.04-arm` as a standard arm64 runner for
public repositories (see the [GitHub-hosted runners reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)).
The source tarballs generated automatically by GitHub are also available; the
installer uses only the two explicit assets.

If the repository has no Release before that first tag, the installer correctly
fails with a clear "release asset not found" message rather than installing an
unverified or locally guessed file.

## Later releases

Use a new, increasing tag for every public binary:

```bash
git checkout main
git pull --ff-only origin main
# inspect and test the changes
git tag -a v0.2.0 -m "OpenBlizz v0.2.0"
git push origin v0.2.0
```

Tags are immutable release inputs. Do not move an existing tag after the
workflow has published assets; create a new tag if the binary or source must
change. The installer URL for the newest release is:

```text
https://github.com/mateusbentes/openblizz/releases/latest/download/openblizz-linux-x86_64
https://github.com/mateusbentes/openblizz/releases/latest/download/openblizz-linux-aarch64
https://github.com/mateusbentes/openblizz/releases/latest/download/SHA256SUMS
```

A user can pin a known release with:

```bash
bash /tmp/install-openblizz.sh --version v0.1.0
```

## Release asset contract

The installer and workflow must continue to agree on these exact names:

| Asset | Contents |
|---|---|
| `openblizz-linux-x86_64` | executable, mode `0755`, built from `CMAKE_BUILD_TYPE=Release` |
| `openblizz-linux-aarch64` | executable, mode `0755`, built from `CMAKE_BUILD_TYPE=Release` on an arm64 runner |
| `SHA256SUMS` | two lines produced by `sha256sum`, containing both exact binary names |

These are OpenBlizz client binaries. They are not Proton, FEX, Steam Runtime,
or game binaries. Running Windows x86/x86-64 games on ARM64 remains dependent
on the user's separately installed ARM64 Proton+FEX/umu stack.

Do not replace the checksum with an unsigned download or a checksum hosted on
another domain. The script fails closed if `SHA256SUMS` is missing, malformed,
or does not match the downloaded binary.

## Adding another architecture

Keep each target isolated in the workflow matrix and publish a distinct asset.
Add a native runner or a reproducible cross-compilation toolchain, run the same
CTest suite where possible, generate one checksum line per asset, and then
update the installer architecture map and this contract in the same change.
Do not make the installer silently download a foreign binary.

## Security and rollback

- The installer uses HTTPS, requires TLS, downloads from GitHub Releases, and
  verifies SHA-256 before replacing `~/.local/bin/openblizz`.
- Installation is user-local and atomic: a failed download or checksum leaves
  the previous executable untouched.
- To roll back, rerun the installer with `--version` and a known-good tag.
- If a release is compromised, delete or edit the Release assets in GitHub,
  revoke the tag according to repository policy, and publish a new fixed tag.
- Never include cookies, browser profiles, account dumps, game files, or any
  proprietary Blizzard data in a Release asset.
