# OpenBlizz

OpenBlizz is an independent, native, cross-platform client for authenticated users to download, install, update, verify, repair, and launch their owned Warcraft-series games on Linux. World of Warcraft is intentionally outside the initial product scope.

The first supported family is:

- Warcraft III: Reforged (`w3`)
- Warcraft III legacy/TFT (`w3-legacy-tft`)
- Warcraft II: Remastered (`w2r`)
- Warcraft II: Battle.net Edition (`w2bn`)
- Warcraft I: Remastered (`w1r`)
- Warcraft I legacy (`war1`)

OpenBlizz does not distribute Battle.net, Agent.exe, game files, private keys, or proprietary Blizzard assets.

## Status

This repository is an early, buildable implementation. The NGDP/Ribbit catalog, CDN configuration retrieval, BLTE decoding, install/download manifest parsing, local caching, verification primitives, authentication hand-off, and Proton/Wine runner are implemented incrementally. Product-specific CASC/TVFS installation and entitlement backends are deliberately covered by tests and explicit capability checks rather than silently claiming unsupported behavior.

## Build on Debian/Ubuntu

```bash
sudo apt install cmake g++ pkg-config \
  libcurl4-openssl-dev libssl-dev zlib1g-dev liblz4-dev \
  nlohmann-json3-dev

cmake -S . -B build -DOPENBLIZZ_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## CLI examples

List the Warcraft products known by the current catalog:

```bash
./build/openblizz products
```

This is the public supported Warcraft catalog, not an account inventory. It
must not be interpreted as a list of games owned by the logged-in user.

Read the current build and CDN metadata:

```bash
./build/openblizz versions w3 --region us
./build/openblizz cdns w3 --region us
```

Inspect the current build manifests without downloading game content:

```bash
./build/openblizz plan w3 --region us --locale enUS
```

After signing in through the official UI, an authenticated download is
explicitly tied to the local Battle.net Agent session:

```bash
./build/openblizz install w3 \
  --prefix "$HOME/Games/openblizz/battlenet" \
  --directory "$HOME/Games/Warcraft3" \
  --region us --locale enUS --jobs 4
```

Authenticate through the official Battle.net application without entering credentials into OpenBlizz:

```bash
./build/openblizz login --prefix "$HOME/Games/openblizz/battlenet"
./build/openblizz auth-status --prefix "$HOME/Games/openblizz/battlenet"
./build/openblizz agent-info --prefix "$HOME/Games/openblizz/battlenet" --product w3
```

Login uses `--backend auto` by default: it prefers `umu-run` for Proton and
falls back to `wine` when `umu-run` is unavailable. To use Proton explicitly,
install [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher)
and keep `--backend umu`; to use system Wine, pass `--backend wine`.

The documented Blizzard OAuth `/userinfo` endpoint can identify an account
when an access token from a registered OAuth client is supplied in memory:

```bash
export OPENBLIZZ_OAUTH_TOKEN='do-not-save-this-in-the-repository'
./build/openblizz account
unset OPENBLIZZ_OAUTH_TOKEN
```

Run a Windows game executable through Proton/umu:

```bash
./build/openblizz launch \
  --directory "$HOME/Games/Warcraft3" \
  --exe "Warcraft III.exe" \
  --prefix "$HOME/Games/openblizz/warcraft3" \
  --proton GE-Proton
```

## Authentication boundary

OpenBlizz never asks for or stores a Battle.net password. The Linux backend
opens the official Battle.net client through `umu-run`/Wine so the user can
complete login and MFA in the official UI. Before `install`, `update`, or
`repair`, OpenBlizz probes the local Agent on localhost, obtains its ephemeral
authorization in memory, and checks the requested product version endpoint.
If no authenticated Agent session is found, the operation is refused.

The public Agent protocol does not expose a documented third-party entitlement
API, so OpenBlizz does not claim that a local file or version response alone
proves ownership. The official Agent remains the authority and may reject a
product operation. This is safer than collecting credentials or silently
downloading an unowned product.

The public OAuth documentation currently lists `/userinfo` and World of
Warcraft profile resources for user-authorized requests. It does not list an
owned-games, entitlement, installer, or download endpoint. OpenBlizz therefore
does not scrape private Battle.net account pages or invent an account inventory.

## Technical sources

- [Blizzard Battle.net developer portal](https://develop.battle.net/documentation)
- [TACT/NGDP documentation](https://wowdev.wiki/TACT)
- [Ribbit product endpoints](https://us.version.battle.net/v2/summary)
- [CASC/TVFS format documentation](https://wowdev.wiki/CASC)

## License and trademarks

The OpenBlizz source is licensed under Apache-2.0. OpenBlizz is an independent project and is not affiliated with or endorsed by Blizzard Entertainment. Warcraft and Battle.net are trademarks of their respective owners. See [NOTICE](NOTICE) and [TRADEMARKS.md](TRADEMARKS.md).
