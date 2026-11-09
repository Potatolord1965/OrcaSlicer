if (MSVC)
    set(_patch_command ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/MSVC.cmake ./cmake/compilers/MSVC.cmake)
elseif (FLATPAK AND "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(_patch_command ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/GNU.cmake ./cmake/compilers/GNU.cmake)
else()
    set(_patch_command "")
endif()

orcaslicer_add_cmake_project(
    TBB
    URL "file:///home/user/src/orca/deps/LOCAL_SRC/TBB/v2021.5.0.zip"
    PATCH_COMMAND ${_patch_command}
    CMAKE_ARGS          
        -DTBB_BUILD_SHARED=OFF
        -DTBB_BUILD_TESTS=OFF
        -DTBB_TEST=OFF
        -DTBB_DISABLE_HWLOC_AUTOMATIC_SEARCH=ON
        -DTBB_ENABLE_IPO=OFF
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_DEBUG_POSTFIX=_debug
)

if (MSVC)
    add_debug_dep(dep_TBB)
endif ()


