# OpenBlizz

OpenBlizz is an independent, native, cross-platform client for authenticated users to download, install, update, verify, repair, and launch their owned Blizzard games on Linux. World of Warcraft is intentionally outside the initial product scope.

The initial supported catalog includes:

- Warcraft III: Reforged (`w3`)
- Warcraft III legacy/TFT (`w3-legacy-tft`)
- Warcraft II: Remastered (`w2r`)
- Warcraft II: Battle.net Edition (`w2bn`)
- Warcraft I: Remastered (`w1r`)
- Warcraft I legacy (`war1`)
- StarCraft: Remastered (`s1`)

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

List the products known by the current catalog:

```bash
./build/openblizz products
```

This is the public supported product catalog, not an account inventory. It
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

Authenticate directly through the official Battle.net OAuth page. Create an
OAuth client in the Blizzard Developer Portal, register its HTTPS redirect URI,
and keep the client secret outside the repository:

```bash
export OPENBLIZZ_CLIENT_SECRET='your-client-secret'
./build/openblizz login \
  --client-id 'your-client-id' \
  --redirect-uri 'https://your.example/callback' \
  --scope openid
unset OPENBLIZZ_CLIENT_SECRET
```

OpenBlizz opens the authorization URL, then asks you to paste the callback URL
from the browser. The resulting access token is stored at
`~/.config/openblizz/oauth-token.json` with owner-only permissions. The client
secret is never stored by OpenBlizz.

The legacy Battle.net UI remains available only as an optional fallback:

```bash
./build/openblizz agent-login \
  --prefix "$HOME/Games/openblizz/battlenet" \
  --backend auto
./build/openblizz auth-status --prefix "$HOME/Games/openblizz/battlenet"
./build/openblizz agent-info --prefix "$HOME/Games/openblizz/battlenet" --product w3
```

After OAuth login, inspect the authenticated identity:

```bash
./build/openblizz account
```

## Account library providers

OpenBlizz keeps account-library state separately from the public product
catalog. The OAuth identity provider confirms the account, but the documented
Battle.net OAuth API does not currently expose an owned-games endpoint. A scan
without another provider therefore records products as `unknown`, never as
`not_owned`:

```bash
./build/openblizz library scan
./build/openblizz library list
```

The manual provider is explicit and does not claim proof of ownership:

```bash
./build/openblizz library add w3
./build/openblizz library remove w3
```

### Account web session provider (experimental, undocumented API)

The Battle.net account management site (`account.battle.net`) lists the
licenses attached to your account through the same internal JSON endpoints its
own web page uses (`/api/games-and-subs` and `/api/classic-games`). OpenBlizz
can read them with the cookies of *your own* browser session. No password is
ever requested, and the cookies are kept in memory only.

1. Log in at https://account.battle.net/games in your browser.
2. Export the cookies for `account.battle.net` as a Netscape `cookies.txt`
   (for example with the "Get cookies.txt LOCALLY" or "cookies.txt" browser
   extensions). Keep that file private; it grants access to your account page.
3. Run:

```bash
./build/openblizz library scan --cookie-file ~/Downloads/cookies.txt
./build/openblizz library list
```

Products returned with a `Good`, `Free`, `Inactive` or similar status become
`owned`; `Trial` becomes `not_owned`; products not returned stay `unknown`.
Add `--dump PATH` to save the raw responses (owner-only permissions) so that
unmapped `titleId` values can be added to the catalog mapping.

These endpoints are not part of Blizzard's documented developer API. They can
change without notice and their use by third-party tools may fall outside
Blizzard's terms; the provider is therefore opt-in and clearly labelled
`account-web` in the library file.

An experimental provider can be enabled only by explicitly configuring an
HTTPS endpoint. OpenBlizz sends the OAuth Bearer token to that endpoint, so do
not configure an endpoint you do not trust:

```bash
# Illustrative only: this hostname does not exist.
./build/openblizz library scan \
  --entitlement-url 'https://your-authorized-service.example/entitlements'
```

Do not run that illustrative command unchanged. Until a real, authorized
endpoint exists, use `library scan` without `--entitlement-url` and keep the
products in the `unknown` state.

The experimental adapter accepts a deliberately small JSON family such as
`{"products":[{"product":"w3","owned":true}]}`. It is not a hardcoded
private Blizzard endpoint and does not pretend that an undocumented response
is a stable Blizzard API. Missing products remain `unknown` rather than being
classified as `not_owned`. Library state is stored with owner-only permissions
at `~/.local/state/openblizz/library.json` unless `--library-file` is used.

The native installer can use the OAuth identity without starting Battle.net:

```bash
./build/openblizz install w3 \
  --directory "$HOME/Games/Warcraft3" \
  --locale enUS --jobs 4
```

This verifies the OAuth identity and downloads public TACT/NGDP content. Since
Blizzard does not document a public entitlement endpoint, OpenBlizz prints a
warning and does not claim that ownership was independently verified.

For a one-off token, an environment variable can be used instead of the token
file:

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

OpenBlizz never asks for or stores a Battle.net password. The primary login
opens the official OAuth page in the user's browser, validates the callback
state, exchanges the one-time code through `/token`, and calls `/userinfo`.
The local Battle.net UI and Agent are not required for this identity flow.

The local Battle.net UI and Agent are also not required for the OAuth-backed
installer. The Agent remains an optional compatibility path when its private
local authority is needed.

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
