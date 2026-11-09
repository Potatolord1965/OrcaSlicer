#/|/ Copyright (c) Prusa Research 2021 - 2022 Tomáš Mészáros @tamasmeszaros, Filip Sykala @Jony01
#/|/
#/|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#/|/
orcaslicer_add_cmake_project(Cereal
    URL "file:///home/user/src/orca/deps/LOCAL_SRC/Cereal/v1.3.0.zip"
    CMAKE_ARGS
        -DJUST_INSTALL_CEREAL=ON
        -DSKIP_PERFORMANCE_COMPARISON=ON
        -DBUILD_TESTS=OFF
)