cmake_minimum_required(VERSION 3.11)
project(SkinGetBE)

set(CMAKE_CXX_STANDARD 17)

# Static build configuration
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Build zlib examples" FORCE)

if(MSVC)
    # Static runtime for MSVC
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Fetch zlib from GitHub
include(FetchContent)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1 # Or any specific version/branch
)
FetchContent_MakeAvailable(zlib)

# Project source files
set(PROJECT_SOURCES
    main.cpp
    Util/Logger.cpp
)

add_executable(SkinGetBE ${PROJECT_SOURCES})

# target_link_libraries will handle include directories and linking for fetched zlib
target_link_libraries(SkinGetBE PRIVATE zlibstatic)

if(WIN32)
    target_link_libraries(SkinGetBE PRIVATE ws2_32 wsock32)
    if(MINGW)
        target_link_options(SkinGetBE PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    target_link_libraries(SkinGetBE PRIVATE pthread)
    target_link_options(SkinGetBE PRIVATE -static-libgcc -static-libstdc++)
endif()
