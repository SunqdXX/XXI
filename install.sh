#!/bin/sh
set -eu

REPO="https://github.com/SunqdXX/XXI.git"

say()  { printf '%s\n' "$*"; }
die()  { printf 'xxi: %s\n' "$*" >&2; exit 1; }

CC="${CC:-}"
if [ -z "$CC" ]; then
    for c in cc gcc clang tcc; do
        command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }
    done
fi
[ -n "$CC" ] || die "no C compiler found. install gcc or clang, then run this again."

SRC=""
[ -f "./xxi.c" ] && SRC="$(pwd)"
if [ -z "$SRC" ]; then
    d="${0%/*}"
    [ "$d" != "$0" ] && [ -f "$d/xxi.c" ] && SRC="$(cd "$d" && pwd)"
fi

TMP=""
if [ -z "$SRC" ]; then
    command -v git >/dev/null 2>&1 || die "no xxi.c here, and git is not installed."
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT INT TERM
    say "getting XXI ..."
    git clone --depth 1 -q "$REPO" "$TMP/XXI" || die "could not download from $REPO"
    SRC="$TMP/XXI"
fi

say "building ..."
( cd "$SRC" && $CC -O2 -std=c11 -o xxi xxi.c ) || die "build failed"

if [ -n "${PREFIX:-}" ]; then
    DEST="$PREFIX/bin"
elif [ "$(id -u)" = "0" ]; then
    DEST="/usr/local/bin"
else
    DEST="$HOME/.local/bin"
fi

mkdir -p "$DEST" || die "cannot create $DEST"
cp "$SRC/xxi" "$DEST/xxi" || die "cannot write to $DEST"
chmod 755 "$DEST/xxi"
say "installed to $DEST/xxi"

case ":${PATH:-}:" in
    *":$DEST:"*) say ""; say "all done. try:   xxi hello.txt"; exit 0 ;;
esac

LINE="export PATH=\"$DEST:\$PATH\""
RC=""
case "$(basename "${SHELL:-sh}")" in
    bash) RC="$HOME/.bashrc" ;;
    zsh)  RC="$HOME/.zshrc" ;;
    ksh)  RC="$HOME/.kshrc" ;;
    fish) RC="$HOME/.config/fish/config.fish"; LINE="fish_add_path $DEST" ;;
esac

say ""
say "$DEST is not in your PATH yet."

if [ -n "$RC" ] && ( exec 3< /dev/tty ) 2>/dev/null; then
    printf 'add it to %s for you? [Y/n] ' "$RC"
    read -r answer < /dev/tty 2>/dev/null || answer="n"
    case "$answer" in
        ""|y|Y|yes|YES)
            mkdir -p "$(dirname "$RC")"
            printf '\n%s\n' "$LINE" >> "$RC"
            say ""
            say "added. open a new terminal, then try:   xxi hello.txt"
            exit 0
            ;;
    esac
fi

say ""
say "put this line in ${RC:-your shell startup file}:"
say ""
say "    $LINE"
say ""
say "then open a new terminal and try:   xxi hello.txt"
