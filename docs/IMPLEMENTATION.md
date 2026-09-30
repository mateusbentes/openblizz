# Implementation notes

The repository is intentionally split into protocol, format, content and
runner layers.

`Catalog` discovers current product versions and CDN hosts from Ribbit V2,
then validates object hashes returned from the CDN. `BlteDecoder` supports the
uncompressed, zlib and LZ4 block modes used by configuration and manifest
objects. `EncodingIndex` resolves install-manifest content keys to encoding
keys. `Installer` selects Windows/x86_64/locale/Release tags, downloads
objects into a content-addressed cache, writes atomically through `.part`
files, verifies MD5 content keys, and repairs missing or corrupted files.

Authentication is deliberately a hand-off to the official Battle.net UI in a
user-controlled Proton/Wine prefix. Before content-changing operations,
OpenBlizz probes the local Agent `/agent` endpoint, keeps the returned
authorization token only in memory, and checks `/version/<uid>`. OpenBlizz
does not collect passwords or MFA values. Blizzard's public documentation does
not define a third-party entitlement API, so the official Agent remains the
authority and can reject an operation; OpenBlizz does not pretend a local
file proves ownership.

`openblizz account` consumes a caller-supplied OAuth bearer token only from an
environment variable and calls the documented `https://oauth.battle.net/userinfo`
endpoint. `openblizz agent-info` reports a recursively redacted `/agent`
response and can query `/version/<product>` for troubleshooting. Neither
command stores tokens. The `products` command is intentionally a supported
public catalog, not an account-owned inventory.

The current format implementation targets the public IN/DL/EN manifest path.
Products that expose only TVFS/VFS mappings or encrypted content must fail
loudly with a capability error until their fixtures and key-handling provider
are implemented. No game data is checked into this repository; fixtures must
be synthetic or generated locally by the developer.
