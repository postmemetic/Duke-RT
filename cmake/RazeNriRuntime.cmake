include_guard(GLOBAL)

# NRI is an independently built DLL. Do not inherit the game's /MT, vcpkg
# triplet, or global compiler flags into its third-party SDK dependencies.
function(raze_add_nri_runtime target_name runtime_directory)
    if(NOT WIN32 OR NOT HAVE_NRI)
        return()
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.30)
        message(FATAL_ERROR "Windows NRI builds require CMake 3.30 or newer to build the vendored runtime.")
    endif()
    include(ExternalProject)
    set(RAZE_NRI_DEPENDENCY_SOURCE_DIR "" CACHE PATH "Optional existing NRI _deps directory; reuse dependency sources, never another build's objects.")
    set(RAZE_NRI_BUILD_JOBS "4" CACHE STRING "Maximum parallel jobs in the isolated NRI runtime build.")
    if(NOT RAZE_NRI_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "RAZE_NRI_BUILD_JOBS must be a positive integer")
    endif()
    get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT _multi AND NOT CMAKE_BUILD_TYPE)
        message(FATAL_ERROR "Set CMAKE_BUILD_TYPE for a Windows NRI build.")
    endif()
    if(_multi)
        set(_build "${CMAKE_BINARY_DIR}/nri-runtime/multi-config")
        string(REPLACE ";" "|" _configurations "${CMAKE_CONFIGURATION_TYPES}")
        set(_configuration_args "-DCMAKE_CONFIGURATION_TYPES:STRING=${_configurations}")
    else()
        set(_build "${CMAKE_BINARY_DIR}/nri-runtime/${CMAKE_BUILD_TYPE}")
        set(_configuration_args "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}")
    endif()
    set(_args
        "-DRAZE_SOURCE_ROOT:PATH=${CMAKE_SOURCE_DIR}"
        "-DRAZE_NRI_DEPENDENCY_SOURCE_DIR:PATH=${RAZE_NRI_DEPENDENCY_SOURCE_DIR}"
        ${_configuration_args}
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH="
        "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}")
    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        list(APPEND _args "-DCMAKE_SYSTEM_VERSION:STRING=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    endif()
    if(CMAKE_MAKE_PROGRAM)
        list(APPEND _args "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}")
    endif()
    ExternalProject_Add(raze_nri_runtime_build
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/cmake/nri-runtime"
        BINARY_DIR "${_build}"
        LIST_SEPARATOR "|"
        CMAKE_ARGS ${_args}
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG> --target NRI --parallel ${RAZE_NRI_BUILD_JOBS}
        BUILD_ALWAYS TRUE
        INSTALL_COMMAND ""
        EXCLUDE_FROM_ALL TRUE)
    add_custom_target(raze_nri_runtime_stage
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DBUILD_INPUTS=${_build}/nri-build-inputs-$<CONFIG>.cmake"
            "-DRUNTIME_DIR=${runtime_directory}"
            -P "${CMAKE_SOURCE_DIR}/cmake/StageNriRuntime.cmake"
        DEPENDS raze_nri_runtime_build
        COMMENT "Staging the source-built NRI runtime and verifying its provenance"
        VERBATIM)
    add_dependencies(${target_name} raze_nri_runtime_stage)
endfunction()

# Generic audio/runtime caches may contain old graphics DLLs. Only the source
# runtime owner may stage these names, regardless of which cache contains them.
function(raze_is_nri_owned_runtime path result)
    get_filename_component(_name "${path}" NAME)
    string(TOLOWER "${_name}" _name)
    if(_name MATCHES "^(nri\\.(dll|pdb)|amd_ags_.*\\.dll|amd_fidelityfx_.*\\.dll|libxess\\.dll|nvngx_dlss[d]?\\.dll|d3d12core\\.dll|d3d12sdklayers\\.dll)$")
        set(${result} TRUE PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()
