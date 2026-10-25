cmake_minimum_required(VERSION 3.22)
project(AOWIS_Server_DB_Custom VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(aowis-native-store STATIC
    src/native_store.cpp
    src/model_record.cpp
    src/access_policy.cpp
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

add_executable(aowis-native-access tools/native_access.cpp)
target_link_libraries(aowis-native-access PRIVATE aowis-native-store)

add_executable(aowis-native-sync tools/native_sync.cpp)
target_link_libraries(aowis-native-sync PRIVATE aowis-native-store)

add_executable(aowis-native-verify tools/native_verify.cpp)
target_link_libraries(aowis-native-verify PRIVATE aowis-native-store)

add_executable(aowis-native-compact tools/native_compact.cpp)
target_link_libraries(aowis-native-compact PRIVATE aowis-native-store)

add_executable(aowis-native-index-scaling-benchmark benchmarks/native_index_scaling_benchmark.cpp)
target_link_libraries(aowis-native-index-scaling-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-benchmark benchmarks/native_benchmark.cpp)
target_link_libraries(aowis-native-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-merge-history-benchmark benchmarks/native_merge_history_benchmark.cpp)
target_link_libraries(aowis-native-merge-history-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-replication-scaling-benchmark
    benchmarks/native_replication_scaling_benchmark.cpp)
target_link_libraries(aowis-native-replication-scaling-benchmark PRIVATE aowis-native-store)

add_executable(aowis-native-model-benchmark benchmarks/model_network_benchmark.cpp)
target_link_libraries(aowis-native-model-benchmark PRIVATE aowis-native-store)

if(BUILD_TESTING)
    # Frozen pre-release bytes built by a previous native engine, not generated
    # by the executable under test. Reopen the historical pack and import its
    # full and incremental bundles before changing stored format encodings.
    add_executable(aowis-native-format-compatibility-tests
        tests/native_format_compatibility_tests.cpp)
    target_link_libraries(aowis-native-format-compatibility-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-format-compatibility-tests
        COMMAND aowis-native-format-compatibility-tests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pack-20261024")
    set_tests_properties(aowis-native-format-compatibility-tests PROPERTIES
        LABELS "aowis;compatibility" TIMEOUT 90)
    add_executable(aowis-native-transfer-staging-budget-tests
        tests/native_transfer_staging_budget_tests.cpp)
    target_include_directories(aowis-native-transfer-staging-budget-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(aowis-native-transfer-staging-budget-tests PRIVATE cxx_std_20)
    target_link_libraries(aowis-native-transfer-staging-budget-tests PRIVATE Threads::Threads)
    add_test(NAME aowis-native-transfer-staging-budget-tests
        COMMAND aowis-native-transfer-staging-budget-tests)
    set_tests_properties(aowis-native-transfer-staging-budget-tests PROPERTIES LABELS "aowis")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME aowis-native-http-resource-monitor-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_http_resources_tests.py")
        set_tests_properties(aowis-native-http-resource-monitor-tests PROPERTIES
            LABELS "aowis;http-stress;qualification" TIMEOUT 30)
        # Offline analysis of saved resource reports. This does not start a
        # server or assert a universal throughput/memory threshold.
        add_test(NAME aowis-native-http-resource-analysis-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_resource_run_analysis_tests.py")
        set_tests_properties(aowis-native-http-resource-analysis-tests PROPERTIES
            LABELS "aowis;http-stress;qualification" TIMEOUT 30)
    endif()
    add_test(NAME aowis-native-index-scaling-smoke
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_native_index.py"
            --binary "$<TARGET_FILE:aowis-native-index-scaling-benchmark>" --smoke)
    set_tests_properties(aowis-native-index-scaling-smoke PROPERTIES LABELS "aowis")
    add_test(NAME aowis-native-index-benchmark-build-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/index_benchmark_build_tests.py"
            --script "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_native_index.py")
    set_tests_properties(aowis-native-index-benchmark-build-tests PROPERTIES LABELS "aowis")
    # Packaging must include the working-tree files of every Git submodule,
    # including recursive submodules. The test creates disposable Git repos.
    find_program(AOWIS_TEST_GIT_EXECUTABLE git)
    find_program(AOWIS_TEST_ZIP_EXECUTABLE zip)
    if(AOWIS_TEST_GIT_EXECUTABLE AND AOWIS_TEST_ZIP_EXECUTABLE)
        add_test(NAME aowis-native-zip-submodule-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/zip_submodule_tests.py"
                --script "${CMAKE_CURRENT_SOURCE_DIR}/tools/zip_db.sh")
        set_tests_properties(aowis-native-zip-submodule-tests PROPERTIES LABELS "aowis")
    endif()
    add_test(NAME aowis-native-replication-benchmark-smoke
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_replication_scaling.py"
            --binary "$<TARGET_FILE:aowis-native-replication-scaling-benchmark>" --smoke)
    set_tests_properties(aowis-native-replication-benchmark-smoke PROPERTIES LABELS "aowis")
    add_test(NAME aowis-native-replication-benchmark-phases
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/replication_benchmark_phases_tests.py"
            --script "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark_replication_scaling.py"
            --binary "$<TARGET_FILE:aowis-native-replication-scaling-benchmark>")
    set_tests_properties(aowis-native-replication-benchmark-phases PROPERTIES LABELS "aowis")
    # Compile the engine a second time exclusively for fault-injection tests.
    # Production targets never contain crash/failure injection hooks.
    add_library(aowis-native-store-fault-tests STATIC
        src/native_store.cpp
        src/model_record.cpp
        src/access_policy.cpp
            )
    target_compile_features(aowis-native-store-fault-tests PUBLIC cxx_std_20)
    target_include_directories(aowis-native-store-fault-tests PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(aowis-native-store-fault-tests PUBLIC OpenSSL::Crypto)
    target_compile_definitions(aowis-native-store-fault-tests PRIVATE AOWIS_NATIVE_FAULT_TEST_BUILD)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(aowis-native-store-fault-tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
    # Docker qualification setup can be validated offline without Docker/QEMU.
    add_test(NAME aowis-native-vm-powercut-docker-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_powercut_docker_tests.py"
            --wrapper "${CMAKE_CURRENT_SOURCE_DIR}/qualify_vm_powercut_docker.sh"
            --helper "${CMAKE_CURRENT_SOURCE_DIR}/tools/vm_powercut_docker.py")
    set_tests_properties(aowis-native-vm-powercut-docker-tests PROPERTIES
        LABELS "aowis;durability" TIMEOUT 30)
    # Offline orchestration regression: a whole-VM kill may discard the worker
    # installed on the *guest OS* disk; recovery must restage it each boot.
    # Mocked QEMU operations never format, boot, or open any real disk.
    add_test(NAME aowis-native-vm-powercut-runner-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_powercut_runner_tests.py")
    set_tests_properties(aowis-native-vm-powercut-runner-tests PROPERTIES
        LABELS "aowis;durability" TIMEOUT 30)
    # Offline evidence checker for QEMU, CTest and HTTP-soak qualification
    # reports; cannot itself exercise a VM or a production filesystem.
    add_test(NAME aowis-native-qualification-evidence-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/qualification_evidence_tests.py")
    set_tests_properties(aowis-native-qualification-evidence-tests PROPERTIES
        LABELS "aowis;durability" TIMEOUT 30)
    # Standalone production-library worker for optional, externally orchestrated
    # QEMU power-cut qualification. Not registered in default CTest: it needs a
    # separately provisioned guest OS and disposable virtual disk.
    add_executable(aowis-native-vm-powercut-worker
        tests/native_vm_powercut_worker.cpp)
    target_link_libraries(aowis-native-vm-powercut-worker PRIVATE aowis-native-store)
    add_test(NAME aowis-native-vm-powercut-worker-smoke
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_powercut_worker_tests.py"
            --binary "$<TARGET_FILE:aowis-native-vm-powercut-worker>")
    set_tests_properties(aowis-native-vm-powercut-worker-smoke PROPERTIES
        LABELS "aowis;durability" TIMEOUT 90)

    # Abrupt SIGKILL of the production store in independent processes,
    # followed by fresh-process verification and continued writes. This is
    # not equivalent to removing power from the block device.
    add_executable(aowis-native-sigkill-restart-tests tests/native_sigkill_restart_tests.cpp)
    target_link_libraries(aowis-native-sigkill-restart-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-sigkill-restart-tests
        COMMAND aowis-native-sigkill-restart-tests)
    set_tests_properties(aowis-native-sigkill-restart-tests PROPERTIES
        LABELS "aowis;durability" TIMEOUT 120)
    add_executable(aowis-native-project-service-tests tests/project_service_tests.cpp)
    target_link_libraries(aowis-native-project-service-tests PRIVATE aowis-native-project-service)
    add_test(NAME aowis-native-project-service-tests COMMAND aowis-native-project-service-tests)
    set_tests_properties(aowis-native-project-service-tests PROPERTIES LABELS "aowis")
    find_package(Threads REQUIRED)
    # Full-service cold backup/restore drill, including independent repository locks.
    add_executable(aowis-native-offline-backup-tests tests/native_offline_backup_tests.cpp)
    target_link_libraries(aowis-native-offline-backup-tests PRIVATE aowis-native-project-service)
    add_dependencies(aowis-native-offline-backup-tests aowis-native-verify)
    add_test(NAME aowis-native-offline-backup-tests
        COMMAND aowis-native-offline-backup-tests
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/offline_backup.py"
            "$<TARGET_FILE:aowis-native-verify>")
    set_tests_properties(aowis-native-offline-backup-tests PROPERTIES LABELS "aowis")
    # Complete offline server-root move/upgrade: frozen earlier-build packs,
    # divergent branches, multi-megabyte current data and a second restore
    # after writing under the newer executable.
    add_executable(aowis-native-upgrade-restore-tests
        tests/native_upgrade_restore_tests.cpp)
    target_link_libraries(aowis-native-upgrade-restore-tests PRIVATE aowis-native-project-service)
    add_dependencies(aowis-native-upgrade-restore-tests aowis-native-verify)
    add_test(NAME aowis-native-upgrade-restore-tests
        COMMAND aowis-native-upgrade-restore-tests
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/offline_backup.py"
            "$<TARGET_FILE:aowis-native-verify>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pack-20261024")
    set_tests_properties(aowis-native-upgrade-restore-tests PROPERTIES
        LABELS "aowis;compatibility" TIMEOUT 180)
    add_executable(aowis-native-project-concurrency-tests tests/project_concurrency_tests.cpp)
    target_link_libraries(aowis-native-project-concurrency-tests PRIVATE aowis-native-project-service Threads::Threads)
    add_test(NAME aowis-native-project-concurrency-tests COMMAND aowis-native-project-concurrency-tests)
    set_tests_properties(aowis-native-project-concurrency-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-pack-operational-stress-tests
        tests/native_pack_operational_stress_tests.cpp)
    target_link_libraries(aowis-native-pack-operational-stress-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-pack-operational-stress-tests
        COMMAND aowis-native-pack-operational-stress-tests)
    set_tests_properties(aowis-native-pack-operational-stress-tests PROPERTIES LABELS "aowis")
    # True kernel ENOSPC on a disposable, 32-MiB private tmpfs. The launcher
    # skips cleanly on hosts without unprivileged user/mount namespaces.
    add_executable(aowis-native-physical-enospc-tests
        tests/native_physical_enospc_tests.cpp)
    target_link_libraries(aowis-native-physical-enospc-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-physical-enospc-tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/physical_enospc_mount.py"
            --binary "$<TARGET_FILE:aowis-native-physical-enospc-tests>")
    set_tests_properties(aowis-native-physical-enospc-tests PROPERTIES
        LABELS "aowis;physical-enospc" SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_executable(aowis-native-recovery-tests tests/native_recovery_tests.cpp)
    target_link_libraries(aowis-native-recovery-tests PRIVATE aowis-native-store-fault-tests)
    add_test(NAME aowis-native-recovery-tests COMMAND aowis-native-recovery-tests)
    set_tests_properties(aowis-native-recovery-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-scratch-tests tests/native_scratch_tests.cpp)
    target_link_libraries(aowis-native-scratch-tests PRIVATE aowis-native-store-fault-tests)
    add_test(NAME aowis-native-scratch-tests COMMAND aowis-native-scratch-tests)
    set_tests_properties(aowis-native-scratch-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-error-kind-tests tests/native_error_kind_tests.cpp)
    target_link_libraries(aowis-native-error-kind-tests PRIVATE aowis-native-store-fault-tests)
    add_test(NAME aowis-native-error-kind-tests COMMAND aowis-native-error-kind-tests)
    set_tests_properties(aowis-native-error-kind-tests PROPERTIES LABELS "aowis")
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
    add_executable(aowis-native-bundle-import-profile-tests
        tests/native_bundle_import_profile_tests.cpp)
    target_link_libraries(aowis-native-bundle-import-profile-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-bundle-import-profile-tests
        COMMAND aowis-native-bundle-import-profile-tests)
    set_tests_properties(aowis-native-bundle-import-profile-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-bundle-limit-tests tests/native_bundle_limit_tests.cpp)
    target_link_libraries(aowis-native-bundle-limit-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-bundle-limit-tests COMMAND aowis-native-bundle-limit-tests)
    set_tests_properties(aowis-native-bundle-limit-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-bundle-disk-index-tests tests/native_bundle_disk_index_tests.cpp)
    target_link_libraries(aowis-native-bundle-disk-index-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-bundle-disk-index-tests COMMAND aowis-native-bundle-disk-index-tests)
    set_tests_properties(aowis-native-bundle-disk-index-tests PROPERTIES LABELS "aowis")
    # Separate, cheap cross-kind trie-cache regression; also useful under
    # sanitizers without executing the long large-bundle stress fixture.
    add_test(NAME aowis-native-bundle-shared-trie-tests
        COMMAND aowis-native-bundle-disk-index-tests --shared-kinds-only)
    set_tests_properties(aowis-native-bundle-shared-trie-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-bundle-revision-marks-tests
        tests/native_bundle_revision_marks_tests.cpp)
    target_link_libraries(aowis-native-bundle-revision-marks-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-bundle-revision-marks-tests
        COMMAND aowis-native-bundle-revision-marks-tests)
    set_tests_properties(aowis-native-bundle-revision-marks-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-sync-tests tests/native_sync_tests.cpp)
    target_link_libraries(aowis-native-sync-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-sync-tests COMMAND aowis-native-sync-tests)
    set_tests_properties(aowis-native-sync-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-merge-ancestry-tests tests/native_merge_ancestry_tests.cpp)
    target_link_libraries(aowis-native-merge-ancestry-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-merge-ancestry-tests COMMAND aowis-native-merge-ancestry-tests)
    set_tests_properties(aowis-native-merge-ancestry-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-incremental-sync-tests tests/native_incremental_sync_tests.cpp)
    target_link_libraries(aowis-native-incremental-sync-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-incremental-sync-tests COMMAND aowis-native-incremental-sync-tests)
    set_tests_properties(aowis-native-incremental-sync-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-diff-scope-tests tests/native_diff_scope_tests.cpp)
    target_link_libraries(aowis-native-diff-scope-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-diff-scope-tests COMMAND aowis-native-diff-scope-tests)
    set_tests_properties(aowis-native-diff-scope-tests PROPERTIES LABELS "aowis")
    add_executable(aowis-native-store-tests tests/native_store_tests.cpp)
    target_link_libraries(aowis-native-store-tests PRIVATE aowis-native-store)
    add_test(NAME aowis-native-store-tests COMMAND aowis-native-store-tests)
    set_tests_properties(aowis-native-store-tests PROPERTIES LABELS "aowis")
    add_custom_target(aowis-db-tests DEPENDS aowis-native-vm-powercut-worker aowis-native-sigkill-restart-tests aowis-native-physical-enospc-tests aowis-native-offline-backup-tests aowis-native-transfer-staging-budget-tests aowis-native-index-scaling-benchmark aowis-native-pack-operational-stress-tests aowis-native-bundle-import-profile-tests aowis-native-replication-scaling-benchmark aowis-native-bundle-revision-marks-tests aowis-native-bundle-disk-index-tests aowis-native-merge-ancestry-tests aowis-native-bundle-limit-tests aowis-native-project-service-tests aowis-native-project-concurrency-tests aowis-native-recovery-tests aowis-native-scratch-tests aowis-native-error-kind-tests aowis-native-repository-verify-tests aowis-native-model-import-integrity-tests aowis-native-merkle-integrity-tests aowis-native-store-tests aowis-native-diff-scope-tests aowis-native-incremental-sync-tests aowis-native-sync-tests aowis-native-sync-cli-tests aowis-native-access-tests)
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
        # The same check requires a value rule for every numeric field and
        # generates the declared enumerators the codec accepts. Changing a
        # Model header, the codec or the rules re-runs it.
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(AOWIS_NATIVE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
        file(GLOB_RECURSE AOWIS_NATIVE_MODEL_HEADERS
            "${AOWIS_NATIVE_MODEL_SOURCE_DIR}/include/aowis/model/*.h")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${AOWIS_NATIVE_MODEL_HEADERS}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_model_codec.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/model_codec_fields.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/model_codec.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/model_value_validation.cpp")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_model_codec.py"
                "${AOWIS_NATIVE_MODEL_SOURCE_DIR}/include/aowis/model"
                "${CMAKE_CURRENT_SOURCE_DIR}/docs/model_codec_fields.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/model_codec.cpp"
                --value-rules "${CMAKE_CURRENT_SOURCE_DIR}/src/model_value_validation.cpp"
                --emit-enums "${AOWIS_NATIVE_GENERATED_DIR}/model_enum_values.inc"
            RESULT_VARIABLE AOWIS_NATIVE_CODEC_SCHEMA_RESULT
            OUTPUT_VARIABLE AOWIS_NATIVE_CODEC_SCHEMA_OUTPUT
            ERROR_VARIABLE AOWIS_NATIVE_CODEC_SCHEMA_ERROR)
        if(NOT AOWIS_NATIVE_CODEC_SCHEMA_RESULT EQUAL 0)
            message(FATAL_ERROR "Native model serializer schema mismatch:\n${AOWIS_NATIVE_CODEC_SCHEMA_ERROR}")
        endif()
        message(STATUS "Native model serializer: ${AOWIS_NATIVE_CODEC_SCHEMA_OUTPUT}")
        add_library(aowis-native-model-codec STATIC
            src/model_codec.cpp
            src/model_validation.cpp
            src/model_value_validation.cpp)
        target_include_directories(aowis-native-model-codec PRIVATE "${AOWIS_NATIVE_GENERATED_DIR}")
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
            add_dependencies(aowis-db-tests aowis-native-hydraulic-edit-benchmark)
        endif()
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
            add_executable(aowis-native-model-value-rules-tests tests/model_value_rules_tests.cpp)
            target_link_libraries(aowis-native-model-value-rules-tests PRIVATE aowis-native-hydraulic)
            target_include_directories(aowis-native-model-value-rules-tests PRIVATE "${AOWIS_NATIVE_GENERATED_DIR}")
            add_test(NAME aowis-native-model-value-rules-tests COMMAND aowis-native-model-value-rules-tests)
            set_tests_properties(aowis-native-model-value-rules-tests PROPERTIES LABELS "aowis")
            add_test(NAME aowis-native-model-codec-check-tests
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_model_codec_tests.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}")
            set_tests_properties(aowis-native-model-codec-check-tests PROPERTIES LABELS "aowis")
            add_dependencies(aowis-db-tests aowis-native-model-codec-tests aowis-native-hydraulic-network-tests
                aowis-native-model-value-rules-tests)
        endif()
        message(STATUS "Native model serializer and hydraulic persistence enabled")
    else()
        message(STATUS "Native model serialization disabled: Qt6 Core development package not found")
    endif()
else()
    message(STATUS "Native model serialization disabled by configuration")
endif()

# HTTP transport: vendored cpp-httplib (external/cpp-httplib, pinned single
# header) on a worker thread pool. Qt Core is used for JSON/CLI/INI and, when
# enabled, the hydraulic model adapter. Qt Network/HttpServer are not used.
# A native-only build uses -DAOWIS_NATIVE_WITH_MODEL_CODEC=OFF (HTTP defaults
# to OFF as well).
option(AOWIS_NATIVE_WITH_HTTP_SERVER "Build the HTTP/TCP multi-project server" ${AOWIS_NATIVE_WITH_MODEL_CODEC})
if(AOWIS_NATIVE_WITH_HTTP_SERVER)
    find_package(Qt6 REQUIRED COMPONENTS Core)
    find_package(Threads REQUIRED)
    add_library(aowis-cpp-httplib INTERFACE)
    target_include_directories(aowis-cpp-httplib SYSTEM INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/external/cpp-httplib")
    target_link_libraries(aowis-cpp-httplib INTERFACE Threads::Threads)
    add_library(aowis-native-http STATIC src/db_http_routes.cpp)
    target_compile_features(aowis-native-http PUBLIC cxx_std_20)
    target_include_directories(aowis-native-http PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(aowis-native-http PUBLIC aowis-native-project-service aowis-cpp-httplib Qt6::Core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(aowis-native-http PRIVATE -Wall -Wextra)
    endif()
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
        # Separate real-process transfer/admission stress: independent of Qt
        # test helpers, but runs against the production HTTP executable.
        add_executable(aowis-native-http-transfer-stress-tests
            tests/native_http_transfer_stress_tests.cpp)
        target_link_libraries(aowis-native-http-transfer-stress-tests PRIVATE
            aowis-native-store aowis-cpp-httplib)
        add_dependencies(aowis-native-http-transfer-stress-tests aowis-server-db)
        add_test(NAME aowis-native-http-stress-fixture-tests
            COMMAND aowis-native-http-transfer-stress-tests --fixture-only)
        set_tests_properties(aowis-native-http-stress-fixture-tests PROPERTIES
            LABELS "aowis;http-stress" TIMEOUT 45)
        add_test(NAME aowis-native-http-transfer-stress-tests
            COMMAND aowis-native-http-transfer-stress-tests $<TARGET_FILE:aowis-server-db>)
        set_tests_properties(aowis-native-http-transfer-stress-tests PROPERTIES
            LABELS "aowis;http-stress" TIMEOUT 180 RUN_SERIAL TRUE)
        # Short end-to-end soak guarantees elapsed-time checkpoints really
        # appear, rather than validating only synthetic progress-log fixtures.
        add_test(NAME aowis-native-http-soak-progress-tests
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_soak_progress_tests.py"
                "$<TARGET_FILE:aowis-native-http-transfer-stress-tests>"
                "$<TARGET_FILE:aowis-server-db>")
        set_tests_properties(aowis-native-http-soak-progress-tests PROPERTIES
            LABELS "aowis;http-stress;qualification" TIMEOUT 100 RUN_SERIAL TRUE)
        add_dependencies(aowis-db-tests aowis-native-http-transfer-stress-tests)
    endif()
    message(STATUS "Native multi-project HTTP server enabled (cpp-httplib)")
else()
    message(STATUS "Native multi-project HTTP server disabled by configuration")
endif()

# Keep the aggregate test build in sync with registered CTest executables.
# compile_linux_tests.sh builds this target before invoking CTest; an omitted
# dependency would otherwise leave a registered test as "Not Run". The explicit
# dependencies above still cover helper executables invoked by script tests.
if(BUILD_TESTING)
    get_property(AOWIS_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    foreach(AOWIS_REGISTERED_TEST IN LISTS AOWIS_REGISTERED_TESTS)
        if(TARGET "${AOWIS_REGISTERED_TEST}")
            get_target_property(AOWIS_TEST_TARGET_TYPE "${AOWIS_REGISTERED_TEST}" TYPE)
            if(AOWIS_TEST_TARGET_TYPE STREQUAL "EXECUTABLE")
                add_dependencies(aowis-db-tests "${AOWIS_REGISTERED_TEST}")
            endif()
        endif()
    endforeach()
endif()
