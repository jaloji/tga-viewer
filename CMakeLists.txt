cmake_minimum_required(VERSION 3.20)
project(tga-viewer VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "tga-viewer builds a Windows WIC codec and needs Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL is loaded into Explorer, so it must not need a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX)
endif()

# All binaries land in one folder next to each other.
foreach(kind RUNTIME LIBRARY ARCHIVE)
    set(CMAKE_${kind}_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG)
        set(CMAKE_${kind}_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin)
    endforeach()
endforeach()

# TGA decoder shared by every component.
add_library(tga_core STATIC src/core/tga.cpp src/core/tga.h)
target_include_directories(tga_core PUBLIC src/core)

# Windows Imaging Component codec: Explorer thumbnails, preview pane, photo viewers.
add_library(TgaWic SHARED src/wic/tgawic.cpp src/wic/tgawic.def)
target_link_libraries(TgaWic PRIVATE tga_core windowscodecs ole32 advapi32 shell32)

# Command-line tools.
add_executable(tga2png tools/tga2png.cpp)
target_link_libraries(tga2png PRIVATE tga_core)

add_executable(wictest tools/wictest.cpp)
target_link_libraries(wictest PRIVATE windowscodecs ole32)

# Release package (cmake --install build --prefix dist).
install(TARGETS TgaWic tga2png wictest RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES scripts/install.bat scripts/uninstall.bat scripts/clear-thumbnails.bat README.md LICENSE
        DESTINATION .)
