if (USE_INTERNAL_GRPC_LIBRARY AND NOT EXISTS "${ClickHouse_SOURCE_DIR}/contrib/tiflash-proxy/CMakeLists.txt")
    message (WARNING "submodules in contrib/grpc is missing. to fix try run: \n git submodule update --init --recursive")
endif()
message(STATUS "Using proxy: ${ClickHouse_SOURCE_DIR}/contrib/tiflash-proxy")
