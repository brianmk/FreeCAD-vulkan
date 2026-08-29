"""Assistant addon - App-side initializer.

IMPORTANT: FreeCAD exec()s this file in the App process *without* setting
``__file__`` in its query scope, so this module must never reference ``__file__``
at import time.  FreeCAD has already added the addon directory to ``sys.path``
before this runs, so no path setup is needed here.
"""


def startup():
    """Hook FreeCAD calls at App startup.  Nothing to do - all wiring is GUI-side."""
    return None
