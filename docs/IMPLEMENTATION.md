# Implementation notes

The repository is intentionally split into protocol, format, content and
runner layers.

`Catalog` discovers current product versions and CDN hosts from Ribbit V2,
then validates object hashes returned from the CDN. `BlteDecoder` supports the
uncompressed, zlib and LZ4 block modes used by configuration and manifest
objects. `EncodingIndex` resolves install-manifest content keys to one or more
encoding keys. `ArchiveIndex` resolves archive-backed encoding keys to offsets;
the installer reads those BLTE ranges without downloading whole multi-hundred-
megabyte archives. `Installer` selects Windows/x86_64/locale/Release tags,
downloads objects into a content-addressed cache, writes atomically through
`.part` files, verifies MD5 content keys, and repairs missing or corrupted
files.

OAuth is the primary authentication path. `oauth-login` opens the official
Battle.net authorization page, validates the state returned to a manually
pasted callback, exchanges the one-time code at `/token`, stores only the
short-lived access token in an owner-only file, and calls `/userinfo`. It never
collects a password or MFA value. The local Battle.net UI and Proton are not
required for this flow.

The Battle.net Agent remains an optional legacy fallback for installation
operations that need its local authority. OpenBlizz probes `/agent`, keeps its
authorization token only in memory, and can check `/version/<uid>`. Blizzard's
public documentation does not define a third-party entitlement API, so the
Agent remains the authority when it is used; OpenBlizz does not pretend a local
file proves ownership.

`openblizz account` consumes a bearer token from the owner-only OAuth token file
or an environment variable and calls the documented
`https://oauth.battle.net/userinfo` endpoint. `openblizz agent-info` reports a
recursively redacted `/agent` response and can query `/version/<product>` for
troubleshooting. The `products` command is intentionally a supported public
catalog, not an account-owned inventory.

The current format implementation targets the public IN/DL/EN manifest path,
including CDN archive indexes and range reads. Products that expose only
TVFS/VFS mappings or encrypted content must fail loudly with a capability
error until their fixtures and key-handling provider are implemented. No game
data is checked into this repository; fixtures must be synthetic or generated
locally by the developer.
