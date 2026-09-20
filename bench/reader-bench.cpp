/* Reader-path CPU benchmark: drives the real Reader::mainLoop over a synthetic redo file
 * of checksum-valid blocks, with a consumer thread that frees chunks like the parser does.
 * On a page-cache-served file this isolates the reader's per-request CPU cost, which is
 * what a "read-parallel 1 before/after" comparison needs. Compile against the baseline
 * tree (no read-parallel) or the new tree (-DHAS_READ_PARALLEL). See bench/README.md.
 *
 *   reader-bench <file> [read-parallel] [direct 0|1] [read-buffer-mb]
 *   reader-bench --make <file> <size-mb>
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../src/common/Ctx.h"
#include "../src/common/Thread.h"
#include "../src/reader/Reader.h"
using namespace OpenLogReplicator;

namespace {
    constexpr uint BLOCK_SIZE = 512;

    void setChecksum(const Ctx& ctx, uint8_t* block) {
        block[14] = 0; block[15] = 0;
        uint64_t sum = 0;
        for (unsigned i = 0; i < BLOCK_SIZE / 8; ++i) { uint64_t w; memcpy(&w, block + i * 8, 8); sum ^= w; }
        sum ^= (sum >> 32); sum ^= (sum >> 16);
        ctx.write16(block + 14, static_cast<uint16_t>(sum & 0xFFFF));
    }

    int makeFile(const Ctx& ctx, const char* path, uint64_t sizeMb) {
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); return 2; }
        std::vector<uint8_t> chunk(Ctx::MEMORY_CHUNK_SIZE);
        std::srand(1);
        for (uint64_t m = 0; m < sizeMb; ++m) {
            for (uint b = 0; b < Ctx::MEMORY_CHUNK_SIZE / BLOCK_SIZE; ++b) {
                uint8_t* p = chunk.data() + b * BLOCK_SIZE;
                for (uint i = 16; i < BLOCK_SIZE; ++i) p[i] = static_cast<uint8_t>(std::rand());
                p[0] = 0; p[1] = 0x22;
                ctx.write32(p + 4, static_cast<uint32_t>(m * (Ctx::MEMORY_CHUNK_SIZE / BLOCK_SIZE) + b));
                ctx.write32(p + 8, 1);
                setChecksum(ctx, p);
            }
            if (write(fd, chunk.data(), chunk.size()) != static_cast<ssize_t>(chunk.size())) { perror("write"); return 2; }
        }
        close(fd);
        return 0;
    }

    class Consumer final : public Thread {
    public:
        explicit Consumer(Ctx* c) : Thread(c, "consumer") {}
        void run() override {}
        std::string getName() const override { return "consumer"; }
    };

    class FileReader final : public Reader {
    public:
        std::string path;
        bool direct{false};
        int fd{-1};

        FileReader(Ctx* c) : Reader(c, "bench-reader", "bench", 0, false) {}
        using Reader::initialize;
        using Reader::mainLoop;
        using Reader::bufferFree;
        REDO_CODE openFile() { return redoOpen(); }
        void closeFile() { redoClose(); }

        void setup(uint64_t size) {
            blockSize = BLOCK_SIZE;
            fileSize = size;
            bufferStart = 0; bufferEnd = 0; bufferScan = 0;
            lastRead = BLOCK_SIZE; lastReadTime = 0; readTime = 0;
            reachedZero = false; readBlocks = false;
            nextScnHeader = Scn(1000);
            numBlocksHeader = static_cast<typeBlk>(size / BLOCK_SIZE);
        }
        uint64_t end() const { return bufferEnd; }

    protected:
        REDO_CODE redoOpen() override {
            fd = open(path.c_str(), O_RDONLY | (direct ? O_DIRECT : 0));
            if (fd < 0) { perror("open"); return REDO_CODE::ERROR; }
#ifdef HAS_READ_PARALLEL
            readParallel = ctx->readParallel > 1 ? static_cast<uint>(ctx->readParallel) : 1;
            startReadPool();
#endif
            return REDO_CODE::OK;
        }
        void redoClose() override {
#ifdef HAS_READ_PARALLEL
            stopReadPool();
            readParallel = 1;
#endif
            if (fd >= 0) close(fd);
            fd = -1;
        }
        int redoRead(uint8_t* buf, uint64_t offset, uint size) override {
            return static_cast<int>(pread(fd, buf, size, static_cast<off_t>(offset)));
        }
        void showHint(Thread*, std::string, std::string) const override {}
    };

    double cpuSeconds() {
        rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
        return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    }
}

int main(int argc, char** argv) {
    static Ctx ctx;
    if (argc >= 4 && std::strcmp(argv[1], "--make") == 0)
        return makeFile(ctx, argv[2], std::strtoull(argv[3], nullptr, 10));
    if (argc < 2) { std::fprintf(stderr, "usage: reader-bench <file> [read-parallel] [direct] [read-buffer-mb]\n"); return 2; }

    const uint parallel = argc > 2 ? std::atoi(argv[2]) : 1;
    const bool direct = argc > 3 && std::atoi(argv[3]) != 0;
    const uint64_t bufMb = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 128;
    ctx.initialize(bufMb + 8, bufMb + 64, bufMb, 4, 0, 0, 16, 4);
    ctx.redoReadSleepUs = 1000;
#ifdef HAS_READ_PARALLEL
    ctx.readParallel = parallel;
#else
    if (parallel != 1) { std::fprintf(stderr, "baseline build: read-parallel must be 1\n"); return 2; }
#endif
    struct stat st{};
    if (::stat(argv[1], &st) != 0) { perror("stat"); return 2; }
    const uint64_t size = static_cast<uint64_t>(st.st_size) / BLOCK_SIZE * BLOCK_SIZE;

    Consumer consumer(&ctx);
    FileReader reader(&ctx);
    reader.path = argv[1];
    reader.direct = direct;
    reader.initialize();
    reader.setup(size);
    if (reader.openFile() != Reader::REDO_CODE::OK) return 2;
    reader.setStatusRead();

    const double cpu0 = cpuSeconds();
    const auto t0 = std::chrono::steady_clock::now();

    // consumer: free chunks behind bufferEnd like Parser::parse does
    std::thread consumerThread([&] {
        uint64_t consumed = 0;
        while (consumed < size) {
            const uint64_t end = reader.end();
            if (end <= consumed) { std::this_thread::sleep_for(std::chrono::microseconds(50)); continue; }
            const uint64_t chunkBefore = consumed / Ctx::MEMORY_CHUNK_SIZE;
            const uint64_t chunkAfter = end / Ctx::MEMORY_CHUNK_SIZE;
            for (uint64_t c = chunkBefore; c < chunkAfter; ++c) {
                reader.bufferFree(&consumer, static_cast<uint>(c % ctx.memoryChunksReadBufferMax));
                reader.confirmReadData(FileOffset((c + 1) * Ctx::MEMORY_CHUNK_SIZE));
            }
            consumed = end;
        }
    });

    std::thread loopThread([&] { reader.mainLoop(); });

    consumerThread.join();
    ctx.softShutdown = true;
    reader.wakeUp();
    loopThread.join();
    reader.closeFile();

    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double cpu = cpuSeconds() - cpu0;
    std::printf("read-parallel=%u direct=%d size=%.0f MB ret=%d: wall %.3f s (%.0f MB/s), process cpu %.3f s (%.1f%% of wall), cpu per GB %.3f s\n",
                parallel, direct ? 1 : 0, size / 1048576.0, static_cast<int>(reader.getRet()), wall, size / 1048576.0 / wall,
                cpu, 100.0 * cpu / wall, cpu / (size / 1073741824.0));
    std::fflush(stdout);
    std::_Exit(reader.end() == size ? 0 : 1);
}
