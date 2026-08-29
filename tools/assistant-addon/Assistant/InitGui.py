"""Assistant addon - GUI initializer (thin, exec-safe shim).

IMPORTANT: FreeCAD exec()s this file without defining ``__file__``, so it must
not reference ``__file__`` and must not rely on module-scope cross-references.
All real logic lives in ``init_impl.py`` (normally imported); this file only
registers the workbench and the two toggle commands.  FreeCAD puts the addon dir
on ``sys.path`` so ``import init_impl`` resolves.
"""

import FreeCADGui  # noqa: E402
import init_impl  # noqa: E402

FreeCADGui.addWorkbench(init_impl.AssistantWorkbench)
FreeCADGui.addCommand("Assistant_Toggle", init_impl.AssistantToggleCmd())
FreeCADGui.addCommand("Assistant_Clear", init_impl.AssistantClearCmd())
