#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
project_root="$(cd "$script_dir/../.." && pwd -P)"
configuration="${1:-Debug}"
case "$configuration" in
    Debug|Release) ;;
    *) printf 'Usage: Resource/Scripts/test-playback-macos.sh [Debug|Release]\n' >&2; exit 1 ;;
esac
if [ "$(uname -s)" != Darwin ]; then
    printf 'Error: playback tests require macOS.\n' >&2
    exit 1
fi
export WAVEFORM_PLAYER_TEST_RESULTS="$project_root/Builds/MacOSX/TestResults"
if command -v ffmpeg >/dev/null 2>&1; then
    export WAVEFORM_PLAYER_TEST_FFMPEG="$(command -v ffmpeg)"
elif [ -x /opt/homebrew/bin/ffmpeg ]; then
    export WAVEFORM_PLAYER_TEST_FFMPEG=/opt/homebrew/bin/ffmpeg
else
    printf 'Error: ffmpeg is required to create the synthetic MP3 test fixture.\n' >&2
    exit 1
fi

# Reuse the generated Shared Code compiler flags and archive, avoiding a second
# JUCE build and keeping test macro definitions identical to the plugin.
# shlex preserves quoted Xcode include paths and literal plugin-name macros.
python3 - "$project_root" "$configuration" <<'PY'
from pathlib import Path
import re
import shlex
import subprocess
import sys

root = Path(sys.argv[1])
configuration = sys.argv[2]
archive = root / 'Builds/MacOSX/build' / configuration / 'libWaveformPlayer.a'
intermediates = root / 'Builds/MacOSX/DerivedData/Build/Intermediates.noindex/WaveformPlayer.build' / configuration
candidates = list(intermediates.glob('WaveformPlayer - Shared Code.build/Objects-normal/*/*-common-args.resp'))
responses = [path for path in candidates if '-std=c++' in path.read_text()]
if not archive.exists() or not responses:
    sys.exit(f'Error: run "{root / "Resource/Scripts/build-macos.sh"}" {configuration} before playback tests.')
response = max(responses, key=lambda path: path.stat().st_mtime)
flags = [flag for flag in shlex.split(response.read_text()) if flag != '-fobjc-weak']
clang = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--find', 'clang++'], text=True).strip()
output_dir = root / 'Builds/MacOSX/TestResults'
output_dir.mkdir(parents=True, exist_ok=True)
executable = output_dir / ('PlaybackTests-' + configuration)
project = root / 'Builds/MacOSX/WaveformPlayer.xcodeproj/project.pbxproj'
frameworks = sorted(set(re.findall(r'path = System/Library/Frameworks/([A-Za-z0-9_]+)\.framework;', project.read_text())))
command = [clang, *flags, str(root / 'Tests/PlaybackTests.cpp'), str(archive), '-o', str(executable)]
for framework in frameworks:
    command.extend(['-framework', framework])
print(f'Compiling playback tests against {configuration} shared code...', flush=True)
subprocess.run(command, check=True, cwd=root)
print('Running processor tests without an audio device or DAW...', flush=True)
try:
    result = subprocess.run([str(executable)], cwd=root, timeout=70)
except subprocess.TimeoutExpired:
    sys.exit('Error: playback test application exceeded the 70 second timeout.')
sys.exit(result.returncode)
PY
