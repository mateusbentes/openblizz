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
- Community API discussion on owned-game discovery (Jan 2025): https://us.forums.blizzard.com/en/blizzard/t/fetching-a-users-owned-games/53759
- galaxy-integration-blizzard (MIT), reference for the account web endpoints, `titleId` mapping and `gameAccountStatus` semantics: https://github.com/FriendsOfGalaxy/galaxy-integration-blizzard
- Public JavaScript of https://account.battle.net/games (endpoint paths `/api/games-and-subs`, `/api/classic-games`, `X-XSRF-TOKEN` usage)

No source code from Battle.Net-Installer is included in this repository.

The documented OAuth user flow currently exposes `/userinfo` and selected
World of Warcraft profile resources. It does not document an owned-games,
entitlement, installer, or download endpoint. OpenBlizz does not claim that
the public developer catalog is an account inventory; its separate account-web
provider uses the user's own browser session against the account site's
undocumented JSON endpoints.
The `account-web` library provider uses `account.battle.net/api/games-and-subs`
and `/api/classic-games` with the user's own browser session cookies. These
endpoints are not documented for third parties (see the discussion above); the
provider is automatic after browser login but labelled experimental, and only
the response shape and the public `titleId` table were taken from the
MIT-licensed reference, not code.
Known `titleId` values: 21297 = StarCraft, 22323 = Warcraft III.
- Battle.net titleId = FourCC of the program code (observed on the account's own games-and-subs response: 22323 "W3", 21297 "S1", 21298 "S2", 5730135 "WoW", 1095647827 "ANBS"; corroborated by the MIT-licensed galaxy-integration-blizzard TITLE_ID_MAP).
- account.battle.net front-end bundle (public JavaScript): `TransactionsService` (`/api/transactions?regionId=N`, `/api/transactions/{orderId}/{invoiceId}`, fields `purchases[].productTitle`, `giftClaims[]`, `lineItems[].productTitle`) and `MyGamesService` (`/api/games-and-subs`, `/api/classic-games`, `/api/time-gated-games`, `/api/external-subs`). Observed 2026-10-02; undocumented, may change.
- us.shop.battle.net home page (public Next.js flight payload): product cards with `productPageName`, `slug`, `franchise`, `appGameCode`, `cmsId`; `/api/user-browsing-cards` (POST, logged-in eligibility), `/api/user-wishlist`. Observed 2026-10-02.
