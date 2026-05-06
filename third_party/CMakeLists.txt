# ─── Third-party Dependencies (portable, no system install required) ──────────
#
# All dependencies are fetched at CMake configure time via FetchContent.
# FFmpeg is pre-built and shipped in third_party/ffmpeg/.
# CUDA headers are optional — NVDEC works through FFmpeg's hwaccel API.
#
# To use on a new machine: just run setup.ps1 then build.ps1.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# ─── spdlog (fast logging) ───────────────────────────────────────────────────
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.0
    GIT_SHALLOW    TRUE
)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# ─── GLM (math library — header only) ────────────────────────────────────────
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# ─── Google Test ─────────────────────────────────────────────────────────────
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ─── Vulkan-Headers (must come before volk) ──────────────────────────────────
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        vulkan-sdk-1.3.290.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(vulkan_headers)

# ─── volk (Vulkan meta-loader — no Vulkan SDK needed) ────────────────────────
# Tell volk where to find Vulkan headers (from our FetchContent above)
set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR CACHE STRING "" FORCE)
set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_INSTALL_DIR "${vulkan_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.3.290.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(volk)

# ─── VMA (Vulkan Memory Allocator — header only) ─────────────────────────────
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.2.1
    GIT_SHALLOW    TRUE
)
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(vma)

# ─── stb (single-header image/truetype libs) ─────────────────────────────────
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master  # stb doesn't tag releases
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(stb)
# stb is header-only, create an interface target
if(NOT TARGET stb)
    add_library(stb INTERFACE)
    target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})
endif()

# ─── FFmpeg (pre-built, shipped in third_party/ffmpeg/) ──────────────────────
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg" CACHE PATH "FFmpeg pre-built directory")

