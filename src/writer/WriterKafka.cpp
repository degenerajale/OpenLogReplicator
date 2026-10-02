/* Thread writing directly to Kafka stream
   Copyright (C) 2018-2026 Adam Leszczynski (aleszczynski@bersler.com)

This file is part of OpenLogReplicator.

This program is free software: you can redistribute it and/or
modify it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public
License along with this program; see the file LICENSE;
If not, see <http://www.gnu.org/licenses/>. */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>
#include "../builder/Builder.h"
#include "../common/TopicMap.h"
#include "../common/exception/ConfigurationException.h"
#include "../common/exception/RuntimeException.h"
#include "../metadata/Metadata.h"
#include "WriterKafka.h"

namespace OpenLogReplicator {
    WriterKafka::WriterKafka(Ctx* newCtx, std::string newAlias, std::string newDatabase, Builder* newBuilder, Metadata* newMetadata, std::string newTopic,
                             const TopicMap* newTopicMap):
            Writer(newCtx, std::move(newAlias), std::move(newDatabase), newBuilder, newMetadata),
            topic(std::move(newTopic)),
            topicMap(newTopicMap) {
        errStr[0] = 0;
    }

    WriterKafka::~WriterKafka() {
        if (conf != nullptr)
            rd_kafka_conf_destroy(conf);

        for (rd_kafka_topic_t* rkt: rkts)
            if (rkt != nullptr)
                rd_kafka_topic_destroy(rkt);

        const rd_kafka_resp_err_t err = rd_kafka_fatal_error(rk, nullptr, 0);
        if (rk != nullptr)
            rd_kafka_destroy(rk);

        ctx->info(0, "Kafka producer exit code: " + std::to_string(err));
    }

    void WriterKafka::addProperty(std::string key, std::string value) {
        if (properties.find(key) != properties.end())
            throw ConfigurationException(30009, "Kafka property '" + key + "' is defined multiple times");
        properties.insert_or_assign(std::move(key), std::move(value));
    }

    void WriterKafka::initialize() {
        Writer::initialize();

        if (properties.find("message.max.bytes") != properties.end())
            throw ConfigurationException(30010, "Kafka property 'message.max.bytes' is defined, but it is not allowed to be set by user");

        conf = rd_kafka_conf_new();
        if (conf == nullptr)
            throw RuntimeException(10058, "Kafka failed to create configuration");

        const std::string maxMessageMbStr(std::to_string(builder->getMaxMessageMb() * 1024 * 1024));
        properties.insert_or_assign("message.max.bytes", maxMessageMbStr);

        if (properties.find("client.id") != properties.end())
            properties.insert_or_assign("client.id", "OpenLogReplicator");

        if (properties.find("group.id") != properties.end())
            properties.insert_or_assign("group.id", "OpenLogReplicator");

        for (const auto& [name, value]: properties)
            if (rd_kafka_conf_set(conf, name.c_str(), value.c_str(), errStr, sizeof(errStr)) != RD_KAFKA_CONF_OK)
                throw RuntimeException(10059, "Kafka message: " + std::string(errStr));

        // librdkafka waits this long before it treats a topic the broker does not report as missing
        // (topic creation takes a while to reach every broker); the startup check waits the same
        char propagationStr[32];
        size_t propagationSize = sizeof(propagationStr);
        int64_t propagationMs = 30000;
        if (rd_kafka_conf_get(conf, "topic.metadata.propagation.max.ms", propagationStr, &propagationSize) == RD_KAFKA_CONF_OK)
            propagationMs = std::strtoll(propagationStr, nullptr, 10);

        rd_kafka_conf_set_opaque(conf, this);
        rd_kafka_conf_set_dr_msg_cb(conf, dr_msg_cb);
        rd_kafka_conf_set_error_cb(conf, error_cb);
        rd_kafka_conf_set_log_cb(conf, logger_cb);

        rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errStr, sizeof(errStr));
        if (rk == nullptr)
            throw RuntimeException(10060, "Kafka failed to create producer, message: " + std::string(errStr));
        conf = nullptr;

        // One handle per entry in the topic map, in id order; index 0 is the default topic
        for (const std::string& topicName: topicMap->names()) {
            rd_kafka_topic_t* rkt = rd_kafka_topic_new(rk, topicName.c_str(), nullptr);
            if (rkt == nullptr)
                throw RuntimeException(10073, "Kafka failed to create topic \"" + topicName + "\", message: " + std::string(errStr));
            rkts.push_back(rkt);
        }

