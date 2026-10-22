cmake_minimum_required(VERSION 3.22)
project(AOWIS_Server_DB_Custom VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(aowis-native-store STATIC
    src/native_store.cpp
    src/model_record.cpp
    src/access_policy.cpp
    src/pack_experiment.cpp
    src/block_store_prototype.cpp
)
target_compile_features(aowis-native-store PUBLIC cxx_std_20)
target_include_directories(aowis-native-store PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aowis-native-store PUBLIC OpenSSL::Crypto)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aowis-native-store PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

# Project catalogue/lifecycle is a direct, Qt-free C++ API and remains usable
# by the standalone build independently of network transport.
add_library(aowis-native-project-service STATIC src/project_service.cpp)
add_library(AOWIS::NativeProjectService ALIAS aowis-native-project-service)
target_compile_features(aowis-native-project-service PUBLIC cxx_std_20)
target_include_directories(aowis-native-project-service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aowis-native-project-service PUBLIC aowis-native-store)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aowis-native-project-service PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(aowis-native-pack-experiment tools/native_pack_experiment.cpp)
target_link_libraries(aowis-native-pack-experiment PRIVATE aowis-native-store)

add_executable(aowis-native-block-store-benchmark tools/native_block_store_benchmark.cpp)
target_link_libraries(aowis-native-block-store-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-access tools/native_access.cpp)
target_link_libraries(aowis-native-access PRIVATE aowis-native-store)

add_executable(aowis-native-sync tools/native_sync.cpp)
target_link_libraries(aowis-native-sync PRIVATE aowis-native-store)

add_executable(aowis-native-verify tools/native_verify.cpp)
target_link_libraries(aowis-native-verify PRIVATE aowis-native-store)

add_executable(aowis-native-compact tools/native_compact.cpp)
target_link_libraries(aowis-native-compact PRIVATE aowis-native-store)

add_executable(aowis-native-benchmark benchmarks/native_benchmark.cpp)
target_link_libraries(aowis-native-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-merge-history-benchmark benchmarks/native_merge_history_benchmark.cpp)
target_link_libraries(aowis-native-merge-history-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-model-benchmark benchmarks/model_network_benchmark.cpp)
target_link_libraries(aowis-native-model-benchmark PRIVATE aowis-native-store)

if(BUILD_TESTING)
    # Compile the engine a second time exclusively for fault-injection tests.
    # Production targets never contain crash/failure injection hooks.
    add_library(aowis-native-store-fault-tests STATIC
        src/native_store.cpp
        src/model_record.cpp
        src/access_policy.cpp
        src/pack_experiment.cpp
        src/block_store_prototype.cpp
    )
    target_compile_features(aowis-native-store-fault-tests PUBLIC cxx_std_20)
    target_include_directories(aowis-native-store-fault-tests PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(aowis-native-store-fault-tests PUBLIC OpenSSL::Crypto)
    target_compile_definitions(aowis-native-store-fault-tests PRIVATE AOWIS_NATIVE_FAULT_TEST_BUILD)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(aowis-native-store-fault-tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
    add_executable(aowis-native-project-service-tests tests/project_service_tests.cpp)
    target_link_libraries(aowis-native-project-service-tests PRIVATE aowis-native-project-service)
    add_test(NAME aowis-native-project-service-tests COMMAND aowis-native-project-service-tests)
    set_tests_properties(aowis-native-project-service-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-recovery-tests tests/native_recovery_tests.cpp)
    target_link_libraries(aowis-native-recovery-tests PRIVATE aowis-native-store-fault-tests)
    add_test(NAME aowis-native-recovery-tests COMMAND aowis-native-recovery-tests)
    set_tests_properties(aowis-native-recovery-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-pack-experiment-tests tests/pack_experiment_tests.cpp)
    target_link_libraries(aowis-native-pack-experiment-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-pack-experiment-tests COMMAND aowis-native-pack-experiment-tests)
    set_tests_properties(aowis-native-pack-experiment-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-sync-cli-tests tests/native_sync_cli_tests.cpp)
    target_link_libraries(aowis-native-sync-cli-tests PRIVATE aowis-native-store)
    add_dependencies(aowis-native-sync-cli-tests aowis-native-sync)
    add_test(NAME aowis-native-sync-cli-tests COMMAND aowis-native-sync-cli-tests
        $<TARGET_FILE:aowis-native-sync>)
    set_tests_properties(aowis-native-sync-cli-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-access-tests tests/native_access_tests.cpp)
    target_link_libraries(aowis-native-access-tests PRIVATE aowis-native-store)
    add_dependencies(aowis-native-access-tests aowis-native-access)
    add_test(NAME aowis-native-access-tests COMMAND aowis-native-access-tests $<TARGET_FILE:aowis-native-access>)
    set_tests_properties(aowis-native-access-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-merkle-integrity-tests tests/native_merkle_integrity_tests.cpp)
    target_link_libraries(aowis-native-merkle-integrity-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-merkle-integrity-tests COMMAND aowis-native-merkle-integrity-tests)
    set_tests_properties(aowis-native-merkle-integrity-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-model-import-integrity-tests
        tests/native_model_import_integrity_tests.cpp)
    target_link_libraries(aowis-native-model-import-integrity-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-model-import-integrity-tests
        COMMAND aowis-native-model-import-integrity-tests)
    set_tests_properties(aowis-native-model-import-integrity-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-repository-verify-tests
        tests/native_repository_verify_tests.cpp)
    target_link_libraries(aowis-native-repository-verify-tests PRIVATE aowis-native-store)
    add_dependencies(aowis-native-repository-verify-tests aowis-native-verify)
    add_test(NAME aowis-native-repository-verify-tests
        COMMAND aowis-native-repository-verify-tests $<TARGET_FILE:aowis-native-verify>)
    set_tests_properties(aowis-native-repository-verify-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-bundle-limit-tests tests/native_bundle_limit_tests.cpp)
    target_link_libraries(aowis-native-bundle-limit-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-bundle-limit-tests COMMAND aowis-native-bundle-limit-tests)
    set_tests_properties(aowis-native-bundle-limit-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-bundle-disk-index-tests tests/native_bundle_disk_index_tests.cpp)
    target_link_libraries(aowis-native-bundle-disk-index-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-bundle-disk-index-tests COMMAND aowis-native-bundle-disk-index-tests)
    set_tests_properties(aowis-native-bundle-disk-index-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-sync-tests tests/native_sync_tests.cpp)
    target_link_libraries(aowis-native-sync-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-sync-tests COMMAND aowis-native-sync-tests)
    set_tests_properties(aowis-native-sync-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-merge-ancestry-tests tests/native_merge_ancestry_tests.cpp)
    target_link_libraries(aowis-native-merge-ancestry-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-merge-ancestry-tests COMMAND aowis-native-merge-ancestry-tests)
    set_tests_properties(aowis-native-merge-ancestry-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-store-tests tests/native_store_tests.cpp)
    target_link_libraries(aowis-native-store-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-store-tests COMMAND aowis-native-store-tests)
    set_tests_properties(aowis-native-store-tests PROPERTIES LABELS "aowis")
    add_custom_target(aowis-db-tests DEPENDS aowis-native-bundle-disk-index-tests aowis-native-merge-ancestry-tests aowis-native-bundle-limit-tests aowis-native-project-service-tests aowis-native-recovery-tests aowis-native-repository-verify-tests aowis-native-model-import-integrity-tests aowis-native-merkle-integrity-tests aowis-native-store-tests aowis-native-pack-experiment-tests aowis-native-sync-tests aowis-native-sync-cli-tests aowis-native-access-tests)
endif()

# Canonical AOWIS Model serialization is owned by this repository.
# It depends on AOWIS-SERVER-MODEL and Qt Core, never on the SQL database.
# The standalone byte/object store can still build without Qt.
option(AOWIS_NATIVE_WITH_MODEL_CODEC "Build native model serialization and hydraulic persistence" ON)
if(AOWIS_NATIVE_WITH_MODEL_CODEC)
    find_package(Qt6 REQUIRED COMPONENTS Core)
    if(Qt6Core_FOUND)
        set(AOWIS_NATIVE_MODEL_SOURCE_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL"
            CACHE PATH "Pinned AOWIS model submodule")
        if(NOT EXISTS "${AOWIS_NATIVE_MODEL_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "Missing Model submodule. Run ./git_submodule_init.sh")
        endif()
        if(NOT TARGET AOWIS::Model)
            add_subdirectory("${AOWIS_NATIVE_MODEL_SOURCE_DIR}"
                "${CMAKE_CURRENT_BINARY_DIR}/aowis-server-model")
        endif()
        # Check all current Model fields against the explicitly maintained canonical model
        # schema contract. A new field must never be silently ignored.
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_model_codec.py"
                "${AOWIS_NATIVE_MODEL_SOURCE_DIR}/include/aowis/model"
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/model_codec_fields.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/model_codec.cpp"
            RESULT_VARIABLE AOWIS_NATIVE_CODEC_SCHEMA_RESULT
            OUTPUT_VARIABLE AOWIS_NATIVE_CODEC_SCHEMA_OUTPUT
            ERROR_VARIABLE AOWIS_NATIVE_CODEC_SCHEMA_ERROR)
        if(NOT AOWIS_NATIVE_CODEC_SCHEMA_RESULT EQUAL 0)
            message(FATAL_ERROR "Native model serializer schema mismatch:\n${AOWIS_NATIVE_CODEC_SCHEMA_ERROR}")
        endif()
        message(STATUS "Native model serializer: ${AOWIS_NATIVE_CODEC_SCHEMA_OUTPUT}")
        add_library(aowis-native-model-codec STATIC
            src/model_codec.cpp
            src/model_validation.cpp)
        add_library(AOWIS::NativeModelCodec ALIAS aowis-native-model-codec)
        target_compile_features(aowis-native-model-codec PUBLIC cxx_std_20)
        target_include_directories(aowis-native-model-codec PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(aowis-native-model-codec PUBLIC
            AOWIS::Model Qt6::Core)

        add_library(aowis-native-hydraulic STATIC
            src/model_codec_adapter.cpp
            src/hydraulic_network_persistence.cpp)
        add_library(AOWIS::NativeHydraulic ALIAS aowis-native-hydraulic)
        target_compile_features(aowis-native-hydraulic PUBLIC cxx_std_20)
        target_include_directories(aowis-native-hydraulic PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(aowis-native-hydraulic PUBLIC
            aowis-native-store AOWIS::NativeModelCodec)
        add_executable(aowis-native-hydraulic-model-benchmark
            benchmarks/v5_model_benchmark.cpp)
        target_link_libraries(aowis-native-hydraulic-model-benchmark PRIVATE aowis-native-hydraulic)
        add_executable(aowis-native-hydraulic-edit-benchmark
            benchmarks/hydraulic_edit_benchmark.cpp)
        target_link_libraries(aowis-native-hydraulic-edit-benchmark PRIVATE aowis-native-hydraulic)
        if(BUILD_TESTING)
            add_executable(aowis-native-model-codec-tests tests/v5_codec_tests.cpp)
            target_link_libraries(aowis-native-model-codec-tests PRIVATE aowis-native-hydraulic)
            add_test(NAME aowis-native-model-codec-tests COMMAND aowis-native-model-codec-tests)
            set_tests_properties(aowis-native-model-codec-tests PROPERTIES LABELS "aowis")
            add_executable(aowis-native-hydraulic-network-tests
                tests/hydraulic_network_persistence_tests.cpp)
            target_link_libraries(aowis-native-hydraulic-network-tests PRIVATE aowis-native-hydraulic)
            add_test(NAME aowis-native-hydraulic-network-tests COMMAND aowis-native-hydraulic-network-tests)
            set_tests_properties(aowis-native-hydraulic-network-tests PROPERTIES LABELS "aowis")
            add_dependencies(aowis-db-tests aowis-native-model-codec-tests aowis-native-hydraulic-network-tests)
        endif()
        message(STATUS "Native model serializer and hydraulic persistence enabled")
    else()
        message(STATUS "Native model serialization disabled: Qt6 Core development package not found")
    endif()
else()
    message(STATUS "Native model serialization disabled by configuration")
endif()

# The HTTP transport optionally links the Qt-based hydraulic model adapter.
# A native-only build uses
# -DAOWIS_NATIVE_WITH_MODEL_CODEC=OFF (HTTP defaults to OFF as well).
option(AOWIS_NATIVE_WITH_HTTP_SERVER "Build the Qt HTTP/TCP multi-project server" ${AOWIS_NATIVE_WITH_MODEL_CODEC})
if(AOWIS_NATIVE_WITH_HTTP_SERVER)
    find_package(Qt6 REQUIRED COMPONENTS Core Network HttpServer)
    add_library(aowis-native-http STATIC src/db_http_routes.cpp)
    target_compile_features(aowis-native-http PUBLIC cxx_std_20)
    target_include_directories(aowis-native-http PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(aowis-native-http PUBLIC aowis-native-project-service Qt6::HttpServer Qt6::Network)
    if(TARGET aowis-native-hydraulic)
        target_link_libraries(aowis-native-http PUBLIC aowis-native-hydraulic)
        target_compile_definitions(aowis-native-http PUBLIC AOWIS_HTTP_WITH_HYDRAULIC=1)
    endif()

    add_executable(aowis-server-db tools/aowis_server_db.cpp)
    target_link_libraries(aowis-server-db PRIVATE aowis-native-http)
    if(BUILD_TESTING)
        add_executable(aowis-native-http-tests tests/native_http_tests.cpp)
        target_link_libraries(aowis-native-http-tests PRIVATE aowis-native-http)
        add_dependencies(aowis-native-http-tests aowis-server-db)
        add_test(NAME aowis-native-http-tests COMMAND aowis-native-http-tests
            $<TARGET_FILE:aowis-server-db>)
        set_tests_properties(aowis-native-http-tests PROPERTIES LABELS "aowis")
        add_dependencies(aowis-db-tests aowis-native-http-tests)
    endif()
    message(STATUS "Native multi-project HTTP server enabled")
else()
    message(STATUS "Native multi-project HTTP server disabled by configuration")
endif()
