cmake_minimum_required(VERSION 3.22)
project(AOWIS_Server_DB_Custom VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(aowis-native-store STATIC
    src/native_store.cpp
    src/model_record.cpp
    src/pack_experiment.cpp
    src/block_store_prototype.cpp
)
target_compile_features(aowis-native-store PUBLIC cxx_std_20)
target_include_directories(aowis-native-store PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aowis-native-store PUBLIC OpenSSL::Crypto)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aowis-native-store PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(aowis-native-pack-experiment tools/native_pack_experiment.cpp)
target_link_libraries(aowis-native-pack-experiment PRIVATE aowis-native-store)

add_executable(aowis-native-block-store-benchmark tools/native_block_store_benchmark.cpp)
target_link_libraries(aowis-native-block-store-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-verify tools/native_verify.cpp)
target_link_libraries(aowis-native-verify PRIVATE aowis-native-store)

add_executable(aowis-native-compact tools/native_compact.cpp)
target_link_libraries(aowis-native-compact PRIVATE aowis-native-store)

add_executable(aowis-native-benchmark benchmarks/native_benchmark.cpp)
target_link_libraries(aowis-native-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-model-benchmark benchmarks/model_network_benchmark.cpp)
target_link_libraries(aowis-native-model-benchmark PRIVATE aowis-native-store)

if(BUILD_TESTING)
    add_executable(aowis-native-pack-experiment-tests tests/pack_experiment_tests.cpp)
    target_link_libraries(aowis-native-pack-experiment-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-pack-experiment-tests COMMAND aowis-native-pack-experiment-tests)
    set_tests_properties(aowis-native-pack-experiment-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-store-tests tests/native_store_tests.cpp)
    target_link_libraries(aowis-native-store-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-store-tests COMMAND aowis-native-store-tests)
    set_tests_properties(aowis-native-store-tests PROPERTIES LABELS "aowis")
    add_custom_target(aowis-db-tests DEPENDS aowis-native-store-tests aowis-native-pack-experiment-tests)
endif()

# Canonical AOWIS Model serialization is owned by this repository.
# It depends on AOWIS-SERVER-MODEL and Qt Core, never on the SQL database.
# The standalone byte/object store can still build without Qt.
option(AOWIS_NATIVE_WITH_MODEL_CODEC "Build the native-owned V5 model codec and persistence API" ON)
if(AOWIS_NATIVE_WITH_MODEL_CODEC)
    find_package(Qt6 QUIET COMPONENTS Core)
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
        # Check all current Model fields against the explicitly maintained V5
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
            message(FATAL_ERROR "Native Model V5 codec schema mismatch:\n${AOWIS_NATIVE_CODEC_SCHEMA_ERROR}")
        endif()
        message(STATUS "Native V5 codec: ${AOWIS_NATIVE_CODEC_SCHEMA_OUTPUT}")
        add_library(aowis-native-model-codec STATIC
            src/model_codec.cpp
            src/model_validation.cpp)
        add_library(AOWIS::NativeModelCodec ALIAS aowis-native-model-codec)
        target_compile_features(aowis-native-model-codec PUBLIC cxx_std_20)
        target_include_directories(aowis-native-model-codec PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(aowis-native-model-codec PUBLIC
            AOWIS::Model Qt6::Core)

        add_library(aowis-native-v5-codec STATIC
            src/model_codec_adapter.cpp
            src/hydraulic_network_persistence.cpp)
        target_compile_features(aowis-native-v5-codec PUBLIC cxx_std_20)
        target_include_directories(aowis-native-v5-codec PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(aowis-native-v5-codec PUBLIC
            aowis-native-store AOWIS::NativeModelCodec)
        add_executable(aowis-native-v5-model-benchmark
            benchmarks/v5_model_benchmark.cpp)
        target_link_libraries(aowis-native-v5-model-benchmark PRIVATE aowis-native-v5-codec)
        add_executable(aowis-native-hydraulic-edit-benchmark
            benchmarks/hydraulic_edit_benchmark.cpp)
        target_link_libraries(aowis-native-hydraulic-edit-benchmark PRIVATE aowis-native-v5-codec)
        if(BUILD_TESTING)
            add_executable(aowis-native-v5-codec-tests tests/v5_codec_tests.cpp)
            target_link_libraries(aowis-native-v5-codec-tests PRIVATE aowis-native-v5-codec)
            add_test(NAME aowis-native-v5-codec-tests COMMAND aowis-native-v5-codec-tests)
            set_tests_properties(aowis-native-v5-codec-tests PROPERTIES LABELS "aowis")
            add_executable(aowis-native-hydraulic-network-tests
                tests/hydraulic_network_persistence_tests.cpp)
            target_link_libraries(aowis-native-hydraulic-network-tests PRIVATE aowis-native-v5-codec)
            add_test(NAME aowis-native-hydraulic-network-tests COMMAND aowis-native-hydraulic-network-tests)
            set_tests_properties(aowis-native-hydraulic-network-tests PROPERTIES LABELS "aowis")
            add_dependencies(aowis-db-tests aowis-native-v5-codec-tests aowis-native-hydraulic-network-tests)
        endif()
        message(STATUS "Native-owned canonical V5 model codec enabled (no SQL dependency)")
    else()
        message(STATUS "Native-owned V5 codec disabled: Qt6 Core development package not found")
    endif()
else()
    message(STATUS "Native-owned V5 codec disabled by configuration")
endif()