        // Check that every topic exists: a misspelled or missing topic would otherwise only show up
        // when its first message fails, which for a rarely changed table can be days later. One
        // metadata request covers all topics (the handles above make them locally known). A topic
        // that is still being created or propagated is reported unknown for a while, so an unknown
        // topic is asked about again until topic.metadata.propagation.max.ms has passed; only the
        // answer to the last request counts. Other topic errors (authorization) do not go away by
        // waiting and are reported at once. A broker that does not answer is not fatal: delivery
        // failures stop replication later anyway.
        const std::vector<std::string>& topicNames = topicMap->names();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(propagationMs);
        std::map<std::string, std::pair<rd_kafka_resp_err_t, int>> topicState;
        const auto unknownTopic = [&topicState](const std::string& topicName) {
            const auto it = topicState.find(topicName);
            return it == topicState.end() || it->second.first == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART ||
                    it->second.first == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;
        };
        bool answered = false;
        for (;;) {
            const rd_kafka_metadata_t* topicMetadata = nullptr;
            rd_kafka_resp_err_t err;
            const auto requestDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(METADATA_TIMEOUT_MS);
            bool sent = false;
            for (;;) {
                // While no broker connection is up, the call returns _TRANSPORT at the end of its
                // wait and the connection attempts go on, so it is called in steps. Once a request
                // went out unanswered (_TIMED_OUT) the broker is slow, not absent: each call sends
                // a new request, so the next one waits for the rest of the time
                const int64_t remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        requestDeadline - std::chrono::steady_clock::now()).count();
                err = rd_kafka_metadata(rk, 0, nullptr, &topicMetadata,
                                        static_cast<int>(std::clamp<int64_t>(remainingMs, 1, sent ? METADATA_TIMEOUT_MS : METADATA_STEP_MS)));
                if (err == RD_KAFKA_RESP_ERR__TIMED_OUT)
                    sent = true;
                if ((err != RD_KAFKA_RESP_ERR__TIMED_OUT && err != RD_KAFKA_RESP_ERR__TRANSPORT) || ctx->softShutdown ||
                    std::chrono::steady_clock::now() >= requestDeadline)
                    break;
            }
            if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
                // An earlier answer is not used: the topics it missed may exist by now
                answered = false;
                if (!ctx->softShutdown)
                    ctx->warning(60038, "Kafka: could not check the topics: " + std::string(rd_kafka_err2str(err)));
                break;
            }
            answered = true;
            topicState.clear();
            for (int i = 0; i < topicMetadata->topic_cnt; ++i)
                topicState[topicMetadata->topics[i].topic] = {topicMetadata->topics[i].err, topicMetadata->topics[i].partition_cnt};
            rd_kafka_metadata_destroy(topicMetadata);

            const auto now = std::chrono::steady_clock::now();
            if (std::none_of(topicNames.begin(), topicNames.end(), unknownTopic) || now >= deadline || ctx->softShutdown)
                break;
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(std::chrono::milliseconds(500), deadline - now));
        }

        // Stopped (Ctrl-C) while waiting for a topic: nothing to report
        if (answered && !ctx->softShutdown) {
            std::string missing;
            for (const std::string& topicName: topicNames) {
                if (unknownTopic(topicName)) {
                    missing += (missing.empty() ? "\"" : ", \"") + topicName + "\"";
                    continue;
                }
                const auto& [topicErr, partitions] = topicState.find(topicName)->second;
                if (topicErr != RD_KAFKA_RESP_ERR_NO_ERROR)
                    ctx->warning(60038, "Kafka: could not check topic \"" + topicName + "\": " + rd_kafka_err2str(topicErr));
                else
                    ctx->info(0, "Kafka topic \"" + topicName + "\": " + std::to_string(partitions) + " partition" + (partitions == 1 ? "" : "s"));
            }
            if (!missing.empty())
                throw RuntimeException(10075, "Kafka topic " + missing + " does not exist, create it before starting (or enable "
                                       "auto.create.topics.enable on the broker)");
        }
        streaming = true;
    }

    void WriterKafka::dr_msg_cb(rd_kafka_t * rkCb __attribute__((unused)), const rd_kafka_message_t * rkMessage, void*opaque __attribute__((unused))) {
        auto* msg = static_cast<BuilderMsg*>(rkMessage->_private);
        auto* writer = static_cast<WriterKafka*>(opaque);
        if (rkMessage->err != 0) {
            // librdkafka has retried until message.timeout.ms: the message will never reach the
            // topic. Continuing would leave a gap in the output, and the unconfirmed message would
            // stop the checkpoint and eventually the queue, so stop with an error instead.
            if (!writer->ctx->hardShutdown) {
                writer->ctx->error(10076, "Kafka: message " + std::to_string(msg->id) + " to topic " + rd_kafka_topic_name(rkMessage->rkt) +
                                   " was not delivered: " + rd_kafka_err2str(rkMessage->err) + ", stopping replication");
                writer->ctx->stopHard();
            }
        } else {
            writer->confirmMessage(msg);
        }
    }

    void WriterKafka::error_cb(rd_kafka_t* rkCb, int err, const char* reason, void* opaque) {
        const auto* writer = static_cast<Writer*>(opaque);

        writer->ctx->warning(70009, "Kafka: " + std::string(rd_kafka_err2name(static_cast<rd_kafka_resp_err_t>(err))) +
                             ", reason: " + reason);

        if (err != RD_KAFKA_RESP_ERR__FATAL)
            return;

        std::array<char, 512> errStrCb {};
        const rd_kafka_resp_err_t orig_err = rd_kafka_fatal_error(rkCb, errStrCb.data(), errStrCb.size());
        writer->ctx->error(10057, "Kafka: fatal error: " + std::string(rd_kafka_err2name(orig_err)) + ", reason: " + errStrCb.data());

        writer->ctx->stopHard();
    }

    void WriterKafka::logger_cb(const rd_kafka_t* rkCb, int level, const char* fac, const char* buf) {
        auto* writer = static_cast<WriterKafka*>(rd_kafka_opaque(rkCb));
        if (unlikely(writer->ctx->isTraceSet(Ctx::TRACE::WRITER)))
            writer->ctx->logTrace(Ctx::TRACE::WRITER, std::to_string(level) + ", rk: " + ((rkCb != nullptr) ? rd_kafka_name(rkCb) : nullptr) +
                                  ", fac: " + fac + ", err: " + buf);
    }

    void WriterKafka::sendMessage(BuilderMsg* msg) {
        // topicId comes from the same TopicMap that built rkts, so it is always in bounds
        ctx->assertDebug(msg->topicId < rkts.size());
        msg->ptr = reinterpret_cast<void*>(this);
        for (;;) {
            rd_kafka_resp_err_t err;
            if (msg->tagSize > 0)
                err = rd_kafka_producev(rk,
                                        RD_KAFKA_VTYPE_RKT, rkts[msg->topicId],
                                        RD_KAFKA_VTYPE_KEY, reinterpret_cast<void*>(msg->data), static_cast<size_t>(msg->tagSize),
                                        RD_KAFKA_VTYPE_VALUE, reinterpret_cast<void*>(msg->data + msg->tagSize), static_cast<size_t>(msg->size - msg->tagSize),
                                        RD_KAFKA_VTYPE_OPAQUE, reinterpret_cast<void*>(msg),
                                        RD_KAFKA_VTYPE_END);
            else
                err = rd_kafka_producev(rk,
                                        RD_KAFKA_VTYPE_RKT, rkts[msg->topicId],
                                        RD_KAFKA_VTYPE_VALUE, reinterpret_cast<void*>(msg->data), static_cast<size_t>(msg->size),
                                        RD_KAFKA_VTYPE_OPAQUE, reinterpret_cast<void*>(msg),
                                        RD_KAFKA_VTYPE_END);

            if (err != 0) {
                if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
                    ctx->warning(60031, "failed to produce to topic " + topicMap->names()[msg->topicId] + ", message: " + rd_kafka_err2str(err));
                    ctx->warning(60031, "queue, full, sleeping " + std::to_string(ctx->pollIntervalUs / 1000) + " ms, then retrying");
                    rd_kafka_poll(rk, static_cast<int>((ctx->pollIntervalUs / 1000)));
                    continue;
                }
                // Any other error means librdkafka refused the message: it would be neither sent nor
                // confirmed, leaving a gap and stalling the checkpoint
                ctx->error(10077, "Kafka: producing message " + std::to_string(msg->id) + " to topic " + topicMap->names()[msg->topicId] +
                           " failed: " + rd_kafka_err2str(err) + ", stopping replication");
                ctx->stopHard();
            }
            break;
        }

        rd_kafka_poll(rk, 0);
    }

    std::string WriterKafka::getType() const {
        return "Kafka:" + topic;
    }

    void WriterKafka::pollQueue() {
        if (metadata->status == Metadata::STATUS::READY)
            metadata->setStatusStarting(this);

        if (currentQueueSize > 0)
            rd_kafka_poll(rk, 0);
    }
}
