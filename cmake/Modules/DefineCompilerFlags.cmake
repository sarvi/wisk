if (UNIX AND NOT WIN32)
    # Activate with: -DCMAKE_BUILD_TYPE=Profiling
    set(CMAKE_C_FLAGS_PROFILING "-O0 -g -fprofile-arcs -ftest-coverage"
        CACHE STRING "Flags used by the C compiler during PROFILING builds.")
    set(CMAKE_CXX_FLAGS_PROFILING "-O0 -g -fprofile-arcs -ftest-coverage"
        CACHE STRING "Flags used by the CXX compiler during PROFILING builds.")
    set(CMAKE_SHARED_LINKER_FLAGS_PROFILING "-fprofile-arcs -ftest-coverage"
        CACHE STRING "Flags used by the linker during the creation of shared libraries during PROFILING builds.")
    set(CMAKE_MODULE_LINKER_FLAGS_PROFILING "-fprofile-arcs -ftest-coverage"
        CACHE STRING "Flags used by the linker during the creation of shared libraries during PROFILING builds.")
    set(CMAKE_EXEC_LINKER_FLAGS_PROFILING "-fprofile-arcs -ftest-coverage"
        CACHE STRING "Flags used by the linker during PROFILING builds.")
endif()
