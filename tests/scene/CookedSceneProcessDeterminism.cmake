if(NOT DEFINED COOKED_SCENE_TEST OR NOT DEFINED OUTPUT_DIRECTORY)
    message(FATAL_ERROR "Cooked scene process determinism inputs are missing")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
set(FIRST "${OUTPUT_DIRECTORY}/first.irartifact")
set(SECOND "${OUTPUT_DIRECTORY}/second.irartifact")

execute_process(
    COMMAND "${COOKED_SCENE_TEST}" --emit "${FIRST}"
    RESULT_VARIABLE FIRST_RESULT)
if(NOT FIRST_RESULT EQUAL 0)
    message(FATAL_ERROR "First independent cooked scene process failed: ${FIRST_RESULT}")
endif()

execute_process(
    COMMAND "${COOKED_SCENE_TEST}" --emit-reversed "${SECOND}"
    RESULT_VARIABLE SECOND_RESULT)
if(NOT SECOND_RESULT EQUAL 0)
    message(FATAL_ERROR "Second independent cooked scene process failed: ${SECOND_RESULT}")
endif()

file(SHA256 "${FIRST}" FIRST_HASH)
file(SHA256 "${SECOND}" SECOND_HASH)
if(NOT FIRST_HASH STREQUAL SECOND_HASH)
    message(FATAL_ERROR
        "Independent cooked scene processes were not byte-identical: ${FIRST_HASH} vs ${SECOND_HASH}")
endif()

file(REMOVE "${FIRST}" "${SECOND}")
