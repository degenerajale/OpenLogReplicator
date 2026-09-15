/* Synthetic unit tests for the parallel reader (specs/olr-parallel-reader-plan.md,
 * specs/olr-parallel-reader-review.md).
 *
 * No real redo logs: TestReader serves an in-memory file of synthetic redo blocks and
 * the parallel path (read-parallel > 1) is compared with the single-request path.
 *
 * Build: part of -DWITH_TESTS=ON (target test_reader_parallel).
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../src/common/Ctx.h"
#include "../src/reader/Reader.h"

using namespace OpenLogReplicator;

namespace {
    constexpr uint BLOCK_SIZE = 512;
    constexpr uint BLOCK_COUNT = 64;
    constexpr uint64_t FILE_SIZE = static_cast<uint64_t>(BLOCK_SIZE) * BLOCK_COUNT;

    // The checksum field at +14 is XORed back into the fold by Reader::calcChSum, so a block
    // validates iff the stored value equals the fold computed with the field zeroed.
    void setChecksum(const Ctx& ctx, uint8_t* block, uint blockSize) {
        block[14] = 0;
        block[15] = 0;
        uint64_t sum = 0;
        for (unsigned i = 0; i < blockSize / 8; ++i) {
            uint64_t word;
            memcpy(&word, block + (i * 8), sizeof(word));
            sum ^= word;
        }
        sum ^= (sum >> 32);
        sum ^= (sum >> 16);
        ctx.write16(block + 14, static_cast<uint16_t>(sum & 0xFFFF));
    }

    std::vector<uint8_t> makeFile(const Ctx& ctx, uint32_t firstSequence, uint64_t badBlockNumber, bool corruptMidFile) {
        std::vector<uint8_t> file(FILE_SIZE, 0);
        for (uint b = 0; b < BLOCK_COUNT; ++b) {
            uint8_t* p = file.data() + static_cast<uint64_t>(b) * BLOCK_SIZE;
            p[0] = 0;
            p[1] = (BLOCK_SIZE == 4096) ? 0x82 : 0x22;
            const uint64_t number = (badBlockNumber != 0 && b == 20) ? badBlockNumber : b;
            ctx.write32(p + 4, static_cast<uint32_t>(number)); // block number
            ctx.write32(p + 8, firstSequence); // sequence
            setChecksum(ctx, p, BLOCK_SIZE);
        }
        if (corruptMidFile)
            file.data()[(20 * BLOCK_SIZE) + 100] ^= 0xFF;
        return file;
    }

    class TestReader final : public Reader {
    public:
        std::vector<uint8_t> file;
        uint64_t shortFirstAt{0}; // read at offset 0 returns only this many bytes
        std::atomic<uint64_t> frontier{0}; // when > 0, reads stop at this offset
        bool failAtZero{false}; // read at offset 0 returns -1
        bool blockReads{false}; // reads at offset > 0 block until released
        std::atomic<uint64_t> reads{0};

        std::mutex blockMtx;
        std::condition_variable blockCond;
        int blocked{0};
        std::atomic<bool> freed{false};
        std::atomic<int> writesAfterFree{0};

        TestReader(Ctx* newCtx, int newGroup) :
            Reader(newCtx, "test-reader", "test-db", newGroup, false) {}

        using Reader::bufferFree;
        using Reader::drainReads;
        using Reader::initialize;
        using Reader::mainLoop;
        using Reader::read1;
        using Reader::startReadPool;
        using Reader::stopReadPool;

        void prepareMainloop(Scn nextScnValue, typeBlk numBlocks) {
            nextScnHeader = nextScnValue;
            numBlocksHeader = numBlocks;
        }

        void setParallel(uint n) {
            ctx->readParallel = n;
            readParallel = n;
        }

        void setup(uint newBlockSize, uint64_t newFileSize) {
            blockSize = newBlockSize;
            fileSize = newFileSize;
            bufferStart = 0;
            bufferEnd = 0;
            bufferScan = 0;
            lastRead = newBlockSize;
            lastReadTime = 0;
            readTime = 0;
            reachedZero = false;
            readBlocks = false;
        }

        [[nodiscard]] uint64_t bufferEndNow() const {
            return bufferEnd;
        }

        [[nodiscard]] uint64_t bufferScanNow() const {
            return bufferScan;
        }

    protected:
        void redoClose() override {
            stopReadPool();
            readParallel = 1;
        }

        REDO_CODE redoOpen() override {
            readParallel = (ctx->readParallel > 1) ? static_cast<uint>(ctx->readParallel) : 1;
            startReadPool();
            return REDO_CODE::OK;
        }

        int redoRead(uint8_t* buf, uint64_t offset, uint size) override {
            ++reads;
            if (failAtZero && offset == 0) {
                errno = EIO;
                return -1;
            }

            if (blockReads && offset > 0) {
                std::unique_lock lck(blockMtx);
                ++blocked;
                blockCond.notify_all();
                blockCond.wait(lck, [this] { return !blockReads; });
                if (freed)
                    ++writesAfterFree;
            }

            if (offset >= file.size())
                return 0;

            uint64_t n = std::min<uint64_t>(size, file.size() - offset);
            if (shortFirstAt > 0 && offset == 0 && n > shortFirstAt)
                n = shortFirstAt;

            const uint64_t front = frontier.load();
            if (front != 0 && offset + n > front)
                n = (front > offset) ? front - offset : 0;

            memcpy(buf, file.data() + offset, n);
            return static_cast<int>(n);
        }

        void showHint(Thread*, std::string, std::string) const override {}
    };

    struct RunResult {
        std::vector<uint64_t> ends;
        std::vector<uint8_t> published;
        Reader::REDO_CODE ret{Reader::REDO_CODE::OK};
    };

    void capturePublished(Ctx& ctx, const TestReader& reader, uint64_t from, uint64_t to, std::vector<uint8_t>& out) {
        for (uint64_t off = from; off < to; ++off) {
            const uint64_t pos = off % Ctx::MEMORY_CHUNK_SIZE;
            const uint64_t num = (off / Ctx::MEMORY_CHUNK_SIZE) % ctx.memoryChunksReadBufferMax;
            out.push_back(reader.redoBufferList[num][pos]);
        }
    }

    // Runs read1 in a loop. When moving is set the frontier advances after every publish,
    // so younger requests hold stale zeros that must be discarded and re-read.
    RunResult runSequence(Ctx& ctx, const std::vector<uint8_t>& file, int group, uint parallel, uint64_t shortFirstAt) {
        TestReader reader(&ctx, group);
        reader.file = file;
        reader.shortFirstAt = shortFirstAt;
        reader.setParallel(parallel);
        reader.initialize();
        reader.setup(BLOCK_SIZE, file.size());
        reader.startReadPool();

        RunResult result;
        uint64_t prevEnd = 0;
        for (int guard = 0; guard < 100000; ++guard) {
            if (prevEnd >= file.size())
                break;
            if (!reader.read1())
                break;

            const uint64_t newEnd = reader.bufferEndNow();
            capturePublished(ctx, reader, prevEnd, newEnd, result.published);
            result.ends.push_back(newEnd);

            prevEnd = newEnd;
        }
        result.ret = reader.getRet();
        reader.stopReadPool();
        return result;
    }

    bool check(const char* name, Ctx& ctx, const std::vector<uint8_t>& file, uint parallel, uint64_t shortFirstAt) {
        const RunResult single = runSequence(ctx, file, 0, 1, shortFirstAt);
        const RunResult multi = runSequence(ctx, file, 0, parallel, shortFirstAt);

        if (single.ends != multi.ends || single.published != multi.published || single.ret != multi.ret) {
            std::printf("FAIL %s: read-parallel=%u (single %zu steps, multi %zu steps)\n", name, parallel, single.ends.size(), multi.ends.size());
            return false;
        }
        std::printf("PASS %s: read-parallel=%u, %zu publish steps, end=%llu, ret=%u\n", name, parallel, single.ends.size(),
                    static_cast<unsigned long long>(single.ends.empty() ? 0 : single.ends.back()), static_cast<uint>(single.ret));
        return true;
    }

    // The frontier moves while younger reads are already in flight; what matters is that
    // everything published is the file itself and nothing is published past the frontier.
    bool checkMoving(Ctx& ctx, const std::vector<uint8_t>& file, uint parallel) {
        TestReader reader(&ctx, 1);
        reader.file = file;
        reader.frontier = BLOCK_SIZE * 4;
        reader.setParallel(parallel);
        reader.initialize();
        reader.setup(BLOCK_SIZE, file.size());
        reader.startReadPool();

        std::vector<uint8_t> published;
        uint64_t prevEnd = 0;
        bool ok = true;
        for (int guard = 0; guard < 100000 && prevEnd < file.size(); ++guard) {
            const uint64_t frontierBefore = reader.frontier;
            if (!reader.read1()) {
                std::printf("  moving-frontier stop: ret=%u end=%llu frontier=%llu\n", static_cast<uint>(reader.getRet()),
                            static_cast<unsigned long long>(reader.bufferEndNow()), static_cast<unsigned long long>(reader.frontier.load()));
                break;
            }
            const uint64_t newEnd = reader.bufferEndNow();
            if (newEnd > frontierBefore) {
                std::printf("FAIL moving-frontier: end %llu past frontier %llu\n", static_cast<unsigned long long>(newEnd),
                            static_cast<unsigned long long>(frontierBefore));
                ok = false;
                break;
            }
            capturePublished(ctx, reader, prevEnd, newEnd, published);
            prevEnd = newEnd;
            reader.frontier = std::min<uint64_t>(file.size(), reader.bufferScanNow() + BLOCK_SIZE * 8);
        }
        reader.stopReadPool();

        if (ok && (published.size() != file.size() || memcmp(published.data(), file.data(), file.size()) != 0)) {
            std::printf("FAIL moving-frontier: published %zu of %zu bytes\n", published.size(), file.size());
            ok = false;
        }
        if (ok)
            std::printf("PASS moving-frontier: read-parallel=%u, %zu bytes published in order\n", parallel, published.size());
        return ok;
    }

    bool checkBadBlock(Ctx& ctx) {
        const std::vector<uint8_t> file = makeFile(ctx, 7, 9999, false);
        const RunResult single = runSequence(ctx, file, 1, 1, 0);
        const RunResult multi = runSequence(ctx, file, 1, 4, 0);

        if (single.ends != multi.ends || single.ret != multi.ret) {
            std::printf("FAIL bad-block: single end %llu ret %u, multi end %llu ret %u\n",
                        static_cast<unsigned long long>(single.ends.empty() ? 0 : single.ends.back()), static_cast<uint>(single.ret),
                        static_cast<unsigned long long>(multi.ends.empty() ? 0 : multi.ends.back()), static_cast<uint>(multi.ret));
            return false;
        }
        if (single.ret != Reader::REDO_CODE::ERROR_BLOCK) {
            std::printf("FAIL bad-block: expected ERROR_BLOCK, got %u\n", static_cast<uint>(single.ret));
            return false;
        }
        std::printf("PASS bad-block: stopped at %llu with ERROR_BLOCK\n",
                    static_cast<unsigned long long>(single.ends.empty() ? 0 : single.ends.back()));
        return true;
    }

    // A CRC failure outside the header, in direct IO mode, must stop at the same point in the
    // parallel and single paths with plain ERROR_CRC. The ERROR_CRC -> EMPTY conversion is
    // gated on redoVerifyDelayUs > 0 && group != 0, where the parallel path is off, so the
    // parallel run must never turn it into EMPTY.
    bool checkCrc(Ctx& ctx) {
        const std::vector<uint8_t> file = makeFile(ctx, 7, 0, true);
        const RunResult single = runSequence(ctx, file, 1, 1, 0);
        const RunResult multi = runSequence(ctx, file, 1, 4, 0);

        if (single.ends != multi.ends || single.ret != multi.ret) {
            std::printf("FAIL crc: single end %llu ret %u, multi end %llu ret %u\n",
                        static_cast<unsigned long long>(single.ends.empty() ? 0 : single.ends.back()), static_cast<uint>(single.ret),
                        static_cast<unsigned long long>(multi.ends.empty() ? 0 : multi.ends.back()), static_cast<uint>(multi.ret));
            return false;
        }
        if (single.ret != Reader::REDO_CODE::ERROR_CRC) {
            std::printf("FAIL crc: expected ERROR_CRC, got %u\n", static_cast<uint>(single.ret));
            return false;
        }
        std::printf("PASS crc: stopped at %llu with ERROR_CRC\n",
                    static_cast<unsigned long long>(single.ends.empty() ? 0 : single.ends.back()));
        return true;
    }

    // A read error on the oldest request must not let the reader free chunks while younger
    // requests are still landing: drainReads must wait for them.
    bool checkDrain(Ctx& ctx) {
        TestReader reader(&ctx, 0);
        reader.file = makeFile(ctx, 7, 0, false);
        reader.failAtZero = true;
        reader.blockReads = true;
        reader.setParallel(4);
        reader.initialize();
        reader.setup(BLOCK_SIZE, reader.file.size());
        reader.startReadPool();

        if (reader.read1()) {
            std::printf("FAIL drain: read1 returned true for a failed oldest read\n");
            return false;
        }

        {
            std::unique_lock lck(reader.blockMtx);
            reader.blockCond.wait_for(lck, std::chrono::seconds(5), [&] { return reader.blocked >= 3; });
            if (reader.blocked < 3) {
                std::printf("FAIL drain: only %d reads blocked\n", reader.blocked);
                return false;
            }
        }

        std::atomic<bool> drainDone{false};
        std::thread drainThread([&] {
            reader.drainReads();
            drainDone = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (drainDone) {
            std::printf("FAIL drain: returned while younger reads were still blocked\n");
            return false;
        }

        {
            std::unique_lock lck(reader.blockMtx);
            reader.blockReads = false;
            reader.blockCond.notify_all();
        }
        drainThread.join();
        if (!drainDone) {
            std::printf("FAIL drain: did not finish after the reads were released\n");
            return false;
        }

        reader.freed = true;
        for (uint num = 0; num < ctx.memoryChunksReadBufferMax; ++num)
            reader.bufferFree(&reader, num);
        reader.stopReadPool();

        if (reader.writesAfterFree != 0) {
            std::printf("FAIL drain: %d writes after chunks were freed\n", reader.writesAfterFree.load());
            return false;
        }
        std::printf("PASS drain: waited for 3 in-flight reads, no write after free\n");
        return true;
    }

    // Drives the real mainLoop. Before the gate fix, once bufferScan reaches fileSize the
    // submit conditions are false and the last in-flight reads are never consumed, so the
    // reader stalls short of the end and sleeps/spins.
    bool checkMainloopTail(Ctx& ctx, uint parallel) {
        TestReader reader(&ctx, 0);
        reader.file = makeFile(ctx, 7, 0, false);
        reader.setParallel(parallel);
        reader.initialize();
        reader.setup(BLOCK_SIZE, reader.file.size());
        reader.startReadPool();
        reader.prepareMainloop(Scn(123), BLOCK_COUNT);
        reader.setStatusRead();

        const uint64_t savedSleep = ctx.redoReadSleepUs;
        ctx.redoReadSleepUs = 1000;

        std::thread loopThread([&] { reader.mainLoop(); });

        bool finished = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (reader.getRet() == Reader::REDO_CODE::FINISHED && reader.bufferEndNow() == FILE_SIZE) {
                finished = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        ctx.softShutdown = true;
        reader.wakeUp();
        loopThread.join();

        ctx.redoReadSleepUs = savedSleep;
        ctx.softShutdown = false;
        reader.stopReadPool();

        if (!finished) {
            std::printf("FAIL mainloop-tail: read-parallel=%u, end=%llu of %llu, ret=%u\n", parallel,
                        static_cast<unsigned long long>(reader.bufferEndNow()), static_cast<unsigned long long>(FILE_SIZE),
                        static_cast<uint>(reader.getRet()));
            return false;
        }
        std::printf("PASS mainloop-tail: read-parallel=%u, end=%llu\n", parallel,
                    static_cast<unsigned long long>(reader.bufferEndNow()));
        return true;
    }
}

int main() {
    Ctx ctx;
    ctx.initialize(32, 64, 32, 8, 0, 8, 32, 8);
    ctx.version = 0x13000000;

    const std::vector<uint8_t> file = makeFile(ctx, 7, 0, false);

    bool ok = true;
    ok &= check("all-good", ctx, file, 4, 0);
    ok &= check("all-good", ctx, file, 8, 0);
    ok &= check("short-first-read", ctx, file, 4, 700);
    ok &= check("short-first-read", ctx, file, 8, 700);

    // Archived logs in buffered mode still use the parallel path (read2 is online-only).
    ctx.redoVerifyDelayUs = 500000;
    ok &= check("archived-buffered", ctx, file, 4, 0);
    ok &= check("archived-buffered", ctx, file, 8, 0);
    ctx.redoVerifyDelayUs = 0;
    ok &= checkMoving(ctx, file, 4);
    ok &= checkMoving(ctx, file, 8);
    ok &= checkBadBlock(ctx);
    ok &= checkCrc(ctx);
    ok &= checkDrain(ctx);
    ok &= checkMainloopTail(ctx, 1);
    ok &= checkMainloopTail(ctx, 4);
    ok &= checkMainloopTail(ctx, 8);

    if (!ok) {
        std::printf("reader-parallel test FAILED\n");
        return 1;
    }
    std::printf("reader-parallel test passed (%llu bytes, %u blocks)\n", static_cast<unsigned long long>(FILE_SIZE), BLOCK_COUNT);
    return 0;
}