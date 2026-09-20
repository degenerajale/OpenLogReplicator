/* Unit test for the partial-commit marker: a transaction committed without a begin record
 * (already open when replication started) produces a standalone {"op":"p"} message that
 * carries the commit position and the xid, and nothing else.
 *
 * Build: part of -DWITH_TESTS=ON (target test_builder_partial).
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/builder/BuilderJson.h"
#include "../src/common/Attribute.h"
#include "../src/common/Ctx.h"
#include "../src/common/Thread.h"
#include "../src/common/types/Seq.h"
#include "../src/common/types/Time.h"
#include "../src/common/types/Xid.h"
#include "../src/locales/Locales.h"
#include "../src/metadata/Metadata.h"

using namespace OpenLogReplicator;

namespace {
    int failures = 0;

    void check(bool ok, const std::string& what) {
        if (ok)
            return;
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }

    void expectContains(const std::string& msg, const std::string& needle, const std::string& what) {
        check(msg.find(needle) != std::string::npos, what + ": expected '" + needle + "' in " + msg);
    }

    void expectNotContains(const std::string& msg, const std::string& needle, const std::string& what) {
        check(msg.find(needle) == std::string::npos, what + ": unexpected '" + needle + "' in " + msg);
    }

    class TestThread final : public Thread {
    public:
        TestThread(Ctx* ctx) : Thread(ctx, "test") {}
        void run() override {}
        std::string getName() const override { return "test"; }
    };

    // Walk the builder queue from the start and return the n-th message as text.
    std::string message(const Builder* builder, uint64_t n, const BuilderMsg** out = nullptr) {
        const BuilderQueue* queue = builder->firstBuilderQueue;
        uint64_t pos = 0;
        for (uint64_t i = 0;; ++i) {
            const auto* msg = reinterpret_cast<const BuilderMsg*>(queue->data + pos);
            const uint64_t size = msg->size.load();
            if (i == n) {
                if (out != nullptr)
                    *out = msg;
                return std::string(reinterpret_cast<const char*>(msg->data), size);
            }
            pos += sizeof(BuilderMsg) + size;
            pos = (pos + 7) & ~static_cast<uint64_t>(7);
        }
    }

    Format makeFormat(Format::SCN_TYPE scnType, Format::TIMESTAMP_TYPE timestampType, Format::MESSAGE_FORMAT messageFormat) {
        return Format(Format::DB_FORMAT::DEFAULT, Format::ATTRIBUTES_FORMAT::DEFAULT, Format::INTERVAL_DTS_FORMAT::UNIX_NANO,
                      Format::INTERVAL_YTM_FORMAT::MONTHS, messageFormat, Format::RID_FORMAT::SKIP, Format::REDO_THREAD_FORMAT::SKIP,
                      Format::XID_FORMAT::TEXT_HEX, Format::TIMESTAMP_FORMAT::UNIX_NANO, Format::TIMESTAMP_FORMAT::UNIX_NANO,
                      Format::TIMESTAMP_TZ_FORMAT::UNIX_NANO_STRING, timestampType, Format::CHAR_FORMAT::UTF8, Format::SCN_FORMAT::NUMERIC,
                      scnType, Format::UNKNOWN_FORMAT::QUESTION_MARK, Format::SCHEMA_FORMAT::DEFAULT, Format::COLUMN_FORMAT::CHANGED,
                      Format::UNKNOWN_TYPE::HIDE, Format::USER_TYPE::DEFAULT);
    }
}

int main() {
    static Ctx ctx;
    TestThread thread(&ctx);
    ctx.parserThread = &thread;
    ctx.initialize(32, 64, 8, 4, 0, 4, 8, 4);

    Locales locales;
    locales.initialize();
    Metadata metadata(&ctx, &locales, "TEST", Scn::none(), Seq::none(), "", 0);

    const Xid xid(0x0009, 0x01a, 0x0000abcd);

    // The user's production shape: DML-only stream (message 12), e_scn/e_tm on every message.
    {
        Format format = makeFormat(Format::SCN_TYPE::COMMIT, Format::TIMESTAMP_TYPE::COMMIT,
                                   static_cast<Format::MESSAGE_FORMAT>(static_cast<unsigned char>(Format::MESSAGE_FORMAT::SKIP_BEGIN) |
                                                                       static_cast<unsigned char>(Format::MESSAGE_FORMAT::SKIP_COMMIT)));
        BuilderJson builder(&ctx, &locales, &metadata, format, 0);
        builder.initialize();

        builder.processPartial(xid, 1, Seq(77), Scn(123456), Time(1700000000));
        const BuilderMsg* msg = nullptr;
        const std::string text = message(&builder, 0, &msg);

        expectContains(text, R"("payload":[{"op":"p"}])", "op");
        expectContains(text, R"("e_scn":123456)", "commit scn as e_scn");
        expectContains(text, R"("scn":123456)", "scn on a standalone message");
        expectContains(text, R"("e_tm":)", "commit timestamp");
        expectContains(text, R"("xid":"0x0009.01a.0000abcd")", "xid in OLR text form");
        // c_idx in the text is the post-increment counter (1-based), as on every other message
        expectContains(text, R"("c_scn":123456,"c_idx":1)", "position anchored at the commit");
        expectNotContains(text, R"("b_scn")", "no begin scn requested");
        expectNotContains(text, R"("payload":[{"op":"commit")", "not a commit envelope");
        check(text.front() == '{' && text.back() == '}', "one complete JSON object");
        check(msg != nullptr && msg->scn == Scn(123456) && msg->lwnScn == Scn(123456) && msg->lwnIdx == 0 && msg->sequence == Seq(77),
              "message header: scn/lwn/sequence at the commit position");

        // A second partial at the same commit scn gets the next index; a different scn resets it.
        builder.processPartial(Xid(0x0009, 0x01b, 0x0000abce), 1, Seq(77), Scn(123456), Time(1700000000));
        expectContains(message(&builder, 1), R"("c_scn":123456,"c_idx":2)", "second partial at the same scn");
        builder.processPartial(Xid(0x0009, 0x01c, 0x0000abcf), 1, Seq(78), Scn(123999), Time(1700000001));
        expectContains(message(&builder, 2), R"("c_scn":123999,"c_idx":1)", "partial at a new scn");

        // A following empty (begin-only) transaction is still swallowed, as before.
        AttributeMap attributes;
        attributes.insert_or_assign(Attribute::KEY::CLIENT_ID, "v");
        const uint64_t before = builder.lastBuilderQueue->confirmedSize;
        builder.processBegin(Xid(0x0009, 0x01d, 0x0000abd0), 1, Seq(78), Scn(124000), Time(1700000002), Seq(78), Scn(124001), Time(1700000003),
                             &attributes);
        builder.processCommit();
        check(builder.lastBuilderQueue->confirmedSize == before, "no message after an empty transaction");
    }

    // Default format: the marker still stands on its own and identifies the transaction.
    {
        Format format = makeFormat(Format::SCN_TYPE::DEFAULT, Format::TIMESTAMP_TYPE::DEFAULT, Format::MESSAGE_FORMAT::DEFAULT);
        BuilderJson builder(&ctx, &locales, &metadata, format, 0);
        builder.initialize();
        builder.processPartial(xid, 1, Seq(5), Scn(42), Time(1700000000));
        const std::string text = message(&builder, 0);
        expectContains(text, R"("scn":42)", "default: scn present");
        expectContains(text, R"("tm":)", "default: timestamp present");
        expectContains(text, R"("xid":"0x0009.01a.0000abcd")", "default: xid");
        expectContains(text, R"("payload":[{"op":"p"}]})", "default: payload closes the message");
    }

    // Kafka topic map: one copy per topic (default first), identical apart from c_idx.
    {
        Metadata mapped(&ctx, &locales, "TEST", Scn::none(), Seq::none(), "", 0);
        mapped.topicMap.setDefault("olr");
        mapped.topicMap.add("FS.PRICES", "prices");
        mapped.topicMap.add("FS.ORDERS", "orders");
        Format format = makeFormat(Format::SCN_TYPE::COMMIT, Format::TIMESTAMP_TYPE::COMMIT, Format::MESSAGE_FORMAT::DEFAULT);
        BuilderJson builder(&ctx, &locales, &mapped, format, 0);
        builder.initialize();
        builder.processPartial(xid, 1, Seq(5), Scn(42), Time(1700000000));
        for (uint64_t i = 0; i < 3; ++i) {
            const BuilderMsg* msg = nullptr;
            const std::string text = message(&builder, i, &msg);
            check(msg != nullptr && msg->topicId == i, "copy " + std::to_string(i) + " routed to topic id " + std::to_string(i));
            expectContains(text, R"("payload":[{"op":"p"}])", "copy " + std::to_string(i) + " op");
            expectContains(text, R"("xid":"0x0009.01a.0000abcd")", "copy " + std::to_string(i) + " xid");
            expectContains(text, R"("c_scn":42,"c_idx":)" + std::to_string(i + 1), "copy " + std::to_string(i) + " index");
        }
        const uint64_t after = builder.lastBuilderQueue->confirmedSize;
        builder.processPartial(Xid(0x0009, 0x01b, 0x0000abce), 1, Seq(5), Scn(43), Time(1700000000));
        check(builder.lastBuilderQueue->confirmedSize > after, "second partial appended");
        const BuilderMsg* msg = nullptr;
        message(&builder, 5, &msg);
        check(msg != nullptr && msg->topicId == 2 && msg->lwnScn == Scn(43), "exactly three copies per partial");
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_builder_partial: OK\n");
    return 0;
}
