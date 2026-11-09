orcaslicer_add_cmake_project(NLopt
  URL "file:///home/user/src/orca/deps/LOCAL_SRC/NLopt/v2.5.0.tar.gz"
  CMAKE_ARGS
    -DNLOPT_PYTHON:BOOL=OFF
    -DNLOPT_OCTAVE:BOOL=OFF
    -DNLOPT_MATLAB:BOOL=OFF
    -DNLOPT_GUILE:BOOL=OFF
    -DNLOPT_SWIG:BOOL=OFF
    -DNLOPT_TESTS:BOOL=OFF
    # testopt is built regardless of NLOPT_TESTS; see deps-windows.cmake.
    "${DEP_LLD_FORCE_MULTIPLE}"
)

if (MSVC)
    add_debug_dep(dep_NLopt)
endif ()
