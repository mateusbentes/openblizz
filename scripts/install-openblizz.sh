#!/usr/bin/env bash
# Install the latest prebuilt OpenBlizz Linux binary for the current user.
# Usage from the repository or directly from GitHub:
#   curl -fsSL https://raw.githubusercontent.com/mateusbentes/openblizz/main/scripts/install-openblizz.sh | bash
set -Eeuo pipefail

readonly DEFAULT_REPOSITORY="mateusbentes/openblizz"
temporary_directory=

cleanup() {
    if [[ -n "$temporary_directory" ]]; then
        rm -rf -- "$temporary_directory"
    fi
}

trap cleanup EXIT

usage() {
    cat <<'EOF'
Install the latest prebuilt OpenBlizz binary for the current user.

Usage:
  install-openblizz.sh [options]

Options:
  --version TAG       Install a specific release tag instead of the latest one.
  --repository OWNER/REPO
                      Download from another GitHub repository.
  --install-dir DIR  Install into DIR (default: ~/.local/bin).
  --asset NAME       Override the release asset name.
  -h, --help         Show this help.

Environment equivalents:
  OPENBLIZZ_VERSION, OPENBLIZZ_REPOSITORY, OPENBLIZZ_INSTALL_DIR,
  OPENBLIZZ_ASSET

The release's SHA256SUMS file is required. The binary is never installed when
its checksum is missing or does not match.
EOF
}

fail() {
    printf 'OpenBlizz installer: %s\n' "$*" >&2
    return 1
}

download() {
    local url=$1
    local destination=$2
    if command -v curl >/dev/null 2>&1; then
        curl --fail --silent --show-error --location \
            --proto '=https' --tlsv1.2 "$url" --output "$destination"
    elif command -v wget >/dev/null 2>&1; then
        wget --https-only --quiet --output-document="$destination" "$url"
    else
        fail 'curl or wget is required; install one and retry'
    fi
}

main() {
    local repository=${OPENBLIZZ_REPOSITORY:-$DEFAULT_REPOSITORY}
    local version=${OPENBLIZZ_VERSION:-latest}
    local install_dir=${OPENBLIZZ_INSTALL_DIR:-"${HOME:-}/.local/bin"}
    local asset=${OPENBLIZZ_ASSET:-}

    while (($# > 0)); do
        case "$1" in
            --version)
                (($# >= 2)) || { fail '--version requires a release tag'; return 1; }
                version=$2
                shift 2
                ;;
            --repository)
                (($# >= 2)) || { fail '--repository requires OWNER/REPO'; return 1; }
                repository=$2
                shift 2
                ;;
            --install-dir)
                (($# >= 2)) || { fail '--install-dir requires a directory'; return 1; }
                install_dir=$2
                shift 2
                ;;
            --asset)
                (($# >= 2)) || { fail '--asset requires an asset name'; return 1; }
                asset=$2
                shift 2
                ;;
            -h|--help)
                usage
                return 0
                ;;
            *)
                fail "unknown option: $1 (use --help)"
                return 1
                ;;
        esac
    done

    [[ -n "${HOME:-}" ]] || { fail 'HOME is not set'; return 1; }
    [[ "$repository" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || {
        fail "invalid GitHub repository: $repository"
        return 1
    }

    if [[ -z "$asset" ]]; then
        local machine
        machine=$(uname -m)
        case "$machine" in
            x86_64|amd64) asset=openblizz-linux-x86_64 ;;
            *)
                fail "no prebuilt asset for CPU architecture: $machine (the public Release currently provides x86_64)"
                return 1
                ;;
        esac
    fi

    local base_url
    if [[ "$version" == latest ]]; then
        base_url="https://github.com/$repository/releases/latest/download"
    else
        base_url="https://github.com/$repository/releases/download/$version"
    fi

    temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/openblizz-install.XXXXXX")

    local binary="$temporary_directory/$asset"
    local checksums="$temporary_directory/SHA256SUMS"
    printf 'Downloading %s (%s) from %s...\n' "$asset" "$version" "$repository"
    if ! download "$base_url/$asset" "$binary"; then
        fail "release asset not found; create a GitHub Release with $asset (or use --version/--asset)"
        return 1
    fi
    if ! download "$base_url/SHA256SUMS" "$checksums"; then
        fail 'the release has no SHA256SUMS asset; refusing to install an unverified binary'
        return 1
    fi

    local expected actual
    expected=$(awk -v file="$asset" '$2 == file || $2 == "*" file { print $1 }' "$checksums" | head -n 1)
    [[ "$expected" =~ ^[[:xdigit:]]{64}$ ]] || {
        fail "SHA256SUMS does not contain a checksum for $asset"
        return 1
    }
    actual=$(sha256sum "$binary" | awk '{print $1}')
    if [[ "${expected,,}" != "${actual,,}" ]]; then
        fail "SHA-256 mismatch for $asset"
        printf 'expected: %s\nactual:   %s\n' "$expected" "$actual" >&2
        return 1
    fi

    mkdir -p -- "$install_dir"
    local target staged
    target="$install_dir/openblizz"
    staged="$install_dir/.openblizz.tmp.$$"
    install -m 0755 "$binary" "$staged"
    mv -f -- "$staged" "$target"

    printf 'Installed %s\n' "$target"
    case ":${PATH:-}:" in
        *:"$install_dir":*) ;;
        *)
            printf 'Note: %s is not currently in PATH. Add this to your shell profile:\n' "$install_dir"
            printf '  export PATH="%s:$PATH"\n' "$install_dir"
            ;;
    esac
    printf 'Run: openblizz --help\n'
}

main "$@"
