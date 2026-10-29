r"""Start the devlink web UI from any folder:  pc\.venv\Scripts\python pc\devlink_web.py  [--host ... --port ...]
A local server on http://127.0.0.1:8470/; the page opens in the default browser."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from devlink_pc.web.server import main  # noqa: E402

sys.exit(main())
