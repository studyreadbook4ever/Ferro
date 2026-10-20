#!/usr/bin/env bash
set -euo pipefail
ferro_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cmake -S "$ferro_root" -B "$ferro_root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ferro_root/build" -j 4
python3 - "$ferro_root" <<'PY'
from pathlib import Path
import os
import shlex
import sys
root = Path(sys.argv[1])
applications = Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'applications'
applications.mkdir(parents=True, exist_ok=True)
executable = str(root / 'run.sh').replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$').replace('%', '%%')
(applications / 'ferro.desktop').write_text(
    '[Desktop Entry]\nType=Application\nName=FERRO\nComment=Music composition and song arrangement\n'
    'Comment[ko]=음악 작곡과 곡 편집\n'
    f'Exec="{executable}"\nIcon={root / "assets/ferro.svg"}\n'
    'Terminal=false\nCategories=AudioVideo;Audio;Sequencer;\nStartupNotify=true\n'
)
print(f'Installed launcher: {applications / "ferro.desktop"}')
bin_dir = Path.home() / '.local/bin'
bin_dir.mkdir(parents=True, exist_ok=True)
agent = bin_dir / 'ferro-agent'
agent.write_text('#!/usr/bin/env bash\nexec ' + shlex.quote(str(root / 'ferro-agent')) + ' "$@"\n')
agent.chmod(0o755)
print(f'Installed agent CLI: {agent}')
PY
