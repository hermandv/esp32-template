"""Start the devlink PC app from any folder:  pc\\.venv\\Scripts\\python pc\\devlink.py"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from devlink_pc.gui.main import main  # noqa: E402

sys.exit(main())
