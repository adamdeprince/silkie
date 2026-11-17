#!/bin/bash
# Install Silkie and unmodified Iosevka fonts from their package sources.
set -euo pipefail

die() { printf 'Silkie: %s\n' "$*" >&2; exit 1; }
confirm_sources() {
    printf '%s\n' "$@" >&2
    printf 'Continue? [y/N] ' >&2
    local answer
    if ! { read -r answer </dev/tty; } 2>/dev/null; then
        die 'Source setup requires approval in an interactive terminal. No sources were changed.'
    fi
    case "$answer" in y|Y|yes|YES) ;; *) die 'Cancelled. No sources were changed.' ;; esac
}
as_root() {
    if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi
}

main() {
    local system
    system=$(uname -s)
    case "$system" in
        Darwin) install_macos ;;
        Linux) install_linux ;;
        *) die "Unsupported operating system: $system" ;;
    esac
    printf '\nSilkie and Iosevka installed. Run silkie to launch it, or silkie-kitten for companion tools.\n'
}

install_cask() {
    local cask=$1 name=${1##*/}
    if brew list --cask "$name" >/dev/null 2>&1; then
        HOMEBREW_NO_INSTALL_CLEANUP=1 brew upgrade --cask "$cask"
    else
        HOMEBREW_NO_INSTALL_CLEANUP=1 brew install --cask "$cask"
    fi
}

download_iosevka_license() {
    # Keep the complete copyright notice and OFL with the unmodified font files.
    curl -fsSL "https://raw.githubusercontent.com/be5invis/Iosevka/v$1/LICENSE.md" -o "$2"
}

install_iosevka_macos() {
    install_cask font-iosevka
    local version fonts license
    version=$(brew list --cask --versions font-iosevka | awk '{print $2}')
    case "$version" in ''|*[!0-9.]*) die 'Cannot determine the installed Iosevka version.' ;; esac
    fonts="$HOME/Library/Fonts"
    mkdir -p "$fonts"
    license=$(mktemp "$fonts/.iosevka-license.XXXXXX")
    if ! download_iosevka_license "$version" "$license"; then
        rm -f "$license"
        die 'Could not install the Iosevka license notice.'
    fi
    chmod 0644 "$license"
    mv -f "$license" "$fonts/Iosevka-LICENSE.md"
}

install_macos() {
    [ "$(uname -m)" = arm64 ] || die 'The macOS package requires Apple Silicon.'
    [ "$(sw_vers -productVersion | cut -d. -f1)" -ge 26 ] || die 'The macOS package requires macOS 26 or newer.'
    if ! command -v brew >/dev/null 2>&1 && [ -x /opt/homebrew/bin/brew ]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    fi
    if ! command -v brew >/dev/null 2>&1; then
        confirm_sources \
            'Silkie requires Homebrew. We will install Homebrew from brew.sh and add' \
            'the Goblin Reactor tap at https://brew.goblinreactor.com/homebrew-tap.git.' \
            'This trusts Goblin Reactor packages and future package updates.'
        local bootstrap
        bootstrap=$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)
        /bin/bash -c "$bootstrap"
        eval "$(/opt/homebrew/bin/brew shellenv)"
    elif ! brew tap | /usr/bin/grep -qx 'goblinreactor/tap'; then
        confirm_sources \
            'We will add the Goblin Reactor Homebrew tap:' \
            'https://brew.goblinreactor.com/homebrew-tap.git' \
            'This trusts Goblin Reactor packages and future package updates.'
    fi
    if ! brew tap | /usr/bin/grep -qx 'goblinreactor/tap'; then
        brew tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git
    fi
    brew update
    install_cask goblinreactor/tap/silkie
    dequarantine_silkie_macos
    install_iosevka_macos
}

dequarantine_silkie_macos() {
    # The build is ad-hoc signed and not notarized, so Gatekeeper refuses a
    # quarantined copy. Find the app through the cask's silkie symlink, which
    # honours any --appdir in HOMEBREW_CASK_OPTS.
    local launcher app
    launcher=$(readlink "$(brew --prefix)/bin/silkie" || true)
    app=${launcher%/Contents/MacOS/silkie}
    if [ -z "$launcher" ] || [ "$app" = "$launcher" ] || [ ! -d "$app" ]; then
        printf 'Could not locate silkie.app. If macOS blocks first launch, approve Silkie in System Settings > Privacy & Security.\n' >&2
        return
    fi
    if /usr/bin/xattr -dr com.apple.quarantine "$app"; then
        printf 'Cleared the macOS quarantine flag from %s.\n' "$app"
    else
        printf 'Could not clear the quarantine flag from %s. Run: xattr -dr com.apple.quarantine "%s"\n' "$app" "$app" >&2
    fi
}

