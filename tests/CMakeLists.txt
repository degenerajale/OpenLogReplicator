add_executable(test_topic_map
        test_topic_map.cpp
        ../src/common/TopicMap.cpp
        ../src/common/exception/ConfigurationException.cpp)
# Ctx.h (included by builder/Builder.h) needs rapidjson; tests/ is wired before the
# top-level include_directories, so the path is added here explicitly
target_include_directories(test_topic_map SYSTEM PRIVATE ${WITH_RAPIDJSON}/include)
target_include_directories(test_topic_map PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(test_topic_map PRIVATE CTXASSERT=0)

add_test(NAME test_topic_map COMMAND test_topic_map)

find_program(BASH_PROGRAM bash)
if (BASH_PROGRAM)
    add_test(NAME test_config_parse
            COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/test_config_parse.sh
            $<TARGET_FILE:OpenLogReplicator> "${WITH_OCI}")
endif ()
