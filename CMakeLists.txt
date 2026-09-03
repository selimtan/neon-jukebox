cmake_minimum_required(VERSION 3.28)

project(NeonJukebox VERSION 1.0.0 LANGUAGES C CXX)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
include(CTest)
set(NEON_BUILD_TESTING ${BUILD_TESTING})

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.14
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

set(SDLMIXER_VENDORED ON CACHE BOOL "" FORCE)
set(SDLMIXER_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDLMIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_AIFF OFF CACHE BOOL "" FORCE)
set(SDLMIXER_AU OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VOC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_GME OFF CACHE BOOL "" FORCE)
set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_LIBFLAC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_DRFLAC ON CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_DRMP3 ON CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_STB ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_mixer
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
    GIT_TAG release-3.2.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3_mixer)

set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
set(SDLIMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_ANI OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BMP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_GIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_LBM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PCX OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_QOI OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SVG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TGA OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XCF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XPM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XV OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG release-3.4.4
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    GIT_SUBMODULES_RECURSE FALSE)
FetchContent_MakeAvailable(SDL3_image)

set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_TESTS OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG release-3.2.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3_ttf)

set(BUILD_TESTING OFF CACHE BOOL "Disable dependency tests" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(taglib
    GIT_REPOSITORY https://github.com/taglib/taglib.git
    GIT_TAG v2.3.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(taglib)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json)
set(BUILD_TESTING ${NEON_BUILD_TESTING} CACHE BOOL "Build Neon Jukebox tests" FORCE)

add_library(neon_core
    src/Models.cpp
    src/Utils.cpp
    src/Queue.cpp
    src/Security.cpp
    src/Storage.cpp
    src/Library.cpp
    src/OnlineArtwork.cpp)
target_include_directories(neon_core PUBLIC include)
target_include_directories(neon_core SYSTEM PUBLIC
    ${taglib_SOURCE_DIR}
    ${taglib_SOURCE_DIR}/taglib
    ${taglib_SOURCE_DIR}/taglib/toolkit
    ${taglib_SOURCE_DIR}/taglib/mpeg
    ${taglib_SOURCE_DIR}/taglib/mpeg/id3v2
    ${taglib_SOURCE_DIR}/taglib/mpeg/id3v2/frames
    ${taglib_SOURCE_DIR}/taglib/flac
    ${taglib_BINARY_DIR})
target_compile_definitions(neon_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(neon_core PUBLIC tag nlohmann_json::nlohmann_json bcrypt winhttp)
if(MSVC)
    target_compile_options(neon_core PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(neon_jukebox WIN32
    src/main.cpp
    src/App.cpp
    src/Audio.cpp
    src/Video.cpp
    src/UI.cpp
    src/Visualizer.cpp
    src/Artwork.cpp)
if(WIN32)
    target_sources(neon_jukebox PRIVATE assets/windows/neon_jukebox.rc)
endif()
target_include_directories(neon_jukebox PRIVATE include)
target_compile_definitions(neon_jukebox PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(neon_jukebox PRIVATE
    neon_core
    SDL3::SDL3-static
    SDL3_mixer::SDL3_mixer-static
    SDL3_image::SDL3_image-static
    SDL3_ttf::SDL3_ttf-static
    mfplay mfplat mfuuid propsys ole32 oleaut32 uuid)
if(MSVC)
    target_compile_options(neon_jukebox PRIVATE /W4 /permissive- /utf-8)
    target_link_options(neon_jukebox PRIVATE /SUBSYSTEM:WINDOWS)
endif()

if(NEON_BUILD_TESTING)
    add_executable(neon_tests tests/test_core.cpp)
    target_link_libraries(neon_tests PRIVATE neon_core)
    target_compile_definitions(neon_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME neon_core_tests COMMAND neon_tests)

    add_executable(neon_audio_tests tests/test_audio.cpp src/Audio.cpp)
    target_include_directories(neon_audio_tests PRIVATE include)
    target_compile_definitions(neon_audio_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        NEON_MIXER_FIXTURES="${sdl3_mixer_SOURCE_DIR}/examples"
        NEON_TAGLIB_FIXTURES="${taglib_SOURCE_DIR}/tests/data")
    target_link_libraries(neon_audio_tests PRIVATE
        neon_core SDL3::SDL3-static SDL3_mixer::SDL3_mixer-static)
    if(MSVC)
        target_compile_options(neon_audio_tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME neon_audio_integration_tests COMMAND neon_audio_tests)
    set_tests_properties(neon_audio_integration_tests PROPERTIES
        ENVIRONMENT "SDL_AUDIODRIVER=dummy")

    add_executable(neon_visualizer_tests tests/test_visualizer.cpp src/Visualizer.cpp)
    target_include_directories(neon_visualizer_tests PRIVATE include)
    target_compile_definitions(neon_visualizer_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(neon_visualizer_tests PRIVATE
        SDL3::SDL3-static SDL3_mixer::SDL3_mixer-static SDL3_ttf::SDL3_ttf-static
        nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(neon_visualizer_tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME neon_visualizer_render_tests COMMAND neon_visualizer_tests)
    set_tests_properties(neon_visualizer_render_tests PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(neon_video_tests tests/test_video.cpp src/Video.cpp src/Audio.cpp)
    target_include_directories(neon_video_tests PRIVATE include)
    target_compile_definitions(neon_video_tests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(neon_video_tests PRIVATE
        neon_core SDL3::SDL3-static SDL3_mixer::SDL3_mixer-static nlohmann_json::nlohmann_json
        mfplay mfplat mfuuid propsys ole32 oleaut32 uuid)
    if(MSVC)
        target_compile_options(neon_video_tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME neon_video_integration_tests COMMAND neon_video_tests)
endif()

install(TARGETS neon_jukebox RUNTIME DESTINATION . COMPONENT Application)
install(DIRECTORY assets/ DESTINATION assets COMPONENT Application)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION . COMPONENT Application)
install(FILES docs/VISUALIZER_RESEARCH.md docs/ARTWORK_SOURCES.md DESTINATION docs COMPONENT Application)
install(FILES "${sdl3_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses
        RENAME SDL3-LICENSE.txt COMPONENT Application)
install(FILES "${sdl3_mixer_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses
        RENAME SDL3_mixer-LICENSE.txt COMPONENT Application)
install(FILES "${sdl3_image_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses
        RENAME SDL3_image-LICENSE.txt COMPONENT Application)
install(FILES "${sdl3_ttf_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses
        RENAME SDL3_ttf-LICENSE.txt COMPONENT Application)
install(FILES "${sdl3_ttf_SOURCE_DIR}/external/freetype/docs/FTL.TXT" DESTINATION licenses
        RENAME FreeType-LICENSE.txt COMPONENT Application)
install(FILES "${sdl3_ttf_SOURCE_DIR}/external/harfbuzz/COPYING" DESTINATION licenses
        RENAME HarfBuzz-LICENSE.txt COMPONENT Application)
install(FILES "${taglib_SOURCE_DIR}/COPYING.MPL" DESTINATION licenses
        RENAME TagLib-MPL-1.1.txt COMPONENT Application)
install(FILES "${json_SOURCE_DIR}/LICENSE.MIT" DESTINATION licenses
        RENAME nlohmann-json-LICENSE.txt COMPONENT Application)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME "NeonJukebox")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "NeonJukebox-${PROJECT_VERSION}-windows-x64")
set(CPACK_COMPONENTS_ALL Application)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
include(CPack)
