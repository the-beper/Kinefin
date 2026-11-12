#!/bin/sh
# Every automated check, the same list locally and in CI. It needs the build image (Qt, KF6, ECM, dbus-daemon, gettext): locally run it as `build/build-env.sh build/check.sh`.
# P0 has a CMake project and QML: mandatory build/test and qmllint gates may not silently skip. Run inside the project build container.
set -eu
cd "$(dirname "$0")/.."
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

step() { printf '\n== %s\n' "$*"; }

step "Format (C++; gate armed for P0)"
cpp_sources=$(find src tests \( -name '*.cpp' -o -name '*.h' \) 2>/dev/null || true)
if [ -n "$cpp_sources" ]; then
    # clang-format --dry-run -Werror fuzzes nothing: word splitting is the point, one path per arg.
    # shellcheck disable=SC2086
    clang-format --dry-run -Werror $cpp_sources
else
    echo "(no C++ sources yet: the .clang-format gate activates with the first one)"
fi

step "Static analysis (cppcheck; gate armed for P0)"
if [ -n "$cpp_sources" ]; then
    # Same shape as Kwi's .kde-ci.yml: keep Qt's value getters (implicit sharing) and let moc-only
    # macros pass; word splitting deliberate.
    # shellcheck disable=SC2086
    cppcheck --language=c++ --library=qt --enable=warning,style,performance --suppress=returnByReference $cpp_sources
else
    echo "(no C++ sources yet: the cppcheck gate activates with the first one)"
fi

step "Build and tests"
[ -f CMakeLists.txt ] || { echo "missing required CMakeLists.txt" >&2; exit 1; }
# The build tree lives outside the checkout (a named podman volume mount, CI's
# throw-away /tmp): the fetched mpvqt subproject is a third-party SPDX tree that
# reuse lint must not have to cover, and this checkout runs without git, so no
# ignore rule could ever hide it. Point KINEFIN_BUILD_DIR elsewhere to override.
bdir=${KINEFIN_BUILD_DIR:-/buildtmp/check}
# ccache is optional (an older image or a host without it builds as before); its cache sits next to the build tree, outside the checkout
set --
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-/buildtmp/ccache}"
    set -- -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
fi
cmake -S . -B "$bdir" -G Ninja -DBUILD_TESTING=ON "$@"
cmake --build "$bdir"
if command -v ccache >/dev/null 2>&1; then ccache -s; fi
jobs=$(nproc)
[ "$jobs" -le 8 ] || jobs=8
ctest --test-dir "$bdir" --output-on-failure -j "$jobs"

step "qmllint"
# Linted through the module's own target, so the module's C++ types resolve; a QML file nobody registers would escape it, so every file under src/qml must be listed in CMakeLists.txt.
qml_sources=$(find src/qml -name '*.qml' 2>/dev/null || true)
[ -n "$qml_sources" ] || { echo "no QML source files found" >&2; exit 1; }
for qml in $qml_sources; do
    grep -q "$qml" CMakeLists.txt || { echo "$qml is not registered in CMakeLists.txt" >&2; exit 1; }
done
cmake --build "$bdir" --target kinefin_ui_qmllint

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

step "REUSE license coverage"
reuse lint

step "Catalogs"
while read -r l; do
    msgfmt -c --statistics -o /dev/null "po/$l.po"
done < po/LINGUAS
