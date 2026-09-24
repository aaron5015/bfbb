add_library(librw
    "${PROJECT_SOURCE_DIR}/args.h"
    "${PROJECT_SOURCE_DIR}/rw.h"

    anim.cpp
    base.cpp
    bmp.cpp
    camera.cpp
    charset.cpp
    clump.cpp
    engine.cpp
    error.cpp
    frame.cpp
    geometry.cpp
    geoplg.cpp
    hanim.cpp
    image.cpp
    light.cpp
    matfx.cpp
    pipeline.cpp
    plg.cpp
    png.cpp
    prim.cpp
    raster.cpp
    render.cpp
    rwanim.h
    rwengine.h
    rwerror.h
    rwobjects.h
    rwpipeline.h
    rwplg.h
    rwplugins.h
    rwrender.h
    rwuserdata.h
    skin.cpp
    texture.cpp
    gcntex.cpp
    tga.cpp
    tristrip.cpp
    userdata.cpp
    uvanim.cpp
    world.cpp

    d3d/d3d8.cpp
    d3d/d3d8matfx.cpp
    d3d/d3d8render.cpp
    d3d/d3d8skin.cpp
    d3d/d3d9.cpp
    d3d/d3d9matfx.cpp
    d3d/d3d9render.cpp
    d3d/d3d9skin.cpp
    d3d/d3d9skinmatfx.cpp
    d3d/d3d9ff.cpp
    d3d/d3d.cpp
    d3d/d3d11buffer.cpp
    d3d/d3d11device.cpp
    d3d/d3d11raster.cpp
    d3d/d3d11shader.cpp
    d3d/d3d11state.cpp
    d3d/d3ddevice.cpp
    d3d/d3ddispatch.cpp
    d3d/d3dimmed.cpp
    d3d/d3drender.cpp
    d3d/vkdevice.cpp
    d3d/vkpipeline.cpp
    d3d/vkresource.cpp
    d3d/vkmem.cpp
    d3d/rwd3d8.h
    d3d/rwd3d9.h
    d3d/rwd3d11.h
    d3d/rwd3dvk.h
    d3d/vkincludes.h
    d3d/rwd3d.h
    d3d/rwd3dimpl.h
    d3d/rwxbox.h
    d3d/rwxboximpl.h
    d3d/xbox.cpp
    d3d/xboxmatfx.cpp
    d3d/xboxskin.cpp
    d3d/xboxvfmt.cpp

    gl/gl3.cpp
    gl/gl3device.cpp
    gl/gl3immed.cpp
    gl/gl3matfx.cpp
    gl/gl3pipe.cpp
    gl/gl3raster.cpp
    gl/gl3render.cpp
    gl/gl3shader.cpp
    gl/gl3skin.cpp
    gl/gl3skinmatfx.cpp
    gl/rwgl3.h
    gl/rwgl3impl.h
    gl/rwgl3plg.h
    gl/rwgl3shader.h
    gl/rwwdgl.h
    gl/wdgl.cpp
    gl/glad/glad.h
    gl/glad/glad.c
    gl/glad/khrplatform.h

    lodepng/lodepng.h
    lodepng/lodepng.cpp

    ps2/pds.cpp
    ps2/ps2.cpp
    ps2/ps2device.cpp
    ps2/ps2matfx.cpp
    ps2/ps2raster.cpp
    ps2/ps2skin.cpp
    ps2/rwps2.h
    ps2/rwps2impl.h
    ps2/rwps2plg.h
)
add_library(librw::librw ALIAS librw)

# The compiled shader blobs are included by PATH -- shaders/ for the SM2 ones,
# shaders11/ for the SM4 ones -- because a build can carry both backends and the
# two trees use the same file names. Nothing to put on the include path.

target_include_directories(librw
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${LIBRW_INSTALL_INCLUDEDIR}>
)

# One RW_<platform> per enabled backend, and PUBLIC because an application that
# mirrors librw's types has to be built against the same set.
set(LIBRW_PLATFORM_DEFINES)
foreach(_p IN LISTS LIBRW_PLATFORM)
    list(APPEND LIBRW_PLATFORM_DEFINES "RW_${_p}")
endforeach()

