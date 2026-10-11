# Downloading third-party Battle.net games

OpenBlizz can attempt downloads of non-Blizzard games distributed through the same public NGDP protocol. The installer does not need a separate store-specific downloader when the build provides supported install/encoding manifests and a mapping from content keys to file paths. This is **experimental download support**, not a promise that every Battle.net purchase is installable or playable without its official runtime.

Three independent checks matter: the account scan must recognize ownership, the selected build must expose usable installation data, and the installed game must work with its authentication, dependencies and Proton/Wine runtime. OpenBlizz does not bypass the last check.

## Coverage policy: protocol support, not a fixed game whitelist

The goal is to download as many account-owned Battle.net PC games as the
supported distribution formats allow, including non-Blizzard titles. New
account-derived products with explicit NGDP codes use the generic pipeline;
they do not need a new downloader for each publisher. A storefront listing or
successful ownership scan alone is not proof that a usable download exists.

The pipeline handles named IN files, EN content-to-encoding mappings, loose
CDN objects, archive indexes and HTTP Range extraction, public KeyRing BLTE
decryption, and the existing TVFS/CASC storage path. For the IN/EN manifests,
planning tries the loose object first. If all announced hosts fail to return
it, the planner loads the announced archive indexes and tries the exact EKey
by range. Both paths must pass BLTE decoding and the build-config CKey check.
Indexes are loaded once per plan and reused from the local index cache.

A successfully fetched but empty IN is still rejected immediately, before
fetching EN or archive indexes. Searching archives for the same already-valid
empty IN would not create a file mapping. This preserves the fast diagnostic
for the currently inspected `lyra` build.

`plan` does not download game payloads or authorize an installation. It may
fetch metadata, manifests and indexes. A successful plan demonstrates file
selection, not full-file availability, a completed installation or gameplay.
`install`/`update`/`repair` still require account-detected ownership for third
parties. Download support is not intentionally restricted to Crash 4, but
unestablished root/database formats, missing keys or server authorization can
prevent other products from working. Curated Call of Duty gates and legacy
non-NGDP installer limitations remain explicit exceptions.

No extra purchase is required for development: synthetic tests validate the
pipeline without game files. A real account-owned installation remains the
next level of validation; do not buy a game merely to fill a test matrix.
For source builds that have not been installed into PATH, replace `openblizz`
with `./build/openblizz` in the examples below.

## First supported download path: Crash Bandicoot 4

The NGDP code `wlby` identifies **Crash Bandicoot 4: It's About Time**, not Call of Duty. Its public product configuration names `CrashBandicoot4.exe` and declares `containerless ngdp`. The curated catalog now puts it under Third-party Battle.net titles and enables the existing downloader experimentally.

On **2026-10-10**, `plan wlby --locale ptBR` resolved build `4062021` / `1.1.04062021`: 1,123 named files totaling 24,980,571,164 decoded bytes, including Windows executables and Unreal `.pak` files. The install-manifest content hash was verified. This was a metadata/manifest test, **not a complete game installation or successful gameplay test**. No game purchase is needed to run the public plan command.

```bash
# Inspect the build before purchasing anything or allocating game disk space.
openblizz plan wlby --region us --locale ptBR

# Download only if the account scan already detects that you own this title.
openblizz login
openblizz library list
openblizz install wlby --directory "$HOME/Games/Crash4" --locale ptBR --jobs 4
openblizz verify wlby --directory "$HOME/Games/Crash4" --locale ptBR

# Later, use the same product id, directory and locale.
openblizz update wlby --directory "$HOME/Games/Crash4" --locale ptBR --jobs 4
openblizz repair wlby --directory "$HOME/Games/Crash4" --locale ptBR --jobs 4
```

The Windows version's official announcement requires an Internet connection, and the product config declares `supports_offline: false`. Downloading the files does not supply a Battle.net runtime token, disable DRM, or establish Proton compatibility. Use the generic `launch` command only as a runtime test; see [RUNTIME.md](RUNTIME.md). OpenBlizz does not copy the account's browser cookies into a game's process.

