#!/bin/sh
# Every automated check, the same list locally and in CI. It needs the build image (Qt, KF6, ECM, dbus-daemon, gettext): locally run it as `build/build-env.sh build/check.sh`.
# The project has CMake and QML: mandatory build/test and qmllint gates may not silently skip. Run inside the project build container
set -eu
cd "$(dirname "$0")/.."
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

step() { printf '\n== %s\n' "$*"; }

# `gates` is everything that needs no build, `build` the build, the tests and qmllint (CI runs the two side by side); with no argument both run, the gates first
phase=${1:-all}
case $phase in
    all | gates | build) ;;
    *) echo "usage: build/check.sh [gates|build]" >&2; exit 2 ;;
esac

if [ "$phase" != build ]; then
    step "Format (C++)"
    cpp_sources=$(find src tests \( -name '*.cpp' -o -name '*.h' \) 2>/dev/null || true)
    if [ -n "$cpp_sources" ]; then
        # clang-format --dry-run -Werror fuzzes nothing: word splitting is the point, one path per arg.
        # shellcheck disable=SC2086
        clang-format --dry-run -Werror $cpp_sources
    else
        echo "(no C++ sources yet: the .clang-format gate activates with the first one)"
    fi

    step "Static analysis (cppcheck)"
    if [ -n "$cpp_sources" ]; then
        # Same shape as Kwi's .kde-ci.yml: keep Qt's value getters (implicit sharing) and let moc-only
        # macros pass; word splitting deliberate.
        # shellcheck disable=SC2086
        cppcheck --language=c++ --library=qt --enable=warning,style,performance --suppress=returnByReference $cpp_sources
    else
        echo "(no C++ sources yet: the cppcheck gate activates with the first one)"
    fi

    # The gates that need no build run first: a failure there is known in seconds, not after the build
    step "Metainfo and desktop file"
    xmllint --noout data/*.metainfo.xml
    appstreamcli validate --no-net data/*.metainfo.xml
    desktop-file-validate data/io.github.the_beper.Kinefin.desktop

    step "Translation extraction and source/catalog consistency"
    po/extract.sh --check
    python3 scripts/po.py status --strict
    python3 -m unittest discover -s scripts -p 'test_po.py' -v

    step "Portable planning/CI and fixture contracts"
    python3 -m unittest discover -s tests -p 'test_*.py' -v

    step "Release simulation"
    if [ "${KINEFIN_RELEASE_SIMULATION:-true}" = false ]; then
        echo "(skipped: nothing the simulation exercises changed)"
    else
        build/release/simulate.sh
    fi

    step "REUSE license coverage"
    reuse lint

    step "Catalogs"
    while read -r l; do
        msgfmt -c --statistics -o /dev/null "po/$l.po"
    done < po/LINGUAS
fi

if [ "$phase" != gates ]; then
    step "Build and tests"
    [ -f CMakeLists.txt ] || { echo "missing required CMakeLists.txt" >&2; exit 1; }
    bdir=${KINEFIN_BUILD_DIR:-/buildtmp/check}
    set --
    if command -v ccache >/dev/null 2>&1; then
        export CCACHE_DIR="${CCACHE_DIR:-/buildtmp/ccache}"
        set -- -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    fi
    cmake -S . -B "$bdir" -G Ninja -DBUILD_TESTING=ON "$@"
    if command -v ccache >/dev/null 2>&1; then ccache -z >/dev/null; fi
    cmake --build "$bdir"
    if command -v ccache >/dev/null 2>&1; then ccache -s; fi
    jobs=$(($(nproc) * 2))
    [ "$jobs" -le 8 ] || jobs=8
    if ! ctest --test-dir "$bdir" --output-on-failure --timeout 900 -j "$jobs"; then
        [ -n "${CI:-}" ] || exit 1
        echo "::warning::Tests failed and are rerun once: a test that passes now is a flake to fix"
        ctest --test-dir "$bdir" --rerun-failed --no-tests=error --output-on-failure --timeout 900 -j "$jobs"
    fi

    step "qmllint"
    qml_sources=$(find src/qml -name '*.qml' 2>/dev/null || true)
    [ -n "$qml_sources" ] || { echo "no QML source files found" >&2; exit 1; }
    for qml in $qml_sources; do
        grep -q "$qml" CMakeLists.txt || { echo "$qml is not registered in CMakeLists.txt" >&2; exit 1; }
    done
    cmake --build "$bdir" --target kinefin_ui_qmllint
fi
