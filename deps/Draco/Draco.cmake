set(_options "")
if (MSVC AND DEP_DEBUG)
    set(_options "FORWARD_CONFIG")
endif ()

orcaslicer_add_cmake_project(Draco
    ${_options}
    URL file:///home/user/src/orca/deps/LOCAL_SRC/Draco/1.5.7.zip
    CMAKE_ARGS
        # The encoder and decoder tools duplicate draco.lib; see deps-windows.cmake.
        "${DEP_LLD_FORCE_MULTIPLE}"
)