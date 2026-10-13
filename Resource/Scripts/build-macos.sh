#!/bin/bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: Resource/Scripts/build-macos.sh [Debug|Release] [xcodebuild arguments...]

Defaults to Debug. PROJUCER may name a Projucer executable or .app bundle.
Examples:
  Resource/Scripts/build-macos.sh Release
  Resource/Scripts/build-macos.sh Debug -quiet
  PROJUCER=/path/to/Projucer.app Resource/Scripts/build-macos.sh
EOF
}

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

configuration=Debug
if [ "$#" -gt 0 ]; then
    case "$1" in
        Debug|Release) configuration="$1"; shift ;;
        -h|--help) usage; exit 0 ;;
        -*|*=*) ;; # Pass additional xcodebuild options through unchanged.
        *) usage >&2; fail "Configuration must be Debug or Release: $1" ;;
    esac
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
project_root="$(cd "$script_dir/../.." && pwd -P)"
project_file="$project_root/WaveformPlayer.jucer"
juce_root="$project_root/../../JUCE"

[ "$(uname -s)" = Darwin ] || fail 'This script requires macOS.'
[ -f "$project_file" ] || fail "Project file not found: $project_file"
[ -f "$juce_root/modules/juce_core/juce_core.h" ] \
    || fail "JUCE modules not found at $juce_root/modules. Update the MODULEPATHS in WaveformPlayer.jucer and juce_root in this script if JUCE has moved."

xcode-select -p >/dev/null 2>&1 \
    || fail 'Xcode is not selected. Install Xcode and select its developer directory with xcode-select.'
xcodebuild -version >/dev/null 2>&1 \
    || fail 'A full Xcode installation is required. Select Xcode.app/Contents/Developer with xcode-select.'
xcrun --sdk macosx --find clang >/dev/null 2>&1 \
    || fail 'The macOS SDK is unavailable. Complete the Xcode first-launch setup.'

projucer="${PROJUCER:-}"
if [ -z "$projucer" ]; then
    if command -v Projucer >/dev/null 2>&1; then
        projucer="$(command -v Projucer)"
    else
        for candidate in \
            "$juce_root/Projucer.app/Contents/MacOS/Projucer" \
            "$juce_root/extras/Projucer/Builds/MacOSX/build/Release/Projucer.app/Contents/MacOS/Projucer" \
            "$juce_root/extras/Projucer/Builds/MacOSX/build/Debug/Projucer.app/Contents/MacOS/Projucer" \
            '/Applications/Projucer.app/Contents/MacOS/Projucer'; do
            if [ -x "$candidate" ]; then
                projucer="$candidate"
                break
            fi
        done
    fi
fi

case "$projucer" in
    *.app|*.app/) projucer="${projucer%/}/Contents/MacOS/Projucer" ;;
esac
if [ -n "$projucer" ] && [ ! -x "$projucer" ]; then
    projucer="$(command -v "$projucer" 2>/dev/null || true)"
fi
[ -n "$projucer" ] && [ -x "$projucer" ] \
    || fail 'Projucer was not found. Set PROJUCER to the JUCE 7.0.11 Projucer executable or .app bundle.'

printf 'Regenerating Xcode project with %s\n' "$projucer"
"$projucer" --resave "$project_file"

printf 'Building WaveformPlayer (%s)\n' "$configuration"
# The exporter controls architecture, signing and product output directories.
# exec preserves the exact xcodebuild exit status and forwards all extra options.
exec xcodebuild \
    -project "$project_root/Builds/MacOSX/WaveformPlayer.xcodeproj" \
    -scheme 'WaveformPlayer - All' \
    -configuration "$configuration" \
    -derivedDataPath "$project_root/Builds/MacOSX/DerivedData" \
    "$@" \
    build
