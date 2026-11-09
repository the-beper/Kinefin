#!/bin/sh
# Regenerates po/kinefin.pot from the files in po/POTFILES.in and merges it into the translations listed in po/LINGUAS (one po/<lang>.po each):
#   build/build-env.sh po/extract.sh
#
# Two extraction passes, because xgettext takes one language per run:
#   C++ (.cpp, .h)     language=C++, keywords i18n/i18nc/i18np/i18ncp
# QML (.qml)         language=JavaScript (localized i18n calls) — the C++ lexer alone would misread QML syntax; both passes join into the same template.
set -eu
cd "$(dirname "$0")/.."

mode=${1:-update}
case "$mode" in
    --check | update) ;;
    *) echo "usage: po/extract.sh [--check]" >&2; exit 2 ;;
esac
# Check mode never edits source catalogs: CI can compare a freshly extracted template against committed translations without timestamp-only diffs
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' 0
trap 'exit 1' 1 2 3 15
pot="$tmpdir/kinefin.pot"

# A source with an i18n call that is not listed would lose its strings silently
for file in $(grep -rlE '\b(kl)?i18n[a-z]*\(' src --include='*.cpp' --include='*.h' --include='*.qml' 2>/dev/null || true)
do
    grep -qx "$file" po/POTFILES.in || { echo "$file has translatable strings but is not in po/POTFILES.in" >&2; exit 1; }
done

cpp_files=$(grep -E '\.(cpp|h)$' po/POTFILES.in || true)
qml_files=$(grep -E '\.qml$' po/POTFILES.in || true)

while IFS= read -r file; do
    [ -z "$file" ] && continue
    [ -f "$file" ] || { echo "po/POTFILES.in names a missing file: $file" >&2; exit 1; }
done <<EOF
$cpp_files
$qml_files
EOF

if [ -n "$cpp_files" ]; then
    printf '%s\n' "$cpp_files" > "$tmpdir/potfiles"
    xgettext --from-code=UTF-8 --language=C++ --add-comments=TRANSLATORS \
        --keyword=i18n:1 --keyword=kli18n:1 --keyword=i18nc:1c,2 --keyword=i18np:1,2 --keyword=i18ncp:1c,2,3 \
        --files-from="$tmpdir/potfiles" \
        --package-name=kinefin --copyright-holder="the-beper" \
        --msgid-bugs-address= \
        -o "$pot"
fi
if [ -n "$qml_files" ]; then
    printf '%s\n' "$qml_files" > "$tmpdir/potfiles"
    qml_flags=--language=JavaScript
    [ -f "$pot" ] && qml_flags="$qml_flags -j"
    # shellcheck disable=SC2086
    xgettext --from-code=UTF-8 $qml_flags --add-comments=TRANSLATORS \
        --keyword=i18n:1 --keyword=kli18n:1 --keyword=i18nc:1c,2 --keyword=i18np:1,2 --keyword=i18ncp:1c,2,3 \
        --files-from="$tmpdir/potfiles" \
        --package-name=kinefin --copyright-holder="the-beper" \
        --msgid-bugs-address= \
        -o "$pot"
fi
[ -f "$pot" ] || { echo "no extracted messages from po/POTFILES.in; refusing a vacuous green gate" >&2; exit 1; }

# xgettext leaves placeholders in the first lines of the template: fill in the ones that are ours
sed -i \
    -e 's/^# SOME DESCRIPTIVE TITLE\./# Translation template for Kinefin./' \
    -e 's/^# Copyright (C) YEAR /# Copyright (C) 2026 /' \
    -e 's/^# FIRST AUTHOR <EMAIL@ADDRESS>, YEAR\./# the-beper <193650145+the-beper@users.noreply.github.com>, 2026./' \
    "$pot"
if [ "$mode" = --check ]; then
    python3 scripts/po.py verify-template "$pot"
    exit $?
fi
cp "$pot" po/kinefin.pot
for lang in $(cat po/LINGUAS); do
    msgmerge --update --backup=none --quiet "po/$lang.po" po/kinefin.pot
    msgattrib --no-obsolete --output-file="po/$lang.po" "po/$lang.po"
done