target_compile_definitions(librw
    PRIVATE
        LODEPNG_NO_COMPILE_CPP
        "$<IF:$<CONFIG:DEBUG>,DEBUG,NDEBUG>"
    PUBLIC
        ${LIBRW_PLATFORM_DEFINES}
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(librw
        PRIVATE
            m
    )
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_compile_options(librw
        PRIVATE
            "-Wall"
    )
    if (NOT LIBRW_PLATFORM_PS2)
        target_compile_options(librw
            PRIVATE
                "-Wextra"
                "-Wdouble-promotion"
                "-Wpedantic"
        )
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(librw
        PUBLIC
            /wd4996 /wd4244
    )
endif()

set_target_properties(librw
    PROPERTIES
        C_STANDARD 11
        C_EXTENSIONS OFF
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 11
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
        PREFIX ""
)

if(LIBRW_PLATFORM_GL3)
    if (LIBRW_GL3_GFXLIB STREQUAL "GLFW")
        if (NOT TARGET glfw)
            find_package(glfw3 REQUIRED)
        endif()
        target_compile_definitions(librw PUBLIC LIBRW_GLFW)
        target_link_libraries(librw
            PUBLIC
                glfw
            )
    elseif (LIBRW_GL3_GFXLIB STREQUAL "SDL2")
        if (NOT TARGET SDL2::SDL2)
            find_package(SDL2 CONFIG)
            if (NOT TARGET SDL2::SDL2)
                find_package(SDL2 MODULE REQUIRED)
            endif()
        endif()
        target_compile_definitions(librw PUBLIC LIBRW_SDL2)
        target_link_libraries(librw
            PUBLIC
                SDL2::SDL2
        )
    elseif (LIBRW_GL3_GFXLIB STREQUAL "SDL3")
        find_package(SDL3 CONFIG REQUIRED)
        target_compile_definitions(librw PUBLIC LIBRW_SDL3)
        target_link_libraries(librw
            PUBLIC
                SDL3::SDL3
        )
    endif()

    # Android has no desktop GL and no GL/gl.h for find_package(OpenGL) to
    # find, so it fails outright there -- and there is nothing for it to find
    # on this build's behalf anyway. glad holds every entry point as a
    # function pointer and fills them from SDL_GL_GetProcAddress at run time
    # (--no-loader; see src/gl/glad/glad.h), so nothing in librw refers to a
    # GL symbol at link time. SDL loads libGLESv2 itself.
    if(ANDROID)
        message(STATUS "librw: Android -- GLES entry points come from SDL at run time, nothing to link")
    else()
        set(OpenGL_GL_PREFERENCE GLVND)
        find_package(OpenGL)
        if(TARGET OpenGL::OpenGL)
            target_link_libraries(librw
                PRIVATE
                    OpenGL::OpenGL
            )
        elseif(TARGET OpenGL::EGL)
            target_link_libraries(librw
                PRIVATE
                    OpenGL::EGL
            )
        elseif(TARGET OpenGL::GL)
            target_link_libraries(librw
                PRIVATE
                    OpenGL::GL
            )
        else()
            message(FATAL_ERROR "find_package(OpenGL) failed.")
        endif()
    endif()
endif()

# Three independent ifs, not one chain: a build can enable GL3 and a D3D backend
# together, and an elseif would have linked only the first of them.
if(LIBRW_PLATFORM_D3D9)
    target_link_libraries(librw
        PRIVATE
            d3d9
            xinput
    )
endif()

if(LIBRW_PLATFORM_D3D11)
    target_link_libraries(librw
        PRIVATE
            d3d11
            dxgi
            dxguid
            xinput
    )
endif()

# Vulkan is loaded at run time through volk, so what it links is headers, volk
# and VMA -- no import library, and no Vulkan SDK needed to build. An embedding
# project that already has the targets keeps its own; otherwise they are looked
# for as installed packages. The window is SDL's.
if(LIBRW_PLATFORM_VULKAN)
    if(NOT TARGET volk::volk)
        find_package(volk CONFIG REQUIRED)
    endif()
    if(NOT TARGET GPUOpen::VulkanMemoryAllocator)
        find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    endif()
    if(NOT TARGET SDL3::SDL3)
        find_package(SDL3 CONFIG REQUIRED)
    endif()
    target_link_libraries(librw
        PRIVATE
            volk::volk
            GPUOpen::VulkanMemoryAllocator
        PUBLIC
            SDL3::SDL3
    )
    # VMA is C++14. The rest of librw stays at 11, so only these files move.
    set_source_files_properties(
        d3d/vkdevice.cpp
        d3d/vkpipeline.cpp
        d3d/vkresource.cpp
        d3d/vkmem.cpp
        PROPERTIES
            COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/std:c++17,-std=c++17>"
    )
endif()

if(LIBRW_INSTALL)
    install(
        FILES
            "${PROJECT_SOURCE_DIR}/args.h"
            "${PROJECT_SOURCE_DIR}/rw.h"
        DESTINATION "${LIBRW_INSTALL_INCLUDEDIR}"
    )
    install(
        FILES
            base.err
            rwbase.h
            rwcharset.h
            rwerror.h
            rwplg.h
            rwrender.h
            rwengine.h
            rwpipeline.h
            rwobjects.h
            rwanim.h
            rwplugins.h
            rwuserdata.h
        DESTINATION "${LIBRW_INSTALL_INCLUDEDIR}/src"
    )
    install(
        FILES
            d3d/rwxbox.h
            d3d/rwd3d.h
            d3d/rwd3d8.h
            d3d/rwd3d9.h
            d3d/rwd3d11.h
        DESTINATION "${LIBRW_INSTALL_INCLUDEDIR}/src/d3d"
    )
    install(
        FILES
            ps2/rwps2.h
            ps2/rwps2plg.h
        DESTINATION "${LIBRW_INSTALL_INCLUDEDIR}/src/ps2"
    )
    install(
        FILES
            gl/rwwdgl.h
            gl/rwgl3.h
            gl/rwgl3plg.h
            gl/rwgl3shader.h
        DESTINATION "${LIBRW_INSTALL_INCLUDEDIR}/src/gl"
    )

    install(
        FILES
            gl/glad/glad.h
            gl/glad/khrplatform.h
        DESTINATION "${LIBRW_INSTALL_INCLUDEDIR}/src/gl/glad"
    )

    install(
        TARGETS librw
        EXPORT librw-targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()
