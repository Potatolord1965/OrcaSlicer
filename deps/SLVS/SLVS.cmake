# libslvs — the geometric constraint solver behind the Design tab's sketch constraints.
# Extraction of solvespace.com's libslvs, taken verbatim from JacobStoren/SolveSpaceLib;
# only the CMakeLists is ours, because upstream's builds a demo and installs nothing.
# GPLv3, compatible with this fork's licence. Self-contained: no external dependencies.
orcaslicer_add_cmake_project(SLVS
    URL file:///home/user/src/orca/deps/LOCAL_SRC/SLVS/4d8704523e4bf212fadf5189f92484244f670fea.zip
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt.in ./CMakeLists.txt
)

if (MSVC)
    add_debug_dep(dep_SLVS)
endif ()