## New games discovered automatically

For purchases not matched by the curated catalog, `library scan` tries to match a public storefront card by purchase title, card name or product slug. A match retains a stable local id such as `thirdparty-exg`, the display name, purchase evidence and two structured fields:

- `ngdp_product`: the card's explicit, lower-case `appGameCode` (when present).
- `shop_slug`: the public `/product/...` path, used for storefront cross-referencing, **not** as a download URL.

A recognized account-owned entry with a usable `ngdp_product` can now go through `plan`, `install`, `update`, `verify`, `repair` and the TVFS inspection commands without first adding a hard-coded product descriptor. The local id remains stable while requests use the real NGDP code. The code must contain only lower-case ASCII letters, digits, underscores or hyphens, and a curated unsupported target cannot be unblocked through a dynamic alias.

```bash
openblizz library scan
# Replace this synthetic example with the Id actually printed by your library.
openblizz plan thirdparty-exg --locale ptBR
openblizz install thirdparty-exg --directory "$HOME/Games/ExampleGame" --locale ptBR
openblizz verify thirdparty-exg --directory "$HOME/Games/ExampleGame" --locale ptBR
```

A slug-only match remains ownership-only: OpenBlizz never guesses an NGDP code from the title, slug, or `reason` text. Existing dynamic entries written by older releases lack the new structured fields; run `library scan` again to refresh them. `--library-file PATH` can select the same library for metadata and maintenance commands.

Third-party `install`, `update` and `repair` require both a valid saved browser session and `owned` evidence detected from the account. A manual library entry, an unknown license, or `--force` does not replace that requirement. The library is a local cache of undocumented account responses, not an independently signed entitlement certificate. Any server-side authorization remains enforced by the provider.

## The Witcher 3 Remastered (`lyra`)

The official Battle.net product exists, and OpenBlizz recognizes the known purchase title as `lyra`. Nevertheless, the **public build inspected on 2026-10-10** (`1048522`, version `5.00`) exposes `feature-placeholder-feature = true`, a zero root hash and an empty install manifest. The public KeyRing permits that manifest to be decrypted, but it does not add any named install files.

```bash
openblizz plan lyra --region us --locale ptBR
# For the inspected build:
# OpenBlizz error: product lyra has an empty install manifest; the current CDN build is metadata-only
```

This is a statement about that selected public build and OpenBlizz's current parser, not a claim that the official game cannot be downloaded through Battle.net. The build advertises `build-file-db`; its direct encoded-object URL returned 404 on the US/EU CDN hosts during the probe, and its format was not established. It could be archived, so the direct failure is not proof of absence. The public product config provides executable/locales/dependency hints, but does not provide the missing file mapping.

The 2026-10-10 follow-up also parsed the advertised loose-object `file-index`
(4,148 bytes, 182 entries); neither the current nor the tested historical
database EKey appeared. The 402 individual archive indexes could not all be
checked within the bounded probe because of network timeouts. This is not
proof that the database is absent from the archives. Neither a readable
database object nor a public specification for its named-file layout was
established, so the alternative is still unimplemented.

There is no speculative `build-file-db` parser in this implementation. OpenBlizz will not invent paths from download-manifest hashes or call a hash-only cache a playable installation. If a later build publishes supported complete named-file manifests, rerun `plan` to evaluate it. Buying the game does not itself fix the missing manifest path, and you do not need to buy more games to validate the generic downloader.

## Validation and limits

`openblizz_thirdparty_tests` uses synthetic data and an in-memory transport, never Blizzard game files or live credentials. It exercises dynamic id resolution, IN/EN planning, plain/encrypted BLTE file download, repeated installs without another content request, corruption detection and repair from the decoded cache. It also checks empty manifests, bad manifest hashes, empty selections, conflicting output paths, invalid product codes, missing appGameCode, unowned entries and blocked aliases.

