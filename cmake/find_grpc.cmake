option (USE_INTERNAL_GRPC_LIBRARY "Set to FALSE to use system grpc library instead of bundled" ${NOT_UNBUNDLED})

# gRPC and relates

if (USE_INTERNAL_GRPC_LIBRARY AND NOT EXISTS "${ClickHouse_SOURCE_DIR}/contrib/grpc/CMakeLists.txt")
    message (WARNING "submodules in contrib/grpc is missing. to fix try run: \n git submodule update --init --recursive")
    set (USE_INTERNAL_GRPC_LIBRARY 0)
endif()

if (USE_INTERNAL_GRPC_LIBRARY)
    # add_subdirectory(contrib/grpc ${CMAKE_CURRENT_BINARY_DIR}/grpc EXCLUDE_FROM_ALL)
    message(STATUS "Using gRPC via add_subdirectory.")

    # After using add_subdirectory, we can now use the grpc targets directly from this build.
    set(_PROTOBUF_LIBPROTOBUF libprotobuf)
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protoc>)
    set(_GRPC_GRPCPP_UNSECURE grpc++_unsecure)
    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)
else()
    # Use System grpc
    message(STATUS "Using system gRPC.")

    find_package(Protobuf REQUIRED)
    message(STATUS "Using protobuf: ${Protobuf_VERSION} : ${Protobuf_INCLUDE_DIRS}, ${Protobuf_LIBRARIES}")

    include_directories(${PROTOBUF_INCLUDE_DIRS})

    find_package(c-ares REQUIRED)
    message(STATUS "Lib c-ares found")

    find_package(ZLIB REQUIRED)
    message(STATUS "Using ZLIB: ${ZLIB_INCLUDE_DIRS}, ${ZLIB_LIBRARIES}")

    find_package(gRPC CONFIG REQUIRED)
    message(STATUS "Using gRPC: ${gRPC_VERSION}")

    set(gRPC_FOUND TRUE)

    set(_PROTOBUF_LIBPROTOBUF protobuf::libprotobuf)
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
    set(_GRPC_GRPCPP_UNSECURE gRPC::grpc++_unsecure)
    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
endif()