if(EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(STATUS "FFmpeg found at: ${FFMPEG_ROOT}")

    # Create imported targets for each FFmpeg library
    foreach(_lib avcodec avformat avutil swscale swresample)
        add_library(ffmpeg::${_lib} SHARED IMPORTED GLOBAL)
        set_target_properties(ffmpeg::${_lib} PROPERTIES
            IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/${_lib}.lib"
            IMPORTED_LOCATION "${FFMPEG_ROOT}/bin/${_lib}*.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
        )
    endforeach()

    # Convenience target linking all FFmpeg libs
    add_library(ffmpeg::ffmpeg INTERFACE IMPORTED GLOBAL)
    target_link_libraries(ffmpeg::ffmpeg INTERFACE
        ffmpeg::avcodec
        ffmpeg::avformat
        ffmpeg::avutil
        ffmpeg::swscale
        ffmpeg::swresample
    )
    target_include_directories(ffmpeg::ffmpeg INTERFACE "${FFMPEG_ROOT}/include")

    set(FFMPEG_FOUND TRUE CACHE BOOL "FFmpeg found" FORCE)
    set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin" CACHE PATH "FFmpeg DLL directory" FORCE)
else()
    message(WARNING "FFmpeg NOT found at ${FFMPEG_ROOT}. Video decode will not be available.")
    message(WARNING "Download pre-built FFmpeg from https://github.com/BtbN/FFmpeg-Builds/releases")
    message(WARNING "Extract to third_party/ffmpeg/ (should contain include/, lib/, bin/)")
    set(FFMPEG_FOUND FALSE CACHE BOOL "FFmpeg not found" FORCE)
endif()

# ─── CUDA (optional, runtime-loaded) ─────────────────────────────────────────
# We don't require CUDA Toolkit installed. Instead we:
# 1. Use FFmpeg's hwaccel API for NVDEC (goes through FFmpeg)
# 2. For direct CUDA interop, dynamically load nvcuda.dll at runtime
# The CUDA driver API headers are minimal — we provide stubs if needed.
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    message(STATUS "CUDA Toolkit found: ${CUDAToolkit_VERSION}")
    # Make visible globally (cache variable propagates to all scopes)
    set(CUDAToolkit_FOUND TRUE CACHE BOOL "CUDA Toolkit found" FORCE)
else()
    message(STATUS "CUDA Toolkit not installed — using FFmpeg hwaccel for GPU decode")
    message(STATUS "NVDEC still works via FFmpeg's CUDA hwaccel (driver provides nvcuda.dll)")
endif()

# ─── PortAudio (real-time audio I/O — WASAPI on Windows) ─────────────────────
FetchContent_Declare(portaudio
    GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
    GIT_TAG        v19.7.0
    GIT_SHALLOW    TRUE
)
set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PA_USE_WASAPI ON CACHE BOOL "" FORCE)
set(PA_USE_WDMKS OFF CACHE BOOL "" FORCE)
set(PA_USE_DS OFF CACHE BOOL "" FORCE)
set(PA_USE_WMME OFF CACHE BOOL "" FORCE)
set(PA_USE_ASIO OFF CACHE BOOL "" FORCE)
set(PA_USE_JACK OFF CACHE BOOL "" FORCE)
# Use dynamic CRT (/MD) to match the rest of the project.
# PortAudio defaults PA_DLL_LINK_WITH_STATIC_RUNTIME=ON which replaces /MD
# with /MT, causing unresolved CRT symbols (memcpy, fflush, _fltused, etc.)
# when the main project and dependencies are normalized to dynamic CRT.
set(PA_DLL_LINK_WITH_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
# Only build the static library — the shared DLL target fails to link CRT
# symbols correctly under FetchContent, and we only need the static lib.
set(PA_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PA_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(portaudio)

if(TARGET portaudio_static)
    set(PORTAUDIO_TARGET portaudio_static CACHE STRING "PortAudio target name" FORCE)
    set(PORTAUDIO_FOUND TRUE CACHE BOOL "PortAudio found" FORCE)
    message(STATUS "PortAudio: static library (WASAPI)")
elseif(TARGET portaudio)
    set(PORTAUDIO_TARGET portaudio CACHE STRING "PortAudio target name" FORCE)
    set(PORTAUDIO_FOUND TRUE CACHE BOOL "PortAudio found" FORCE)
    message(STATUS "PortAudio: shared library (WASAPI)")
else()
    set(PORTAUDIO_FOUND FALSE CACHE BOOL "PortAudio not found" FORCE)
    message(WARNING "PortAudio: build failed")
endif()

# ─── libsndfile (audio file I/O — WAV, FLAC, OGG, etc.) ─────────────────────
FetchContent_Declare(libsndfile
    GIT_REPOSITORY https://github.com/libsndfile/libsndfile.git
    GIT_TAG        1.2.2
    GIT_SHALLOW    TRUE
)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTERNAL_LIBS OFF CACHE BOOL "" FORCE)  # No Ogg/Vorbis/FLAC deps for now
set(BUILD_REGTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_MPEG OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(ENABLE_PACKAGE_CONFIG OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libsndfile)

if(TARGET sndfile)
    set(SNDFILE_FOUND TRUE CACHE BOOL "libsndfile found" FORCE)
    message(STATUS "libsndfile: available (WAV, AIFF, AU, RAW, W64, CAF)")
else()
    set(SNDFILE_FOUND FALSE CACHE BOOL "libsndfile not found" FORCE)
    message(WARNING "libsndfile: build failed")
endif()

# ─── SoundTouch (real-time pitch-preserving time-stretch) ─────────────────────
FetchContent_Declare(soundtouch
    GIT_REPOSITORY https://codeberg.org/soundtouch/soundtouch.git
    GIT_TAG        2.3.3
    GIT_SHALLOW    TRUE
)
set(SOUNDTOUCH_DLL OFF CACHE BOOL "" FORCE)
set(SOUNDSTRETCH OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(soundtouch)

if(TARGET SoundTouch)
    set(SOUNDTOUCH_FOUND TRUE CACHE BOOL "SoundTouch found" FORCE)
    message(STATUS "SoundTouch: pitch-preserving time-stretch (static)")
    if(MSVC)
        target_compile_options(SoundTouch PRIVATE /W0)
    else()
        target_compile_options(SoundTouch PRIVATE -w)
    endif()
else()
    set(SOUNDTOUCH_FOUND FALSE CACHE BOOL "SoundTouch not found" FORCE)
    message(WARNING "SoundTouch: build failed")
endif()

# ─── spine-cpp (Spine 4.1 C++ runtime — vendored source) ─────────────────────
set(SPINE_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spine-cpp")
if(EXISTS "${SPINE_CPP_DIR}/include/spine/spine.h")
    file(GLOB SPINE_CPP_SOURCES "${SPINE_CPP_DIR}/src/spine/*.cpp")
    add_library(spine-cpp STATIC ${SPINE_CPP_SOURCES})
    target_include_directories(spine-cpp PUBLIC "${SPINE_CPP_DIR}/include")
    # Build as static lib, no DLL export decorators needed
    target_compile_definitions(spine-cpp PRIVATE SP_API=)
    # Suppress warnings from third-party code
    if(MSVC)
        target_compile_options(spine-cpp PRIVATE /W0)
    else()
        target_compile_options(spine-cpp PRIVATE -w)
    endif()
    set(SPINE_CPP_FOUND TRUE CACHE BOOL "spine-cpp found" FORCE)
    message(STATUS "spine-cpp: Spine 4.1 C++ runtime (static)")
else()
    set(SPINE_CPP_FOUND FALSE CACHE BOOL "spine-cpp not found" FORCE)
    message(WARNING "spine-cpp: NOT found at ${SPINE_CPP_DIR}")
endif()

# ─── whisper.cpp (speech-to-text — optional) ─────────────────────────────────
option(ROUNDTABLE_ENABLE_WHISPER "Enable whisper.cpp transcription" ON)
if(ROUNDTABLE_ENABLE_WHISPER)
    FetchContent_Declare(whisper
        GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
        GIT_TAG        v1.7.4
        GIT_SHALLOW    TRUE
    )
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(GGML_METAL OFF CACHE BOOL "" FORCE)
    set(GGML_CUDA OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(whisper)

    if(TARGET whisper)
        set(WHISPER_FOUND TRUE CACHE BOOL "whisper.cpp found" FORCE)
        message(STATUS "whisper.cpp: speech-to-text (static)")
        # Suppress warnings from whisper third-party code
        if(MSVC)
            target_compile_options(whisper PRIVATE /W0)
        endif()
    else()
        set(WHISPER_FOUND FALSE CACHE BOOL "whisper.cpp not found" FORCE)
        message(WARNING "whisper.cpp: build failed")
    endif()
else()
    set(WHISPER_FOUND FALSE CACHE BOOL "whisper.cpp disabled" FORCE)
    message(STATUS "whisper.cpp: disabled (set ROUNDTABLE_ENABLE_WHISPER=ON to enable)")
endif()
