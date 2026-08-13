if(NOT DEFINED COOK_SCENE OR NOT DEFINED INSPECT_SCENE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "Cook scene CLI test inputs are missing")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(SOURCE "${TEST_ROOT}/fixture.iridium.scene.json")
set(METADATA "${TEST_ROOT}/fixture.iridium.scene.json.iridium.meta")
set(DDC "${TEST_ROOT}/ddc")

file(WRITE "${SOURCE}" [=[
{"format":"iridium.scene","schemaVersion":1,"name":"CLI fixture","entities":[
 {"uuid":"019fb7d3-0300-7000-8000-000000000001","components":[
  {"id":"iridium.component.name","version":1,"data":{"value":"Root"}},
  {"id":"iridium.component.transform","version":1,"data":{}},
  {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}},
  {"id":"iridium.component.light","version":1,"data":{"type":1,"intensity":4}}
 ]}
]}
]=])
file(WRITE "${METADATA}" [=[
{
  "schemaVersion": 1,
  "assetGuid": "01890f4c-0000-7000-8000-000000000010",
  "assetType": "iridium.scene",
  "importer": { "id": "iridium.scene", "version": 1 },
  "settings": { "schemaVersion": 1, "values": {} },
  "subassets": [],
  "tags": []
}
]=])

execute_process(
    COMMAND "${COOK_SCENE}" --source "${SOURCE}" --metadata "${METADATA}"
        --ddc "${DDC}" --platform windows-x64 --profile release --quality high
    RESULT_VARIABLE COLD_RESULT
    OUTPUT_VARIABLE COLD_OUTPUT
    ERROR_VARIABLE COLD_ERROR)
if(NOT COLD_RESULT EQUAL 0)
    message(FATAL_ERROR "Cold scene cook failed: ${COLD_ERROR}\n${COLD_OUTPUT}")
endif()
string(JSON COLD_PREPARATION GET "${COLD_OUTPUT}" preparation)
string(JSON COLD_SOURCE_PARSE GET "${COLD_OUTPUT}" sourceParse)
string(JSON ARTIFACT_PATH GET "${COLD_OUTPUT}" artifactPath)
if(NOT COLD_PREPARATION STREQUAL "source-parse" OR
    NOT COLD_SOURCE_PARSE STREQUAL "ON")
    message(FATAL_ERROR "Cold scene cook did not parse and compile source")
endif()

execute_process(
    COMMAND "${COOK_SCENE}" --source "${SOURCE}" --metadata "${METADATA}"
        --ddc "${DDC}" --platform windows-x64 --profile release --quality high
    RESULT_VARIABLE WARM_RESULT
    OUTPUT_VARIABLE WARM_OUTPUT
    ERROR_VARIABLE WARM_ERROR)
if(NOT WARM_RESULT EQUAL 0)
    message(FATAL_ERROR "Warm scene cook failed: ${WARM_ERROR}\n${WARM_OUTPUT}")
endif()
string(JSON WARM_STATUS GET "${WARM_OUTPUT}" status)
string(JSON WARM_PREPARATION GET "${WARM_OUTPUT}" preparation)
string(JSON WARM_SOURCE_PARSE GET "${WARM_OUTPUT}" sourceParse)
string(JSON WARM_SCENE_COMPILE GET "${WARM_OUTPUT}" sceneCompile)
if(NOT WARM_STATUS STREQUAL "cache-hit" OR
    NOT WARM_PREPARATION STREQUAL "receipt-hit" OR
    NOT WARM_SOURCE_PARSE STREQUAL "OFF" OR
    NOT WARM_SCENE_COMPILE STREQUAL "OFF")
    message(FATAL_ERROR "Warm scene cook did not bypass source parse/compile")
endif()

execute_process(
    COMMAND "${INSPECT_SCENE}" "${ARTIFACT_PATH}"
    RESULT_VARIABLE INSPECT_RESULT
    OUTPUT_VARIABLE INSPECT_OUTPUT
    ERROR_VARIABLE INSPECT_ERROR)
if(NOT INSPECT_RESULT EQUAL 0)
    message(FATAL_ERROR "Cooked scene inspection failed: ${INSPECT_ERROR}")
endif()
string(JSON INSPECT_STATUS GET "${INSPECT_OUTPUT}" status)
string(JSON ENTITY_COUNT GET "${INSPECT_OUTPUT}" entityCount)
if(NOT INSPECT_STATUS STREQUAL "ok" OR NOT ENTITY_COUNT EQUAL 1)
    message(FATAL_ERROR "Cooked scene inspector returned unexpected data")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
