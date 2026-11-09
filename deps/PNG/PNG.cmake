orcaslicer_add_cmake_project(PNG
        URL file:///home/user/src/orca/deps/LOCAL_SRC/PNG/v1.6.59.zip
        DEPENDS ${ZLIB_PKG}
        CMAKE_ARGS
            -DPNG_SHARED=OFF
            -DPNG_STATIC=ON
            -DPNG_PREFIX=prusaslicer_
            -DPNG_TESTS=OFF
            -DDISABLE_DEPENDENCY_TRACKING=OFF
)

if (MSVC)
    add_debug_dep(dep_PNG)
endif ()
