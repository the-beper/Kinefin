#!/bin/sh
set -eu

out=${GITHUB_OUTPUT:-/dev/stdout}
code=false
packaging=false

# Prose and templates never need a build.
is_doc() {
    case $1 in
        *.md | LICENSE | .github/ISSUE_TEMPLATE/* | .gitignore) return 0 ;;
    esac
    return 1
}

# Packaging inputs: every source that can change the build artifact, including C++ and headers, dependencies, shell scripts, resources, translation catalogs and CI/configuration.
is_packaging() {
    case $1 in
        build/* | .github/* | CMakeLists.txt | */CMakeLists.txt | src/* | data/* | po/* | REUSE.toml | LICENSES/*) return 0 ;;
    esac
    return 1
}

if [ "${EVENT_NAME:-}" != pull_request ]; then
    code=true
    packaging=true
elif [ -z "${BASE_SHA:-}" ] || ! git cat-file -e "${BASE_SHA}^{commit}" 2>/dev/null; then
    echo "changes.sh: base commit '${BASE_SHA:-}' not available, running everything" >&2
    code=true
    packaging=true
else
    files=$(git -c core.quotePath=false diff --name-only "$BASE_SHA" HEAD)
    echo "Changed files:"
    printf '%s\n' "$files" | sed 's/^/  /'
    # The loop reads a here-document, not a pipe, so the variables it sets are kept.
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        is_doc "$f" && continue
        code=true
        if is_packaging "$f"; then packaging=true; fi
    done <<EOF
$files
EOF
fi

echo "code=$code" >>"$out"
echo "packaging=$packaging" >>"$out"
echo "code=$code packaging=$packaging"
