include(GNUInstallDirs)
orcaslicer_add_cmake_project(Qhull
    URL "file:///home/user/src/orca/deps/LOCAL_SRC/Qhull/v8.0.2.zip"
    CMAKE_ARGS 
        -DINCLUDE_INSTALL_DIR=${CMAKE_INSTALL_INCLUDEDIR}
)

if (MSVC)
    add_debug_dep(dep_Qhull)
endif ()
