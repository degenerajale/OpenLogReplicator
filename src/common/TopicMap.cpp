#include "TopicMap.h"
#include <cstdint>
#include "exception/ConfigurationException.h"

namespace OpenLogReplicator {
    void TopicMap::setDefault(std::string topicName) {
        validateTopicName(topicName, "\"topic\"");

        if (!topicNames.empty()) {
            if (topicNames[DEFAULT_ID] != topicName)
                throw ConfigurationException(30001, "bad JSON, invalid \"topic\" value: \"" + topicName + "\", expected: \"" +
                                             topicNames[DEFAULT_ID] + "\" (already defined for this source)");
            return;
        }

        byName.emplace(topicName, DEFAULT_ID);
        topicNames.push_back(std::move(topicName));
    }

    void TopicMap::add(const std::string& ownerTable, const std::string& topicName) {
        const std::string::size_type pos = ownerTable.find('.');
        if (pos == std::string::npos || ownerTable.find('.', pos + 1) != std::string::npos)
            throw ConfigurationException(30001, "bad JSON, invalid \"topics\" key: \"" + ownerTable +
                                         "\", expected: format \"OWNER.TABLE\"");

        std::string owner = ownerTable.substr(0, pos);
        std::string table = ownerTable.substr(pos + 1);
        if (owner.empty() || table.empty())
            throw ConfigurationException(30001, "bad JSON, invalid \"topics\" key: \"" + ownerTable +
                                         "\", expected: format \"OWNER.TABLE\" with two non-empty parts");

        validateTopicName(topicName, "\"topics\" value for key \"" + ownerTable + "\"");

        // The key is matched against the dictionary names exactly, so a quoted identifier
        // ("hr"."Employees") is written with its real case. Unquoted identifiers are stored
        // uppercase by Oracle, so an uppercase alias is registered as well for keys written in
        // lower/mixed case; an explicit key always wins over an alias.
        std::string key = owner + "." + table;
        std::string upperKey = key;
        for (char& character: upperKey)
            if (character >= 'a' && character <= 'z')
                character = static_cast<char>(character - 'a' + 'A');

        // Reuse the id when the topic name was already seen (fan-in, or the default topic)
        uint16_t id;
        const auto& nameIt = byName.find(topicName);
        if (nameIt != byName.end()) {
            id = nameIt->second;
        } else {
            if (topicNames.size() >= UINT16_MAX)
                throw ConfigurationException(30001, "bad JSON, invalid \"topics\" value: too many topics, expected: max " +
                                             std::to_string(UINT16_MAX) + " topics");
            id = static_cast<uint16_t>(topicNames.size());
            topicNames.push_back(topicName);
            byName.emplace(topicName, id);
        }

        const auto& tableIt = byTable.find(key);
        if (tableIt != byTable.end()) {
            if (tableIt->second != id && (aliasKeys.find(key) == aliasKeys.end()))
                throw ConfigurationException(30001, "bad JSON, invalid \"topics\" key: \"" + key +
                                             "\", expected: not defined multiple times with different topics");
            // An explicit key replaces an alias registered earlier for the same name
            byTable[key] = id;
            aliasKeys.erase(key);
        } else
            byTable.emplace(key, id);

        if (upperKey != key && byTable.find(upperKey) == byTable.end()) {
            byTable.emplace(upperKey, id);
            aliasKeys.insert(upperKey);
        }
    }

    uint16_t TopicMap::idFor(const std::string& owner, const std::string& name) const {
        if (byTable.empty())
            return DEFAULT_ID;

        const auto& it = byTable.find(owner + "." + name);
        if (it == byTable.end())
            return DEFAULT_ID;

        return it->second;
    }

    void TopicMap::validateTopicName(const std::string& topicName, const std::string& entry) {
        if (topicName.empty())
            throw ConfigurationException(30001, "bad JSON, invalid " + entry + ": empty string, expected: non-empty topic name");

        if (topicName.length() > TOPIC_NAME_MAX_LENGTH)
            throw ConfigurationException(30001, "bad JSON, invalid " + entry + ": \"" + topicName + "\", length: " +
                                         std::to_string(topicName.length()) + ", expected: max " +
                                         std::to_string(TOPIC_NAME_MAX_LENGTH) + " characters");

        if (topicName == "." || topicName == "..")
            throw ConfigurationException(30001, "bad JSON, invalid " + entry + ": \"" + topicName + "\", expected: not \".\" or \"..\"");

        for (const char character: topicName) {
            if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                    character == '.' || character == '_' || character == '-')
                continue;
            throw ConfigurationException(30001, "bad JSON, invalid " + entry + ": \"" + topicName + "\", character: '" +
                                         std::string(1, character) + "', expected: one of {[A-Z], [a-z], [0-9], '.', '_', '-'}");
        }
    }
}
