#ifndef TOPIC_MAP_H_
#define TOPIC_MAP_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenLogReplicator {
    // Immutable after the config is parsed. Maps "OWNER.TABLE" to a small integer
    // topic id; id 0 is reserved for the writer's default topic.
    //
    // Threading: setDefault/add are called only during config parsing, before any
    // thread is spawned. idFor is called by the thread that builds the schema,
    // names() is read by the writer thread during initialize() - all after the
    // map is fully built, so no locking is needed.
    class TopicMap final {
    public:
        static constexpr uint16_t DEFAULT_ID{0};
        static constexpr size_t TOPIC_NAME_MAX_LENGTH{249};   // Kafka's real limit

        // Reserve id 0 for the writer's default `topic`. Call once, before add().
        void setDefault(std::string topicName);

        // ownerTable is "OWNER.TABLE" as written in the config; uppercased here.
        // Two keys may map to the same topic name - they will resolve to the same id.
        void add(const std::string& ownerTable, const std::string& topicName);
        // Schema-build path. Returns DEFAULT_ID for any table not in the map.
        [[nodiscard]] uint16_t idFor(const std::string& owner, const std::string& name) const;

        // Writer path, called once from WriterKafka::initialize().
        [[nodiscard]] const std::vector<std::string>& names() const {
            return topicNames;
        }

        [[nodiscard]] bool empty() const {
            return byTable.empty();
        }

        // For logging the full mapping at startup
        [[nodiscard]] const std::unordered_map<std::string, uint16_t>& mapping() const {
            return byTable;
        }

    protected:
        std::vector<std::string> topicNames;                // id -> name, index 0 = default
        std::unordered_map<std::string, uint16_t> byName;   // name -> id, for fan-in dedupe
        std::unordered_map<std::string, uint16_t> byTable;  // "OWNER.TABLE" -> id

        static void validateTopicName(const std::string& topicName, const std::string& entry);
    };
}

#endif