install_iosevka_linux() {
    local temporary=$1 fonts version=34.9.1
    # SHA-256 published on the official v34.9.1 GitHub release.
    local checksum=7a77a3dfa0e2d713a8920a30624863e23df2867a1f55a5e3d6d23dad07c14c65
    if [ "$(id -u)" = 0 ]; then
        fonts=/usr/local/share/fonts/iosevka
    else
        fonts="${XDG_DATA_HOME:-$HOME/.local/share}/fonts/iosevka"
    fi
    printf 'Installing unmodified Iosevka %s from its official GitHub release (SIL OFL 1.1).\n' "$version"
    curl -fsSL "https://github.com/be5invis/Iosevka/releases/download/v$version/PkgTTF-Iosevka-$version.zip" \
        -o "$temporary/iosevka.zip"
    printf '%s  %s\n' "$checksum" "$temporary/iosevka.zip" | sha256sum -c -
    download_iosevka_license "$version" "$temporary/Iosevka-LICENSE.md"
    unzip -q "$temporary/iosevka.zip" -d "$temporary/iosevka"
    find "$temporary/iosevka" -type f -name '*.ttf' -print0 >"$temporary/font-files"
    [ -s "$temporary/font-files" ] || die 'The Iosevka archive contains no TrueType fonts.'
    install -d -m 0755 "$fonts"
    install -m 0644 "$temporary/Iosevka-LICENSE.md" "$fonts/Iosevka-LICENSE.md"
    local font
    while IFS= read -r -d '' font; do
        install -m 0644 "$font" "$fonts/"
    done <"$temporary/font-files"
    fc-cache -f "$fonts"
}

install_linux() {
    [ -r /etc/os-release ] || die '/etc/os-release is required.'
    . /etc/os-release
    local suite arch key source temporary fingerprint configured list
    suite=${VERSION_CODENAME:-}
    arch=$(dpkg --print-architecture)
    case "${ID:-}:$suite:$arch" in
        ubuntu:resolute:amd64|ubuntu:resolute:arm64|ubuntu:noble:amd64|debian:trixie:amd64|kylin:kylin:loongarch64) ;;
        *) die "No Silkie package is published for ${ID:-unknown} $suite ($arch)." ;;
    esac
    key=/etc/apt/keyrings/goblinreactor-archive-keyring.gpg
    source=/etc/apt/sources.list.d/goblinreactor.sources
    temporary=$(mktemp -d)
    # Expand the path now: local variables no longer exist when EXIT runs.
    trap "rm -rf -- '$temporary'" EXIT
    printf '%s\n' 'Types: deb deb-src' 'URIs: https://apt.goblinreactor.com/' \
        "Suites: $suite" 'Components: main' "Architectures: $arch" \
        "Signed-By: $key" >"$temporary/goblinreactor.sources"
    configured=false
    if [ -r "$key" ] && command -v gpg >/dev/null 2>&1; then
        fingerprint=$(gpg --batch --show-keys --with-colons "$key" 2>/dev/null | awk -F: '$1 == "fpr" {print $10; exit}')
        if [ "$fingerprint" = B7B61E8CB85A9270530364F23273CB62402FC793 ]; then
            if cmp -s "$source" "$temporary/goblinreactor.sources"; then configured=true; fi
            # Reuse the repository's older one-line format without adding a duplicate.
            for list in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
                [ -r "$list" ] || continue
                if awk -v suite="$suite" -v key="$key" -v arch="$arch" '
                    $1 == "deb" && index($0, "signed-by=" key) && index($0, "arch=" arch) {
                        for (i=1; i<NF; i++)
                            if ($i ~ /^https:\/\/apt[.]goblinreactor[.]com\/?$/ && $(i+1) == suite && $(i+2) == "main") found=1
                    } END {exit !found}' "$list"; then configured=true; fi
            done
        fi
    fi
    if [ "$configured" = false ]; then
        confirm_sources \
            "We will set up the Goblin Reactor APT source for $suite ($arch):" \
            'https://apt.goblinreactor.com/' \
            "Repository file: $source; signing key: $key" \
            'This trusts Goblin Reactor packages and future package updates.'
        as_root apt-get update
        as_root apt-get install -y ca-certificates curl gnupg
        curl -fsSL https://apt.goblinreactor.com/goblinreactor-archive-keyring.gpg -o "$temporary/keyring.gpg"
        fingerprint=$(gpg --batch --show-keys --with-colons "$temporary/keyring.gpg" | awk -F: '$1 == "fpr" {print $10; exit}')
        [ "$fingerprint" = B7B61E8CB85A9270530364F23273CB62402FC793 ] || die 'Unexpected repository signing key.'
        as_root install -d -m 0755 /etc/apt/keyrings
        as_root install -m 0644 "$temporary/keyring.gpg" "$key"
        as_root install -m 0644 "$temporary/goblinreactor.sources" "$source"
    fi
    as_root apt-get update
    as_root apt-get install -y silkie ca-certificates curl unzip fontconfig
    install_iosevka_linux "$temporary"
    silkie --version
    silkie-kitten --version
}

main "$@"
