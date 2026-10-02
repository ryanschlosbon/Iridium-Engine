# Process test: cook a tracked engine-authored fixture model, then run the engine on
# a benchmark fixture with Vulkan validation and an explicit qualification oracle.
# Oracles are no longer implied by --validation (M7R R2.5), so this keeps the
# device/CPU parity checks exercised by ctest.
#
# Inputs: ENGINE, COOK, SOURCE_ROOT, WORK_DIR, MODEL (path under assets/),
#         BENCHMARK, MANIFEST (path under SOURCE_ROOT), ORACLE_ARGS (;-list)
foreach(var ENGINE COOK SOURCE_ROOT WORK_DIR MODEL BENCHMARK MANIFEST ORACLE_ARGS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "EngineOracleProcessTest requires -D${var}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(
    COMMAND "${COOK}" --source "${SOURCE_ROOT}/assets/${MODEL}" --ddc "${WORK_DIR}/ddc"
    OUTPUT_VARIABLE cook_output
    ERROR_VARIABLE cook_error
    RESULT_VARIABLE cook_result)
if(NOT cook_result EQUAL 0)
    message(FATAL_ERROR "Cook failed (${cook_result}): ${cook_error}")
endif()
string(JSON cook_key GET "${cook_output}" cookKey)
string(SUBSTRING "${cook_key}" 0 2 key_prefix)
string(SUBSTRING "${cook_key}" 2 -1 key_rest)
set(artifact "${WORK_DIR}/ddc/${key_prefix}/${key_rest}.irartifact")

execute_process(
    COMMAND "${ENGINE}"
        --benchmark "${BENCHMARK}"
        --benchmark-manifest "${SOURCE_ROOT}/${MANIFEST}"
        --cooked-model-artifact "${artifact}"
        --window-size 640x360 --hidden-window --borderless-window
        --warmup-frames 8 --frame-limit 24
        --validation ${ORACLE_ARGS}
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
    RESULT_VARIABLE run_result)
set(log "${run_output}\n${run_error}")
file(WRITE "${WORK_DIR}/engine.log" "${log}")
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Engine failed (${run_result}); see ${WORK_DIR}/engine.log")
endif()
string(FIND "${log}" "[Validation]:" validation_index)
if(NOT validation_index EQUAL -1)
    message(FATAL_ERROR "Vulkan validation reported messages; see ${WORK_DIR}/engine.log")
endif()
message(STATUS "Oracle run passed: ${BENCHMARK} ${ORACLE_ARGS}")
