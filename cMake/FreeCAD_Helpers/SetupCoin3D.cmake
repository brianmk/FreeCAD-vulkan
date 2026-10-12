# ---------------------------------------------------------------------------
# Configure-time Coin IR checks
#
# FreeCAD's Vulkan viewport records IR commands and flags the annotation ones
# with SoRenderCommand::isAnnotation (src/Gui/Inventor/So3DAnnotation.cpp); the
# backend's annotation/overlay sweep consumes that flag.  The member was added
# by brianmk/coin 738926470 ("Vulkan: annotate overlay commands, draw them with
# a depth clear; drop dead code").  Revision 7912d9798, which an earlier fork
# revision pinned, has the IR API but not the member, so FreeCAD configured
# fine and only failed inside the compiler with
#   error: 'struct SoRenderCommand' has no member named 'isAnnotation'
#
# HAVE_COIN_IR_RENDER_ACTION only proves that the SoIRRenderAction header
# exists, so it cannot catch that.  FreeCADCheckCoinIRAnnotation() probes the
# member itself and fails at configure time with the symbol name.
#
# Required bundled Coin revision (or a descendant).  Cache-internal so the
# macros below, which run in the including scope, can always read it: this file
# is included from the cMake/FreeCAD_Helpers subdirectory, so a plain set()
# here would be forgotten before SetupCoinPivy() runs.
set(FREECAD_COIN_IR_ANNOTATION_REVISION "73892647017188122384b0abdc343f2cee7ae58c"
    CACHE INTERNAL "Bundled Coin revision that introduced SoRenderCommand::isAnnotation")

