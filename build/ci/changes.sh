#!/bin/sh
set -eu

out=${GITHUB_OUTPUT:-/dev/stdout}
code=false
packaging=false
images=false

# Prose and templates never need a build, except the settings reference, which test_settings_parity reads
is_doc() {
    case $1 in
        docs/settings.md) return 1 ;;
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

# The paths that make publish-images.yml publish the build images on a push to main: the jobs that run in those images must wait for that run
is_image_input() {
    case $1 in
        build/Containerfile | build/flatpak/Containerfile | .github/workflows/publish-images.yml) return 0 ;;
    esac
    return 1
}

if [ "${EVENT_NAME:-}" != pull_request ]; then
    code=true
    packaging=true
    # Only a push to main starts publish-images.yml, and only when it changed one of those paths: judge by the pushed range, as GitHub's path filter does. After a force-push (or a new branch) the previous head is not in the checkout, so the range is unknown: images=maybe makes the job wait only if an Images run shows up
    if [ "${EVENT_NAME:-}" = push ]; then
        if [ -z "${BEFORE_SHA:-}" ] || ! git cat-file -e "${BEFORE_SHA}^{commit}" 2>/dev/null; then
            echo "changes.sh: previous commit '${BEFORE_SHA:-}' not available, cannot tell whether the push changed an image input (images=maybe)" >&2
            images=maybe
        else
            pushed=$(git -c core.quotePath=false diff --name-only "$BEFORE_SHA" HEAD)
            while IFS= read -r f; do
                if is_image_input "$f"; then images=true; fi
            done <<EOF
$pushed
EOF
        fi
    fi
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
echo "images=$images" >>"$out"
echo "code=$code packaging=$packaging images=$images"
