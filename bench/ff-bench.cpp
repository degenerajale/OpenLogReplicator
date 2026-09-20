/*
 * Synthetic microbenchmark for the FAST_FILTER change (specs/olr-fast-filter-plan.md).
 *
 * It does not read redo logs. It builds one structurally valid row-DML change-vector
 * pair (undo 0x0501 with opc 0x0B01 + redo 0x0B05) in memory and measures the CPU
 * cost of:
 *   - the full decode performed today (process0501 + process0B05), and
 *   - the FAST_FILTER path (process0501Head + header peek of the redo vector).
 *
 * "Infinite I/O" is assumed: everything is L1-resident and there is no reader,
 * heap or transaction bookkeeping. The result is therefore an *upper bound* on the
 * per-vector CPU saving; it isolates the decode work that the filter removes.
 *
 * Build:
 *   - as part of a normal build with -DWITH_TESTS=ON (target "ff_bench"); or
 *   - against an existing Release tree with bench/build-ff-bench.sh.
 * Build a Release tree (-DCMAKE_BUILD_TYPE=Release) for meaningful timings; the Debug
 * tree is -O0 with sanitizers and is only useful as a smoke test.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/common/Ctx.h"
#include "../src/common/RedoLogRecord.h"
// test-only: expose the protected 0x0501 helpers for granular timing
#define protected public
#include "../src/parser/OpCode0501.h"
#undef protected
#include "../src/parser/OpCode0B05.h"

using namespace OpenLogReplicator;

namespace {

volatile uint64_t g_sink = 0;

struct Vec {
    std::vector<uint8_t> data;
    uint32_t fieldOffset{32};
    uint32_t fieldPos{0};
    uint32_t fieldCnt{0};
    uint32_t total{0};
    std::vector<uint16_t> sizes; // 1-based

    [[nodiscard]] uint32_t pre(uint16_t n) const {
        uint32_t p = 0;
        for (uint16_t i = 1; i < n; ++i)
            p += (sizes[i] + 3) & 0xFFFC;
        return p;
    }

    [[nodiscard]] uint8_t* field(uint16_t n) {
        return data.data() + fieldPos + pre(n);
    }
};

Vec buildVector(const Ctx* ctx, uint16_t opCode, uint32_t fieldOffset, const std::vector<uint16_t>& fieldSizes) {
    Vec v;
    v.fieldOffset = fieldOffset;
    v.fieldCnt = static_cast<uint32_t>(fieldSizes.size());
    v.sizes.assign(v.fieldCnt + 1, 0);
    for (uint32_t i = 0; i < v.fieldCnt; ++i)
        v.sizes[i + 1] = fieldSizes[i];

    const uint32_t len = v.fieldCnt * 2 + 2;
    v.fieldPos = fieldOffset + ((len + 2) & 0xFFFC);
    uint32_t total = v.fieldPos;
    for (uint32_t i = 1; i <= v.fieldCnt; ++i)
        total += (v.sizes[i] + 3) & 0xFFFC;
    v.total = total;
    v.data.assign(total, 0);

    v.data[0] = static_cast<uint8_t>(opCode >> 8);
    v.data[1] = static_cast<uint8_t>(opCode & 0xFF);
    ctx->write16(v.data.data() + fieldOffset, static_cast<uint16_t>(len));
    for (uint32_t i = 1; i <= v.fieldCnt; ++i)
        ctx->write16(v.data.data() + fieldOffset + i * 2, v.sizes[i]);
    return v;
}

void initRec(RedoLogRecord& rec, Vec& v, uint16_t opCode, uint32_t vectorNo) {
    rec.clear();
    rec.opCode = opCode;
    rec.size = v.total;
    rec.fieldSizesDelta = v.fieldOffset;
    rec.fieldPos = v.fieldPos;
    rec.fieldCnt = v.fieldCnt;
    rec.dataExt = v.data.data();
    rec.vectorNo = vectorNo;
    rec.recordObj = 0xFFFFFFFF;
    rec.recordDataObj = 0xFFFFFFFF;
    rec.thread = 1;
}

// Faithful subset of Parser::parseVectorHeader for the redo vector (no body decode).
void peekHeader(const Ctx* ctx, const Vec& v, RedoLogRecord& out) {
    out.clear();
    uint8_t* data = const_cast<uint8_t*>(v.data.data());
    out.dataExt = data;
    const uint32_t offset = 0;
    uint16_t fieldOffset;
    if (ctx->version >= RedoLogRecord::REDO_VERSION_12_1) {
        fieldOffset = 32;
        out.flgRecord = ctx->read16(data + offset + 28);
        out.conId = static_cast<typeConId>(ctx->read16(data + offset + 24));
    } else {
        fieldOffset = 24;
        out.flgRecord = 0;
        out.conId = 0;
    }
    const uint8_t* fieldList = data + offset + fieldOffset;
    out.opCode = (static_cast<typeOp1>(data[offset + 0]) << 8) | data[offset + 1];
    out.size = fieldOffset + ((ctx->read16(fieldList) + 2) & 0xFFFC);
    out.fieldSizesDelta = fieldOffset;
    out.fieldCnt = (ctx->read16(out.data(out.fieldSizesDelta)) - 2) / 2;
    out.fieldPos = fieldOffset + ((ctx->read16(out.data(out.fieldSizesDelta)) + 2) & 0xFFFC);
    for (typeField i = 1; i <= out.fieldCnt; ++i)
        out.size += (ctx->read16(fieldList + (i * 2)) + 3) & 0xFFFC;
}

constexpr uint16_t COL_SIZE = 12;  // bytes per column piece
constexpr uint16_t KDO_SIZE = 32;  // KDO opcode header, holds cc + nulls
constexpr uint16_t KTB_SIZE = 8;
struct Pair {
    Vec undo;
    Vec redo;
    RedoLogRecord undoRec;
    RedoLogRecord redoRec;
    RedoLogRecord peekRec;
};

void makePair(Pair& p, const Ctx* ctx, uint16_t cc, uint16_t suppCc) {

    // ---- undo 0x0501 (opc 0x0B01 = row DML) ----
    std::vector<uint16_t> u;
    u.push_back(24);                 // 1 ktudb
    u.push_back(40);                 // 2 ktub
    u.push_back(KTB_SIZE);           // 3 ktbRedo
    u.push_back(KDO_SIZE);           // 4 KDO opcode
    u.push_back(2 * cc);             // 5 colNums
    for (uint16_t i = 0; i < cc; ++i)
        u.push_back(COL_SIZE);       // 6..6+cc-1 column data
    u.push_back(26);                 // supp log header
    u.push_back(2 * suppCc);         // supp column numbers
    u.push_back(20);                 // supp column lengths
    for (uint16_t i = 0; i < suppCc; ++i)
        u.push_back(COL_SIZE);       // supp column data
    p.undo = buildVector(ctx, 0x0501, 32, u);

    // ktudb
    ctx->write16(p.undo.field(1) + 8, 7);       // usn
    ctx->write16(p.undo.field(1) + 10, 3);      // slt
    ctx->write32(p.undo.field(1) + 12, 0x11223344); // xid
    // ktub
    ctx->write32(p.undo.field(2) + 0, 1234);    // obj
    ctx->write32(p.undo.field(2) + 4, 5678);    // dataObj
    *(p.undo.field(2) + 16) = 0x0B;             // opc high
    *(p.undo.field(2) + 17) = 0x01;             // opc low
    ctx->write16(p.undo.field(2) + 20, 0x0000); // flg
    // ktbRedo (KTBOP_Z: no further fields needed)
    *p.undo.field(3) = 0x03;
    *(p.undo.field(3) + 1) = 0x04;
    // KDO opcode: op = URP, cc, nulls
    *p.undo.field(4) = static_cast<uint8_t>(RedoLogRecord::OP_URP);
    *(p.undo.field(4) + 10) = static_cast<uint8_t>(RedoLogRecord::OP_URP);
    *(p.undo.field(4) + 11) = 0x00; // flags
    *(p.undo.field(4) + 23) = static_cast<uint8_t>(cc);
    *(p.undo.field(4) + 26) = 0x00; // nulls: all columns present
    // supp log header: suppLogCC
    ctx->write16(p.undo.field(14) + 2, suppCc);

    // ---- redo 0x0B05 (update row piece) ----
    std::vector<uint16_t> r;
    r.push_back(KTB_SIZE);           // 1 ktbRedo
    r.push_back(KDO_SIZE);           // 2 KDO opcode
    r.push_back(2 * cc);             // 3 colNums
    for (uint16_t i = 0; i < cc; ++i)
        r.push_back(COL_SIZE);       // 4..4+cc-1
    p.redo = buildVector(ctx, 0x0B05, 32, r);

    *p.redo.field(1) = 0x03;
    *(p.redo.field(1) + 1) = 0x04;
    *(p.redo.field(2) + 10) = static_cast<uint8_t>(RedoLogRecord::OP_URP);
    *(p.redo.field(2) + 11) = 0x00;
    *(p.redo.field(2) + 23) = static_cast<uint8_t>(cc);
    *(p.redo.field(2) + 26) = 0x00;

    initRec(p.undoRec, p.undo, 0x0501, 1);
    initRec(p.redoRec, p.redo, 0x0B05, 2);
    initRec(p.peekRec, p.redo, 0x0B05, 2);
}

template <typename Fn>
double bench(const char* name, uint64_t n, Fn fn) {
    // warmup
    for (uint64_t i = 0; i < n / 10; ++i)
        fn(i);
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < n; ++i)
        fn(i);
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n);
    std::printf("  %-28s %8.1f ns/op\n", name, ns);
    return ns;
}

// Field-walk cost only: the four nextField calls the 0x0501 head makes.
typeSize walkUndo(const Ctx* ctx, RedoLogRecord* rec) {
    typePos fieldPos = 0;
    typeField fieldNum = 0;
    typeSize fieldSize = 0;
    RedoLogRecord::nextFieldOpt(ctx, rec, fieldNum, fieldPos, fieldSize, 0x050101);
    RedoLogRecord::nextFieldOpt(ctx, rec, fieldNum, fieldPos, fieldSize, 0x050102);
    RedoLogRecord::nextField(ctx, rec, fieldNum, fieldPos, fieldSize, 0x050112);
    RedoLogRecord::nextFieldOpt(ctx, rec, fieldNum, fieldPos, fieldSize, 0x050113);
    return fieldPos + fieldSize;
}

// The unconditional std::string work in OpCode::ktub (built before the dump guard).
// noinline + consume the characters so the compiler cannot fold the constructions away.
__attribute__((noinline)) uint64_t ktubStrings(const Ctx* ctx, const RedoLogRecord* rec) {
    std::string ktuType{"ktubu"};
    std::string prevObj;
    std::string postObj;
    std::string lastBufferSplit = (ctx->version < RedoLogRecord::REDO_VERSION_19_0) ? "No" : " No";
    std::string userUndoDone = (ctx->version < RedoLogRecord::REDO_VERSION_19_0) ? "No" : " No";
    std::string undoType = (ctx->version < RedoLogRecord::REDO_VERSION_19_0) ? "Regular undo      " : "Regular undo";
    std::string tempObject = (ctx->version < RedoLogRecord::REDO_VERSION_19_0) ? "No" : " No";
    std::string tablespaceUndo = (ctx->version < RedoLogRecord::REDO_VERSION_19_0) ? "No" : " No";
    std::string userOnly = (ctx->version < RedoLogRecord::REDO_VERSION_19_0) ? "No" : " No";
    return ktuType[0] + prevObj.size() + postObj.size() + lastBufferSplit[0] + userUndoDone[0] +
            undoType[undoType.size() - 1] + tempObject[0] + tablespaceUndo[0] + userOnly[0] + rec->flg;
}

// Replica of process0501Head's work, to cross-check the measured head cost.
bool headCopy(Ctx* ctx, RedoLogRecord* r) {
    typePos fieldPos = 0;
    typeField fieldNum = 0;
    typeSize fieldSize = 0;
    if (!RedoLogRecord::nextFieldOpt(ctx, r, fieldNum, fieldPos, fieldSize, 0x050101))
        return false;
    if (!RedoLogRecord::nextFieldOpt(ctx, r, fieldNum, fieldPos, fieldSize, 0x050102))
        return false;
    r->obj = ctx->read32(r->data(fieldPos + 0));
    r->dataObj = ctx->read32(r->data(fieldPos + 4));

    fieldPos = 0;
    fieldNum = 0;
    fieldSize = 0;
    RedoLogRecord::nextField(ctx, r, fieldNum, fieldPos, fieldSize, 0x050112);
    r->xid = Xid(static_cast<typeUsn>(ctx->read16(r->data(fieldPos + 8))), ctx->read16(r->data(fieldPos + 10)),
                 ctx->read32(r->data(fieldPos + 12)));
    if (!RedoLogRecord::nextFieldOpt(ctx, r, fieldNum, fieldPos, fieldSize, 0x050113))
        return false;
    r->obj = ctx->read32(r->data(fieldPos + 0));
    r->dataObj = ctx->read32(r->data(fieldPos + 4));
    r->opc = (static_cast<typeOp1>(*r->data(fieldPos + 16)) << 8) | *r->data(fieldPos + 17);
    r->slt = *r->data(fieldPos + 18);
    r->flg = ctx->read16(r->data(fieldPos + 20));
    g_sink += ktubStrings(ctx, r);
    if ((r->flg & 0x001C) != 0)
        return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    uint64_t n = 2000000;
    if (argc > 1)
        n = std::strtoull(argv[1], nullptr, 10);
    uint16_t cc = 8;
    if (argc > 2)
        cc = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
    const uint16_t suppCc = cc / 2;

    static Ctx ctx;
    ctx.version = RedoLogRecord::REDO_VERSION_19_0;
    ctx.dumpRedoLog = 0;

    std::printf("dumpRedoLog=%u\n", ctx.dumpRedoLog.load());

    Pair p;
    makePair(p, &ctx, cc, suppCc);

    // validate once: no exceptions, head sees the object/id
    OpCode0501::process0501(&ctx, &p.undoRec);
    OpCode0B05::process0B05(&ctx, &p.redoRec);
    if (p.undoRec.opc != 0x0B01 || p.undoRec.obj != 1234 || p.undoRec.dataObj != 5678) {
        std::fprintf(stderr, "validation failed: opc=0x%X obj=%u dataObj=%u\n", p.undoRec.opc, p.undoRec.obj, p.undoRec.dataObj);
        return 1;
    }

    // Optional synthetic dump-mode check: argv[3] == "dump".
    // Emits the redo-dump output for the same pair so a build with and without the
    // ktub guard can be diffed (validation item 2 of the plan) without real archives.
    if (argc > 3 && std::string(argv[3]) == "dump") {
        initRec(p.undoRec, p.undo, 0x0501, 1);
        initRec(p.redoRec, p.redo, 0x0B05, 2);
        ctx.dumpStream->open("/tmp/ff-dump.txt");
        if (!ctx.dumpStream->is_open()) {
            std::fprintf(stderr, "cannot open dump file\n");
            return 1;
        }
        ctx.dumpRedoLog = 1;
        OpCode0501::process0501(&ctx, &p.undoRec);
        OpCode0B05::process0B05(&ctx, &p.redoRec);
        ctx.dumpRedoLog = 0;
        ctx.dumpStream->close();
        return 0;
    }

    std::printf("record: undo %u B + redo %u B = %u B/pair, cc=%u suppCc=%u\n",
                p.undo.total, p.redo.total, p.undo.total + p.redo.total, cc, suppCc);
    std::printf("iterations: %llu\n", static_cast<unsigned long long>(n));

    // reset recs (the validation left them modified)
    initRec(p.undoRec, p.undo, 0x0501, 1);
    initRec(p.redoRec, p.redo, 0x0B05, 2);

    const double t_full_undo = bench("process0501 (undo full)", n, [&](uint64_t) {
        OpCode0501::process0501(&ctx, &p.undoRec);
        g_sink += p.undoRec.obj + p.undoRec.opc + p.undoRec.cc + p.undoRec.fieldCnt;
    });
    const double t_head_undo = bench("process0501Head (undo head)", n, [&](uint64_t) {
        g_sink += OpCode0501::process0501Head(&ctx, &p.undoRec) ? 1 : 0;
        g_sink += p.undoRec.obj + p.undoRec.opc + p.undoRec.flg;
    });
    const double t_walk = bench("walk fields (4 nextField)", n, [&](uint64_t) {
        g_sink += walkUndo(&ctx, &p.undoRec);
    });
    const double t_ktubstr = bench("ktub strings (dump only!)", n, [&](uint64_t) {
        g_sink += ktubStrings(&ctx, &p.undoRec);
    });
    const double t_head_copy = bench("headCopy (replica)", n, [&](uint64_t) {
        g_sink += headCopy(&ctx, &p.undoRec) ? 1 : 0;
        g_sink += p.undoRec.obj + p.undoRec.opc + p.undoRec.flg;
    });
    const double t_init = bench("  init", n, [&](uint64_t) {
        OpCode0501::init(&ctx, &p.undoRec);
        g_sink += p.undoRec.obj;
    });
    const double t_ktudb = bench("  ktudb", n, [&](uint64_t) {
        typePos fp = 0;
        typeField fn = 0;
        typeSize fs = 0;
        RedoLogRecord::nextField(&ctx, &p.undoRec, fn, fp, fs, 0x050112);
        OpCode0501::ktudb(&ctx, &p.undoRec, fp, fs);
        g_sink += p.undoRec.xid.getData();
    });
    const double t_ktub = bench("  ktub", n, [&](uint64_t) {
        typePos fp = p.undoRec.fieldPos + 24; // field 2 (ktub)
        typeSize fs = 40;
        OpCode0501::ktub(&ctx, &p.undoRec, fp, fs, true);
        g_sink += p.undoRec.obj + p.undoRec.flg;
    });
    const double t_full_redo = bench("process0B05 (redo full)", n, [&](uint64_t) {
        OpCode0B05::process0B05(&ctx, &p.redoRec);
        g_sink += p.redoRec.obj + p.redoRec.op + p.redoRec.cc + p.redoRec.fieldCnt;
    });
    const double t_peek = bench("peekHeader (redo header)", n, [&](uint64_t) {
        peekHeader(&ctx, p.redo, p.peekRec);
        g_sink += p.peekRec.opCode + p.peekRec.size + p.peekRec.fieldCnt;
    });

    const double t_off = t_full_undo + t_full_redo;
    const double t_on_untracked = t_head_undo + t_peek;

    std::printf("\nbody/head split: undo full=%.1f head=%.1f headCopy=%.1f walk=%.1f ktubStrings=%.1f body=%.1f\n",
                t_full_undo, t_head_undo, t_head_copy, t_walk, t_ktubstr, t_full_undo - t_head_undo);
    std::printf("head parts: init=%.1f ktudb=%.1f ktub=%.1f\n", t_init, t_ktudb, t_ktub);
    std::printf("per pair, off        : %.1f ns (undo full + redo full)\n", t_off);
    std::printf("per pair, on untracked: %.1f ns (undo head + redo peek)\n", t_on_untracked);
    std::printf("saved work per untracked pair: %.1f ns (%.1f%% of off)\n",
                t_off - t_on_untracked, 100.0 * (t_off - t_on_untracked) / t_off);

    for (double u : {0.90, 0.95}) {
        const double t_on = (1.0 - u) * t_off + u * t_on_untracked;
        std::printf("untracked=%.0f%%: on=%.1f ns/pair, speedup=%.2fx\n", u * 100.0, t_on, t_off / t_on);
    }
    return 0;
}
