// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

#pragma once

/** The single home of the "promote freshly recorded render commands to the
 *  screen-space OVERLAY pass" convention.
 *
 *  Five nodes used to carry their own copy of this loop, which is how the
 *  scissor requirement and the annotation flag could drift apart.  A node that
 *  only needs the full-viewport scissor belongs here; a node that also pins a
 *  per-rect viewport (SoNaviCube) genuinely differs and keeps its own loop.
 */

#include <algorithm>

#include <Inventor/SbViewportRegion.h>
#include <Inventor/elements/SoViewportRegionElement.h>
#include <Inventor/misc/SoState.h>
#include <Inventor/rendering/SoRenderIR.h>

namespace Gui
{

/** Promote every render command recorded since \a firstCommand to the OVERLAY
 *  pass, scissored to \a viewport.
 *
 *  A command in the OVERLAY pass is drawn as a layer above the main scene
 *  instead of inside it, so the cached base geometry stays stable and a
 *  selection, highlight or overlay is composited on top.  The raster backend
 *  skips an unscissored overlay, so the scissor must be set together with the
 *  pass switch; this function is the one place that pairing lives.
 *
 *  \a annotation additionally marks the commands as annotation overlay commands
 *  (SoRenderCommand::isAnnotation).  The raster Vulkan backend draws that group
 *  last and clears the depth buffer once before it, which is the behaviour the
 *  GL path gets from the delayed-annotation replay
 *  (View3DInventorViewer::renderDelayedAnnotations).  Depth testing stays on,
 *  so an annotation subtree still occludes itself.  So3DAnnotation::IRRender is
 *  the only producer of annotation commands.
 *
 *  A no-op when nothing was recorded since \a firstCommand.
 */
inline void promoteCommandsToOverlay(SoDrawList& list,
                                     int firstCommand,
                                     const SbViewportRegion& viewport,
                                     bool annotation = false)
{
    const int count = list.getNumCommands();
    if (count <= firstCommand) {
        return;
    }

    const short vx = std::max(0, static_cast<int>(viewport.getViewportOriginPixels()[0]));
    const short vy = std::max(0, static_cast<int>(viewport.getViewportOriginPixels()[1]));
    const short vw = std::max(1, static_cast<int>(viewport.getViewportSizePixels()[0]));
    const short vh = std::max(1, static_cast<int>(viewport.getViewportSizePixels()[1]));

    for (int i = firstCommand; i < count; ++i) {
        SoRenderCommand& cmd = list.getCommand(i);
        cmd.pass = SO_RENDERPASS_OVERLAY;
        if (annotation) {
            cmd.isAnnotation = TRUE;
        }
        cmd.state.raster.scissorEnabled = TRUE;
        cmd.state.raster.scissorX = vx;
        cmd.state.raster.scissorY = vy;
        cmd.state.raster.scissorWidth = vw;
        cmd.state.raster.scissorHeight = vh;
    }
}

/** Convenience overload: scissor to the state's current viewport region. */
inline void promoteCommandsToOverlay(SoDrawList& list,
                                     int firstCommand,
                                     SoState* state,
                                     bool annotation = false)
{
    promoteCommandsToOverlay(list, firstCommand, SoViewportRegionElement::get(state), annotation);
}

}  // namespace Gui
