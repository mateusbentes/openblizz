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
- Public product versions endpoint pattern: https://us.version.battle.net/v2/products/{product}/versions
- Public product CDN endpoint pattern: https://us.version.battle.net/v2/products/{product}/cdns
- CASC overview: https://wowdev.wiki/CASC
- TVFS format documentation: https://wowdev.wiki/TVFS
- BLTE format documentation: https://wowdev.wiki/BLTE

No source code from Battle.Net-Installer is included in this repository.

The documented OAuth user flow currently exposes `/userinfo` and selected
World of Warcraft profile resources. It does not document an owned-games,
entitlement, installer, or download endpoint. OpenBlizz does not scrape
private account pages or claim that the public catalog is an account inventory.