macro(FreeCADCheckCoinIRAnnotation _include_dirs _context)
    include(CheckCXXSourceCompiles)
    # Do not reuse a cached verdict: the Coin checkout can change between two
    # configures of the same build directory, and a cached result would then
    # describe a revision that is no longer checked out.
    unset(HAVE_COIN_IR_ANNOTATION CACHE)
    set(_freecad_coin_annotation_saved_includes "${CMAKE_REQUIRED_INCLUDES}")
    set(CMAKE_REQUIRED_INCLUDES ${_include_dirs})
    check_cxx_source_compiles("
        #include <Inventor/rendering/SoRenderIR.h>
        // So3DAnnotation.cpp writes this member; without it the build fails
        // with \"SoRenderCommand has no member named 'isAnnotation'\".
        void freecadCoinIRAnnotationProbe(SoRenderCommand& command) { command.isAnnotation = true; }
        int main() { return 0; }
    " HAVE_COIN_IR_ANNOTATION)
    set(CMAKE_REQUIRED_INCLUDES "${_freecad_coin_annotation_saved_includes}")
    if (HAVE_COIN_IR_ANNOTATION)
        add_definitions(-DHAVE_COIN_IR_ANNOTATION)
        message(STATUS "${_context}: SoRenderCommand::isAnnotation is available")
    else ()
        message(FATAL_ERROR
            "${_context} is missing SoRenderCommand::isAnnotation.\n"
            "FreeCAD writes that member in src/Gui/Inventor/So3DAnnotation.cpp (under "
            "HAVE_COIN_IR_RENDER_ACTION), so this configuration cannot compile: the compiler "
            "would report \"SoRenderCommand has no member named 'isAnnotation'\".\n"
            "The bundled Coin must be ${FREECAD_COIN_IR_ANNOTATION_REVISION} (\"Vulkan: annotate "
            "overlay commands, draw them with a depth clear; drop dead code\") or a descendant.\n"
            "Probed with include directories: ${_include_dirs}")
    endif ()
endmacro()

macro(SetupCoin3D)
    # -------------------------------- Coin3D --------------------------------

    if (WIN32 AND MINGW)
        find_path(COIN3D_INCLUDE_DIRS Inventor/So.h)
        find_library(COIN3D_LIBRARIES Coin)
    endif ()

    # Try CONFIG mode -- Coin supports very old CMake files, which emits noisy warnings. Silence them.
    set(CMAKE_WARN_DEPRECATED_OLD_STATE ${CMAKE_WARN_DEPRECATED})
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
    find_package(Coin CONFIG)
    set(CMAKE_WARN_DEPRECATED ${CMAKE_WARN_DEPRECATED_OLD_STATE} CACHE BOOL "" FORCE)
    if (Coin_FOUND)
        set(COIN3D_INCLUDE_DIRS ${Coin_INCLUDE_DIR})
        set(COIN3D_LIBRARIES ${Coin_LIBRARIES})
        set(COIN3D_LIB_DIRS ${Coin_LIB_DIR})
    else ()
        # Try MODULE mode (FindCoin3D.cmake, included by CMake)
        find_package(Coin3D)
        if (NOT COIN3D_FOUND)
            message(FATAL_ERROR "Could not find Coin3D")
        endif ()
    endif ()

    IF (NOT COIN3D_VERSION)
        file(READ "${COIN3D_INCLUDE_DIRS}/Inventor/C/basic.h" _coin3d_basic_h)
        string(REGEX MATCH "define[ \t]+COIN_MAJOR_VERSION[ \t]+([0-9?])" _coin3d_major_version_match "${_coin3d_basic_h}")
        set(COIN3D_MAJOR_VERSION "${CMAKE_MATCH_1}")
        string(REGEX MATCH "define[ \t]+COIN_MINOR_VERSION[ \t]+([0-9?])" _coin3d_minor_version_match "${_coin3d_basic_h}")
        set(COIN3D_MINOR_VERSION "${CMAKE_MATCH_1}")
        string(REGEX MATCH "define[ \t]+COIN_MICRO_VERSION[ \t]+([0-9?]+)" _coin3d_micro_version_match "${_coin3d_basic_h}")
        set(COIN3D_MICRO_VERSION "${CMAKE_MATCH_1}")
        set(COIN3D_VERSION "${COIN3D_MAJOR_VERSION}.${COIN3D_MINOR_VERSION}.${COIN3D_MICRO_VERSION}")
    ENDIF ()

    # The retained (immediate-render) API used by the Vulkan viewport only
    # exists in the bundled Coin.  Guard every SoIRRenderAction consumer with
    # HAVE_COIN_IR_RENDER_ACTION so external/system Coin builds (which lack
    # SoNode::IRRender) still compile; the IR overrides simply compile out.
    if (EXISTS "${COIN3D_INCLUDE_DIRS}/Inventor/actions/SoIRRenderAction.h")
        add_definitions(-DHAVE_COIN_IR_RENDER_ACTION)
        message(STATUS "Coin3D provides the IR render API (SoIRRenderAction)")
        # Every HAVE_COIN_IR_RENDER_ACTION consumer also writes the annotation
        # flag, so probe for it here as well.  External Coin normally has
        # neither, in which case this branch is not entered at all.
        FreeCADCheckCoinIRAnnotation("${COIN3D_INCLUDE_DIRS}" "External Coin3D")
    endif ()
endmacro(SetupCoin3D)

macro(SetupPivy)
    # -------------------------------- Pivy --------------------------------

    IF (FREECAD_CHECK_PIVY) # do not make pivy a host dependency for cross compiling
        IF (NOT PIVY_VERSION)
            message(STATUS "Checking Pivy version by importing it in a Python program...")
            execute_process(
                    COMMAND ${Python3_EXECUTABLE} -c "import pivy as p; print(p.__version__,end='')"
                    OUTPUT_VARIABLE PIVY_VERSION
                    RESULT_VARIABLE RETURN_CODE)
            if (RETURN_CODE EQUAL 0)
                message(STATUS "Found Pivy ${PIVY_VERSION}")
            else ()
                message(ERROR "Failed to import Pivy using ${Python3_EXECUTABLE}")
            endif ()
        ENDIF ()

        message(STATUS "Checking Pivy Coin3D version by importing it in a Python program...")
        execute_process(
                COMMAND ${Python3_EXECUTABLE} -c "import pivy as p; print(p.SoDB.getVersion(),end='')"
                OUTPUT_VARIABLE PIVY_COIN_VERSION
                RESULT_VARIABLE RETURN_CODE)
        if (RETURN_CODE EQUAL 0)
            message(STATUS "Found Pivy Coin3D ${PIVY_COIN_VERSION}")
        else ()
            message(ERROR "Failed to get Pivy Coin3D version using ${Python3_EXECUTABLE}")
        endif ()

        if (${PIVY_COIN_VERSION} MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
            set(PIVY_COIN_MAJOR_VERSION ${CMAKE_MATCH_1})
            set(PIVY_COIN_MINOR_VERSION ${CMAKE_MATCH_2})
            set(PIVY_COIN_MICRO_VERSION ${CMAKE_MATCH_3})
            set(PIVY_COIN_VERSION "${PIVY_COIN_MAJOR_VERSION}.${PIVY_COIN_MINOR_VERSION}.${PIVY_COIN_MICRO_VERSION}")
        else ()
            message(FATAL_ERROR "Failed to match Pivy Coin3D version string output")
        endif ()

        if (NOT (
            (${COIN3D_MAJOR_VERSION} EQUAL ${PIVY_COIN_MAJOR_VERSION}) AND
            (${COIN3D_MINOR_VERSION} EQUAL ${PIVY_COIN_MINOR_VERSION}) AND
            (${COIN3D_MICRO_VERSION} EQUAL ${PIVY_COIN_MICRO_VERSION})))
            message(FATAL_ERROR "Coin3D version ${COIN3D_VERSION} mismatches Pivy Coin3D ${PIVY_COIN_VERSION}.")
        endif ()
    endif(FREECAD_CHECK_PIVY)
endmacro(SetupPivy)

macro(SetupBundledCoinPivy)
    if (NOT EXISTS "${CMAKE_SOURCE_DIR}/src/3rdParty/coin/CMakeLists.txt"
        OR NOT EXISTS "${CMAKE_SOURCE_DIR}/src/3rdParty/pivy/CMakeLists.txt")
        message(FATAL_ERROR
            "Bundled Coin/Pivy git submodules are not available. "
            "Please run:\n"
            "  git submodule update --init --recursive"
        )
    endif ()

    find_package(EXPAT QUIET)
    if (EXPAT_FOUND)
        if (DEFINED EXPAT_VERSION_STRING AND NOT EXPAT_VERSION_STRING STREQUAL "")
            set(FREECAD_COIN_EXPAT_SOURCE "external (${EXPAT_VERSION_STRING})")
            set(_freecad_coin_expat_message "Bundled Coin will use system Expat ${EXPAT_VERSION_STRING}")
        else ()
            set(FREECAD_COIN_EXPAT_SOURCE "external")
            set(_freecad_coin_expat_message "Bundled Coin will use system Expat")
        endif ()
        message(STATUS "${_freecad_coin_expat_message}")
    else ()
        set(FREECAD_COIN_EXPAT_SOURCE "bundled fallback")
        message(WARNING
            "Bundled Coin could not find a system Expat. "
            "Coin will fall back to its vendored Expat sources. "
            "This can fail on some platforms; install Expat development files "
            "or make Expat discoverable to CMake."
        )
    endif ()

    set(USE_EXTERNAL_EXPAT ON CACHE BOOL "Use system Expat for bundled Coin" FORCE)
    set(FREETYPE_RUNTIME_LINKING OFF CACHE BOOL "Disable FreeType runtime linking for bundled Coin" FORCE)
    set(COIN_BUILD_TESTS OFF CACHE BOOL "Build bundled Coin tests" FORCE)
    set(COIN_INSTALL OFF CACHE BOOL "Disable standalone install rules for bundled Coin" FORCE)
    # Enable the Vulkan render backend when FreeCAD requests the experimental
    # Vulkan viewport.  The OpenGL backend remains built and the default.
    set(COIN_BUILD_VULKAN_RENDERER ${FREECAD_USE_VULKAN} CACHE BOOL
        "Build the bundled Coin Vulkan render backend" FORCE)
    # A Vulkan build requires the Vulkan Memory Allocator (VMA); Coin no longer
    # bundles it.  Resolve it here so a missing header fails configure with
    # actionable guidance instead of deep inside the Coin subdirectory.
    if (FREECAD_USE_VULKAN)
        find_package(VulkanMemoryAllocator CONFIG QUIET)
        if (NOT TARGET VulkanMemoryAllocator::VulkanMemoryAllocator AND NOT COIN_VMA_INCLUDE_DIR)
            find_path(COIN_VMA_INCLUDE_DIR
                NAMES vk_mem_alloc.h
                PATH_SUFFIXES vma VulkanMemoryAllocator
                DOC "Directory containing vk_mem_alloc.h (Vulkan Memory Allocator)")
        endif ()
        if (TARGET VulkanMemoryAllocator::VulkanMemoryAllocator)
            set(FREECAD_COIN_VMA_SOURCE "system (VulkanMemoryAllocator CMake package)")
        elseif (COIN_VMA_INCLUDE_DIR)
            set(FREECAD_COIN_VMA_SOURCE "system (${COIN_VMA_INCLUDE_DIR})")
        else ()
            message(FATAL_ERROR
                "FREECAD_USE_VULKAN requires the Vulkan Memory Allocator (VMA).\n"
                "Coin no longer bundles VMA. Install the vk_mem_alloc.h header, e.g.:\n"
                "  - pixi/conda:    vulkan-memory-allocator\n"
                "  - Debian/Ubuntu: libvulkan-memory-allocator-dev\n"
                "  - vcpkg:         vulkan-memory-allocator\n"
                "  - manual:        configure with -DCOIN_VMA_INCLUDE_DIR=<dir containing vk_mem_alloc.h>")
        endif ()
        message(STATUS "Bundled Coin will use ${FREECAD_COIN_VMA_SOURCE} for the Vulkan Memory Allocator")
    endif ()
    # The bundled Coin is pinned by this repository's gitlink.  A submodule
    # checkout that lags behind it (a bump that was not followed by
    # `git submodule update`, or a build directory reused across branches)
    # builds a Coin other than the one the tree pins, so report both revisions
    # and stop when they disagree.  Skipped when there is no git checkout.
    find_package(Git QUIET)
    if (GIT_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse "HEAD:src/3rdParty/coin"
            OUTPUT_VARIABLE _freecad_coin_pinned_revision
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE _freecad_coin_pinned_rc)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}/src/3rdParty/coin" rev-parse HEAD
            OUTPUT_VARIABLE _freecad_coin_checked_out_revision
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE _freecad_coin_checked_out_rc)
        if (_freecad_coin_pinned_rc EQUAL 0 AND _freecad_coin_checked_out_rc EQUAL 0)
            message(STATUS
                "Bundled Coin revision: pinned ${_freecad_coin_pinned_revision}, "
                "checked out ${_freecad_coin_checked_out_revision}")
            if (NOT _freecad_coin_pinned_revision STREQUAL _freecad_coin_checked_out_revision)
                message(FATAL_ERROR
                    "The bundled Coin submodule is not at the revision this tree pins.\n"
                    "  pinned in the src/3rdParty/coin gitlink : ${_freecad_coin_pinned_revision}\n"
                    "  checked out in src/3rdParty/coin      : ${_freecad_coin_checked_out_revision}\n"
                    "Run: git submodule update --init src/3rdParty/coin")
            endif ()
        else ()
            message(STATUS "Bundled Coin revision: no git checkout, skipping the pinned-revision check")
        endif ()
    endif ()

    add_subdirectory("${CMAKE_SOURCE_DIR}/src/3rdParty/coin" "${CMAKE_BINARY_DIR}/src/3rdParty/coin")
    if (NOT DEFINED COIN_VERSION OR COIN_VERSION STREQUAL "")
        message(FATAL_ERROR "Bundled Coin did not define COIN_VERSION")
    endif ()
    # The bundled Coin always provides the retained IR render API used by
    # the Vulkan viewport.  The matching probe for external Coin lives in
    # SetupCoin3D() above; both gate FreeCAD's SoIRRenderAction consumers on
    # HAVE_COIN_IR_RENDER_ACTION.  The IR API alone is not enough: FreeCAD also
    # writes SoRenderCommand::isAnnotation, so probe for that member here and
    # fail the configure (not the compiler) when the revision lacks it.  The
    # bundled include directory is passed as well because Coin generates
    # headers into the build tree.
    add_definitions(-DHAVE_COIN_IR_RENDER_ACTION)
    FreeCADCheckCoinIRAnnotation(
        "${CMAKE_SOURCE_DIR}/src/3rdParty/coin/include;${CMAKE_BINARY_DIR}/src/3rdParty/coin/include"
        "Bundled Coin")
    set(COIN3D_VERSION "${COIN_VERSION}")
    # Match external Coin usage: FreeCAD targets should treat Coin headers as third-party headers.
    target_include_directories(Coin
        SYSTEM INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/3rdParty/coin/include>"
        "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/src/3rdParty/coin/include>"
    )
    if (UNIX AND NOT APPLE)
        # Coin directly loads sibling libraries such as libEGL.  The RUNPATH of
        # a library that loads Coin is not used to resolve Coin's dependencies.
        # Keep the bundled library relocatable in both Pixi and AppImage installs.
        set_property(TARGET Coin PROPERTY INSTALL_RPATH "$ORIGIN")
    endif ()
    install(TARGETS Coin
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} NAMELINK_SKIP
    )

    set(PIVY_USE_SOQT OFF CACHE BOOL "Build bundled Pivy SoQt bindings" FORCE)
    set(PIVY_PACKAGE_INSTALL_DIR "Mod/pivy"
        CACHE STRING "Install directory for bundled Pivy inside FreeCAD packages" FORCE)
    cmake_path(APPEND CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_LIBDIR}" OUTPUT_VARIABLE LIBPATH)
    cmake_path(APPEND CMAKE_INSTALL_PREFIX "${PIVY_PACKAGE_INSTALL_DIR}" OUTPUT_VARIABLE PIVYPATH)
    cmake_path(RELATIVE_PATH LIBPATH BASE_DIRECTORY "${PIVYPATH}" OUTPUT_VARIABLE RPATH)
    set(PIVY_MODULE_INSTALL_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>/${RPATH}"
        CACHE STRING "RUNPATH for bundled Pivy extension modules in FreeCAD packages" FORCE)
    set(PIVY_PACKAGE_OUTPUT_DIR "${PROJECT_BINARY_DIR}/Mod/pivy"
        CACHE PATH "Build-tree output directory for bundled Pivy" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/3rdParty/pivy" "${CMAKE_BINARY_DIR}/src/3rdParty/pivy")
    set_property(TARGET pivy_coin PROPERTY INSTALL_REMOVE_ENVIRONMENT_RPATH TRUE)
    if (NOT DEFINED PIVY_VERSION OR PIVY_VERSION STREQUAL "")
        message(FATAL_ERROR "Bundled Pivy did not define PIVY_VERSION")
    endif ()
endmacro(SetupBundledCoinPivy)

macro(SetupCoinPivy)
    if (FREECAD_USE_EXTERNAL_COIN_PIVY)
        set(FREECAD_COIN3D_SOURCE "external")
        set(FREECAD_PIVY_SOURCE "external")
        SetupCoin3D()
        SetupPivy()
    else ()
        set(FREECAD_COIN3D_SOURCE "bundled")
        set(FREECAD_PIVY_SOURCE "bundled")
        SetupBundledCoinPivy()
    endif ()
endmacro(SetupCoinPivy)
