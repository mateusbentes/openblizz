# Technical sources

OpenBlizz is an independent implementation. The following public sources are
used as protocol and format references:

- Blizzard Battle.net developer portal: https://develop.battle.net/documentation
- Battle.net OAuth API reference: https://develop.battle.net/documentation/battle-net/oauth-apis
- OAuth authorization code flow: https://develop.battle.net/documentation/guides/using-oauth/authorization-code-flow
- OAuth/OIDC endpoint discovery: https://develop.battle.net/documentation/guides/using-oauth/oidc-endpoints
- Blizzard Developer API Terms of Use: https://www.blizzard.com/legal/a2989b50-5f16-43b1-abec-2ae17cc09dd6/blizzard-developer-api-terms-of-use
- NGDP/TACT overview and product tables: https://wowdev.wiki/TACT
- Public Ribbit V2 summary: https://us.version.battle.net/v2/summary
- Public Warcraft III product versions endpoint: https://us.version.battle.net/v2/products/w3/versions
- Public Warcraft III product CDN endpoint: https://us.version.battle.net/v2/products/w3/cdns
- CASC overview: https://wowdev.wiki/CASC
- TVFS format documentation: https://wowdev.wiki/TVFS
- BLTE format documentation: https://wowdev.wiki/BLTE
- Salsa20 specification (public primitive reference): https://cr.yp.to/snuffle/spec.pdf
- ARC4 test vectors and specification reference: https://www.rfc-editor.org/rfc/rfc6229
- Community API discussion on owned-game discovery (Jan 2025): https://us.forums.blizzard.com/en/blizzard/t/fetching-a-users-owned-games/53759
- galaxy-integration-blizzard (MIT), reference for the account response shape, `titleId` mapping and `gameAccountStatus` semantics: https://github.com/FriendsOfGalaxy/galaxy-integration-blizzard

No source code from Battle.Net-Installer is included in this repository.

The documented OAuth user flow currently exposes `/userinfo` and selected
World of Warcraft profile resources. It does not document an owned-games,
entitlement, installer, or download endpoint. OpenBlizz does not claim that
the public developer catalog is an account inventory; its separate account-web
provider uses the user's own browser session against the account site's
undocumented JSON endpoints.

## Account-page JSON observations (not a public API)

OpenBlizz uses the authenticated browser session for these account JSON
endpoints:

- `https://account.battle.net/api/games-and-subs` — game accounts; `titleId` is
  the big-endian FourCC of the NGDP product code.
- `https://account.battle.net/api/classic-games` — classic CD-key records.
- `https://account.battle.net/api/transactions?regionId=1` (and the same path
  with `regionId=2` and `regionId=3`) — purchase history used for entitlement
  mapping.

These responses are observations from the account page rather than a stable
third-party contract. Observed `titleId` values include `22323` (`W3`), `21297`
(`S1`), `21298` (`S2`), `5730135` (`WoW`) and `1095647827` (`ANBS`); the
response shape and public mapping table were corroborated by the MIT-licensed
reference above, not copied from it.

The Netscape cookie jar used for these requests is a **bearer credential**:
protect it like a credential and never attach it to a bug report. `library scan --dump` writes raw account JSON responses and may contain sensitive account and
purchase data; redact it before sharing.

## Third-party download observations (2026-10-10)

The public `wlby` product config identifies Crash Bandicoot 4 and containerless
NGDP, not Call of Duty. The public plan resolved 1,123 named install files;
gameplay and its online runtime were not tested. The selected public `lyra`
build still has an empty install manifest, despite its readable public
KeyRing. Sources, dates and exact limits are collected in
[docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).

The generic download resolver and offline integration tests use this project's
existing IN/EN/BLTE parsers, public metadata observations and synthetic content.
No Battle.Net-Installer or cascette source was used for this change. No game
files, live account responses, session cookies or private keys are included in
the test fixtures.
