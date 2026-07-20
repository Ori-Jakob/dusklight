cmake_minimum_required(VERSION 3.25)
project(lantern LANGUAGES C CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to Dusklight source root")
    option(DUSK_MOD_USE_FULL_TREE "Use full Dusklight tree instead of the standalone SDK" OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if (DUSK_MOD_USE_FULL_TREE)
        add_subdirectory("${DUSK_DIR}" dusk EXCLUDE_FROM_ALL)
    else ()
        add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
    endif ()
endif ()

option(LANTERN_ENABLE_NETWORKING "Build GameNetworkingSockets transport support" ON)
option(LANTERN_BUILD_SERVER "Build the self-hosted Lantern relay" ON)
option(LANTERN_BUILD_TESTS "Build Lantern unit and integration tests" ON)

add_library(lantern_protocol STATIC
    common/protocol.cpp
)
target_include_directories(lantern_protocol PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(lantern_protocol PUBLIC cxx_std_20)
set_target_properties(lantern_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (LANTERN_ENABLE_NETWORKING)
    include(FetchContent)

    # GameNetworkingSockets uses protobuf for its internal wire protocol. Pin both dependencies so
    # standalone SDK and in-tree builds produce the same transport ABI.
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(Protobuf_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
    set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
    FetchContent_Declare(Protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG v21.12
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR cmake
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(Protobuf)

    # Dusklight's MSVC options are inherited by subdirectories. CMake forwards /bigobj to protobuf's
    # optional Windows version resources under clang-cl, which rc.exe rejects. Static transport
    # libraries do not need embedded version resources, so omit those four generated .rc inputs.
    if (MSVC)
        foreach (_protobuf_target libprotobuf libprotobuf-lite libprotoc protoc)
            if (TARGET ${_protobuf_target})
                get_target_property(_protobuf_sources ${_protobuf_target} SOURCES)
                if (_protobuf_sources)
                    list(FILTER _protobuf_sources EXCLUDE REGEX "version\\.rc$")
                    set_property(TARGET ${_protobuf_target} PROPERTY SOURCES "${_protobuf_sources}")
                endif ()
            endif ()
        endforeach ()
    endif ()

    # FetchContent's find-package redirect intentionally exposes the protobuf targets but not the
    # legacy helper expected by GameNetworkingSockets. Keep this tiny compatibility wrapper local
    # to Lantern rather than modifying either dependency.
    if (NOT COMMAND protobuf_generate_cpp)
        function(protobuf_generate_cpp out_sources out_headers)
            set(_generated_sources "")
            set(_generated_headers "")
            foreach (_proto IN LISTS ARGN)
                get_filename_component(_proto_abs "${_proto}" ABSOLUTE
                    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
                get_filename_component(_proto_dir "${_proto_abs}" DIRECTORY)
                get_filename_component(_proto_name "${_proto_abs}" NAME_WE)
                set(_source "${CMAKE_CURRENT_BINARY_DIR}/${_proto_name}.pb.cc")
                set(_header "${CMAKE_CURRENT_BINARY_DIR}/${_proto_name}.pb.h")
                add_custom_command(
                    OUTPUT "${_source}" "${_header}"
                    COMMAND "$<TARGET_FILE:protobuf::protoc>"
                        "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}"
                        "-I${_proto_dir}" "${_proto_abs}"
                    DEPENDS protobuf::protoc "${_proto_abs}"
                    VERBATIM
                )
                list(APPEND _generated_sources "${_source}")
                list(APPEND _generated_headers "${_header}")
            endforeach ()
            set(${out_sources} "${_generated_sources}" PARENT_SCOPE)
            set(${out_headers} "${_generated_headers}" PARENT_SCOPE)
        endfunction()
    endif ()

    set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
    set(USE_CRYPTO BCrypt CACHE STRING "" FORCE)
    set(USE_CRYPTO25519 Reference CACHE STRING "" FORCE)
    set(MSVC_CRT_STATIC OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(lantern_gns
        GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
        GIT_TAG v1.5.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(lantern_gns)

    # Dusklight enables an intentionally broad warning set. Keep it on Lantern itself, but do not
    # apply it to the pinned third-party sources (which otherwise produce megabytes of diagnostics
    # under clang-cl).
    if (MSVC)
        foreach (_lantern_third_party libprotobuf libprotobuf-lite libprotoc protoc
                GameNetworkingSockets_s)
            if (TARGET ${_lantern_third_party})
                target_compile_options(${_lantern_third_party} PRIVATE /w)
            endif ()
        endforeach ()
    endif ()

    add_library(lantern_transport STATIC
        common/gns_runtime.cpp
        common/network_client.cpp
    )
    target_include_directories(lantern_transport PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_compile_features(lantern_transport PUBLIC cxx_std_20)
    target_compile_definitions(lantern_transport PUBLIC LANTERN_HAS_GNS=1)
    target_link_libraries(lantern_transport PUBLIC lantern_protocol GameNetworkingSockets::static)
    set_target_properties(lantern_transport PROPERTIES POSITION_INDEPENDENT_CODE ON)
else ()
    add_library(lantern_transport INTERFACE)
    target_compile_definitions(lantern_transport INTERFACE LANTERN_HAS_GNS=0)
    target_link_libraries(lantern_transport INTERFACE lantern_protocol)
endif ()

add_mod(lantern_core
    SOURCES core/src/mod.cpp
    MOD_JSON core/mod.json
    RES_DIR core/res
)
target_include_directories(lantern_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
)
target_link_libraries(lantern_core PRIVATE lantern_transport)

add_mod(lantern_tp
    FEATURES game webgpu
    SOURCES
        tp/src/mod.cpp
        tp/src/nameplates.cpp
        tp/src/presence.cpp
    MOD_JSON tp/mod.json
    RES_DIR tp/res
)
target_include_directories(lantern_tp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/tp/src"
)
target_link_libraries(lantern_tp PRIVATE lantern_protocol)

if (LANTERN_BUILD_SERVER AND LANTERN_ENABLE_NETWORKING)
    add_executable(lantern_server
        server/src/main.cpp
        server/src/relay.cpp
    )
    target_include_directories(lantern_server PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/server/src"
    )
    target_compile_features(lantern_server PRIVATE cxx_std_20)
    target_link_libraries(lantern_server PRIVATE lantern_transport)
endif ()

if (LANTERN_BUILD_TESTS)
    include(CTest)
    add_executable(lantern_protocol_tests
        tests/protocol_tests.cpp
        tp/src/presence.cpp
    )
    target_include_directories(lantern_protocol_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tp/src"
    )
    target_compile_features(lantern_protocol_tests PRIVATE cxx_std_20)
    target_link_libraries(lantern_protocol_tests PRIVATE lantern_protocol)
    if (WIN32)
        # Also works when the selected CTest comes from an MSYS environment, which otherwise
        # prepends its POSIX build path to CMake's absolute Windows executable path.
        add_test(NAME lantern_protocol_tests
            COMMAND powershell.exe -NoProfile -Command
                "& '$<TARGET_FILE:lantern_protocol_tests>'"
        )
    else ()
        add_test(NAME lantern_protocol_tests COMMAND lantern_protocol_tests)
    endif ()

    if (WIN32 AND LANTERN_BUILD_SERVER AND LANTERN_ENABLE_NETWORKING)
        add_executable(lantern_relay_integration tests/relay_integration.cpp)
        target_compile_features(lantern_relay_integration PRIVATE cxx_std_20)
        target_link_libraries(lantern_relay_integration PRIVATE lantern_transport)
        add_test(NAME lantern_relay_integration
            COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_relay_integration.ps1"
                -Server "$<TARGET_FILE:lantern_server>"
                -Client "$<TARGET_FILE:lantern_relay_integration>"
        )
        set_tests_properties(lantern_relay_integration PROPERTIES TIMEOUT 20)
    endif ()
endif ()
