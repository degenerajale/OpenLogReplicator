#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include "builder/Builder.h"
#include "common/TopicMap.h"
#include "common/exception/ConfigurationException.h"

using namespace OpenLogReplicator;

static uint testsRun = 0;
static uint testsFailed = 0;

#define CHECK(condition) do { \
    ++testsRun; \
    if (!(condition)) { \
        ++testsFailed; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (false)

// Runs `expression` expecting ConfigurationException; returns the message, or fails the test suite
static std::string expectConfigurationException(const std::function<void()>& expression) {
    try {
        ++testsRun;
        expression();
    } catch (const ConfigurationException& ex) {
        return ex.msg;
    }
    ++testsFailed;
    fprintf(stderr, "FAIL %s:%d: expected ConfigurationException\n", __FILE__, __LINE__);
    return "";
}

static void testEmptyMap() {
    TopicMap topicMap;
    CHECK(topicMap.empty());
    CHECK(topicMap.idFor("HR", "EMPLOYEES") == TopicMap::DEFAULT_ID);
    CHECK(topicMap.names().empty());
}

static void testAssignmentAndLookup() {
    TopicMap topicMap;
    topicMap.setDefault("olr_default");
    topicMap.add("USER.TABLE", "user_table");
    topicMap.add("HR.EMPLOYEES", "hr_employees");

    CHECK(!topicMap.empty());
    CHECK(topicMap.names().size() == 3);
    CHECK(topicMap.names()[0] == "olr_default");
    CHECK(topicMap.names()[1] == "user_table");
    CHECK(topicMap.names()[2] == "hr_employees");

    CHECK(topicMap.idFor("USER", "TABLE") == 1);
    CHECK(topicMap.idFor("HR", "EMPLOYEES") == 2);
    // Misses fall back to the default topic
    CHECK(topicMap.idFor("HR", "DEPTS") == TopicMap::DEFAULT_ID);
    CHECK(topicMap.idFor("", "") == TopicMap::DEFAULT_ID);
}

static void testFanIn() {
    TopicMap topicMap;
    topicMap.setDefault("olr_default");
    topicMap.add("HR.EMPLOYEES", "hr_employees");
    topicMap.add("HR.DEPTS", "hr_employees");

    // Two keys, one topic, one id - so only one topic handle is created
    CHECK(topicMap.names().size() == 2);
    CHECK(topicMap.idFor("HR", "EMPLOYEES") == topicMap.idFor("HR", "DEPTS"));
    CHECK(topicMap.idFor("HR", "EMPLOYEES") != TopicMap::DEFAULT_ID);

    // Mapping to the default topic name reuses id 0
    TopicMap topicMap2;
    topicMap2.setDefault("olr_default");
    topicMap2.add("USER.TABLE", "olr_default");
    CHECK(topicMap2.names().size() == 1);
    CHECK(topicMap2.idFor("USER", "TABLE") == TopicMap::DEFAULT_ID);
}

static void testCaseFolding() {
    TopicMap topicMap;
    topicMap.setDefault("olr_default");
    topicMap.add("hr.Employees", "hr_employees");

    // A lower/mixed-case key routes the exact (quoted) name and, as an alias, the uppercase
    // name Oracle stores for unquoted identifiers
    CHECK(topicMap.idFor("hr", "Employees") != TopicMap::DEFAULT_ID);
    CHECK(topicMap.idFor("HR", "EMPLOYEES") == topicMap.idFor("hr", "Employees"));
    CHECK(topicMap.mapping().find("HR.EMPLOYEES") != topicMap.mapping().end());

    // Quoted identifiers with different case are distinct tables and route independently
    TopicMap topicMap2;
    topicMap2.setDefault("olr_default");
    topicMap2.add("hr.Employees", "quoted_topic");
    topicMap2.add("HR.EMPLOYEES", "upper_topic");
    CHECK(topicMap2.idFor("hr", "Employees") != topicMap2.idFor("HR", "EMPLOYEES"));
    CHECK(topicMap2.names()[topicMap2.idFor("HR", "EMPLOYEES")] == "upper_topic");
    CHECK(topicMap2.names()[topicMap2.idFor("hr", "Employees")] == "quoted_topic");

    // The explicit uppercase key wins regardless of the order it is given in
    TopicMap topicMap3;
    topicMap3.setDefault("olr_default");
    topicMap3.add("HR.EMPLOYEES", "upper_topic");
    topicMap3.add("hr.Employees", "quoted_topic");
    CHECK(topicMap3.names()[topicMap3.idFor("HR", "EMPLOYEES")] == "upper_topic");
    CHECK(topicMap3.names()[topicMap3.idFor("hr", "Employees")] == "quoted_topic");
}

static void testTopicNameValidation() {
    TopicMap topicMap;
    topicMap.setDefault("olr_default");

    std::string msg = expectConfigurationException([&topicMap]() { topicMap.add("HR.X", ""); });
    CHECK(msg.find("HR.X") != std::string::npos);

    msg = expectConfigurationException([&topicMap]() { topicMap.add("HR.X", std::string(TopicMap::TOPIC_NAME_MAX_LENGTH + 1, 'a')); });
    CHECK(msg.find("249") != std::string::npos);

    // Exactly 249 characters is valid
    topicMap.add("HR.OK", std::string(TopicMap::TOPIC_NAME_MAX_LENGTH, 'a'));

    msg = expectConfigurationException([&topicMap]() { topicMap.setDefault("bad name"); });
    CHECK(msg.find("bad name") != std::string::npos);

    msg = expectConfigurationException([&topicMap]() { topicMap.add("HR.X", "top!ic"); });
    CHECK(msg.find("top!ic") != std::string::npos);

    msg = expectConfigurationException([&topicMap]() { topicMap.add("HR.X", "."); });
    CHECK(msg.find("top!ic") == std::string::npos && msg.find("\".\"") != std::string::npos);

    msg = expectConfigurationException([&topicMap]() { topicMap.add("HR.X", ".."); });
    CHECK(msg.find("\"..\"") != std::string::npos);

    // Valid edge names: dots, underscores, dashes, digits are fine
    topicMap.add("HR.X", "a.b_c-d0");
}

static void testKeyValidation() {
    TopicMap topicMap;
    topicMap.setDefault("olr_default");

    // No dot
    std::string msg = expectConfigurationException([&topicMap]() { topicMap.add("NO_TABLE", "t"); });
    CHECK(msg.find("NO_TABLE") != std::string::npos);

    // Two dots
    msg = expectConfigurationException([&topicMap]() { topicMap.add("A.B.C", "t"); });
    CHECK(msg.find("A.B.C") != std::string::npos);

    // Empty halves
    msg = expectConfigurationException([&topicMap]() { topicMap.add(".TABLE", "t"); });
    CHECK(msg.find(".TABLE") != std::string::npos);
    msg = expectConfigurationException([&topicMap]() { topicMap.add("OWNER.", "t"); });
    CHECK(msg.find("OWNER.") != std::string::npos);

    // Special characters are usable in keys - it is a literal
    topicMap.add("SYS.OBJ$", "sys_obj");
    CHECK(topicMap.idFor("SYS", "OBJ$") != TopicMap::DEFAULT_ID);

    // Duplicate key with a different topic is rejected
    topicMap.add("HR.EMPLOYEES", "hr_employees");
    msg = expectConfigurationException([&topicMap]() { topicMap.add("HR.EMPLOYEES", "other_topic"); });
    CHECK(msg.find("HR.EMPLOYEES") != std::string::npos);

    // Duplicate key with the same topic is idempotent
    topicMap.add("HR.EMPLOYEES", "hr_employees");
    CHECK(topicMap.idFor("HR", "EMPLOYEES") != TopicMap::DEFAULT_ID);

    // Same default topic twice is fine, a different one is not
    topicMap.setDefault("olr_default");
    msg = expectConfigurationException([&topicMap]() { topicMap.setDefault("other_default"); });
    CHECK(msg.find("other_default") != std::string::npos);
}

// A DbTable is deleted and re-created (at a new address) on every schema rebuild;
// the id must depend on the table name only, never on the object
static void testSchemaRebuild() {
    TopicMap topicMap;
    topicMap.setDefault("olr_default");
    topicMap.add("USER.TABLE", "user_table");
    topicMap.add("HR.EMPLOYEES", "hr_employees");

    struct FakeTable {
        std::string owner;
        std::string name;
        uint16_t topicId{TopicMap::DEFAULT_ID};
    };

    auto buildSchema = [&topicMap]() {
        // Fresh objects each time, like Schema::purgeMetadata() + buildMaps()
        auto* table1 = new FakeTable{"USER", "TABLE"};
        auto* table2 = new FakeTable{"HR", "EMPLOYEES"};
        table1->topicId = topicMap.idFor(table1->owner, table1->name);
        table2->topicId = topicMap.idFor(table2->owner, table2->name);
        const uint16_t id1 = table1->topicId;
        const uint16_t id2 = table2->topicId;
        delete table1;
        delete table2;
        return std::pair<uint16_t, uint16_t>(id1, id2);
    };

    const auto first = buildSchema();
    CHECK(first.first == 1);
    CHECK(first.second == 2);

    // Rebuild must produce identical ids
    const auto second = buildSchema();
    CHECK(second.first == first.first);
    CHECK(second.second == first.second);

    // And a rebuild after the map grew (addTableToDict re-runs on live tables)
    topicMap.add("HR.DEPTS", "hr_employees");
    const auto third = buildSchema();
    CHECK(third.first == first.first);
    CHECK(third.second == first.second);
    CHECK(topicMap.idFor("HR", "DEPTS") == second.second);
}

// Regression for a null-table crash: with schemaless mode, system transactions or DDL
// for objects outside the filter, the builder receives a null table (Builder.cpp dispatches
// processInsert/processUpdate/processDelete/processDdl without requiring table != nullptr).
// Those messages must take the default topic, not dereference the pointer.
static void testTopicIdOf() {
    CHECK(Builder::topicIdOf(nullptr) == TopicMap::DEFAULT_ID);
}

int main() {
    testEmptyMap();
    testAssignmentAndLookup();
    testFanIn();
    testCaseFolding();
    testTopicNameValidation();
    testKeyValidation();
    testSchemaRebuild();
    testTopicIdOf();

    if (testsFailed > 0) {
        fprintf(stderr, "%u/%u tests failed\n", testsFailed, testsRun);
        return 1;
    }
    fprintf(stderr, "all %u tests passed\n", testsRun);
    return 0;
}
