if(BUILD_SHARED_LIBS)
    set(_build_shared ON)
    set(_build_static OFF)
else()
    set(_build_shared OFF)
    set(_build_static ON)
endif()

set(_glfw_platform_args "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_glfw_platform_args -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON)
endif()

orcaslicer_add_cmake_project(GLFW
    URL file:///home/user/src/orca/deps/LOCAL_SRC/GLFW/3.4.zip
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=${_build_shared}
        -DGLFW_BUILD_DOCS=OFF
        -DGLFW_BUILD_EXAMPLES=OFF
        -DGLFW_BUILD_TESTS=OFF
        ${_glfw_platform_args}
)

if (MSVC)
    add_debug_dep(dep_GLFW)
endif ()