The same suite exercises a synthetic archive index, archive-range extraction
and direct-object fallback when an archive request fails. This verifies the
transport contract through dependency injection, not live libcurl networking
or real-game entitlement services.

On Linux, `openblizz_http_tests` separately uses real libcurl with a temporary
loopback HTTP server. It checks `206`, exact `Content-Range`/body size,
case-insensitive headers, redirects, incorrect/absent ranges, oversized and
short bodies, and offset overflow. Range buffering is capped at the requested
size, and HTTP content compression is not accepted for encoded archive
offsets. These are local transport tests, not live CDN/TLS or entitlement tests.

Additional synthetic regressions put IN and EN manifests only in an archive:
planning resolves them by range, installation and verification succeed, and
the index is not fetched twice. Missing EKeys and corrupt decoded manifests
fail explicitly. A valid loose empty IN is checked to make zero archive-index
or EN requests. This does not establish that a particular live game's
manifests are archived.

It also checks a synthetic CASC storage plan: encrypted encoded objects are
stored and reused, deep verification succeeds, and `.build.info` uses the real
NGDP code rather than the local `thirdparty-*` id. This is a writer test, not
an end-to-end real-game TVFS installation. Generic account products matched
against the Ribbit summary also retain their code explicitly during scan;
the resolver does not infer missing codes from local ids.

Selected install/encoding manifest content hashes are checked before use. Selected output paths are normalized, identical duplicate files are processed once, and conflicting contents for the same case-insensitive Windows path are rejected before parallel writes. CASC resumability is still handled by the existing storage writer and journals. Partial HTTP objects are not a full byte-range resume guarantee; successfully completed files and objects are reused.

Temporary writes have unique process/worker names so distinct files sharing
one content/cache key do not race over a single `.part` file. A parallel
synthetic regression installs 16 such files and verifies every output.

The current install-file path buffers encoded/decoded objects in memory and
uses a decoded cache. Large containerless files can therefore require substantial
RAM and additional disk space; reduce `--jobs` (for example to 1) on small
machines. Streaming very large files is not implemented by this change.

Call of Duty remains gated separately; this change does not validate its content/key/runtime combination or anti-cheat support. Google Play/App Store packages, Steam/GOG downloads, external store credentials, private key delivery and authentication/DRM bypass are not implemented. Games already installed elsewhere may be launched using the generic runner, subject to their own compatibility and prerequisites.

## Public references

- [TACT: CDN paths, build/encoding/install manifests and public KeyRing](https://wowdev.wiki/TACT)
- [Crash 4 versions](https://us.version.battle.net/v2/products/wlby/versions) and [CDNs](https://us.version.battle.net/v2/products/wlby/cdns)
- [Crash 4 public build config](https://us.cdn.blizzard.com/tpr/wallaby/config/a3/82/a3821180eceeda683cf6c7794172f390) and [product config](https://us.cdn.blizzard.com/tpr/configs/data/cb/84/cb84dc5ee397f23eab8e23511534eca8)
- [Official Crash 4 Battle.net launch and connection requirements](https://news.blizzard.com/en-us/article/23652233/crash-bandicoottm-4-its-about-time-available-now-on-battle-net)
- [Lyra versions](https://us.version.battle.net/v2/products/lyra/versions), [public build config](https://us.cdn.blizzard.com/tpr/lyra/config/53/91/539107277edad4a35c928ba8d765bd5b) and [product config](https://us.cdn.blizzard.com/tpr/configs/data/b7/ac/b7aca940133ea074c9ff23a8cf32d419)
- [Official Witcher 3 Remastered Battle.net announcement](https://news.blizzard.com/en-us/article/24301608/the-witcher-3-wild-hunt-remastered-arrives-on-battle-net)

Live build hashes and public endpoint schemas can change. No Battle.net installer or third-party downloader source was used for this implementation.
