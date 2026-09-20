/* Header for Parser class
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

#ifndef PARSER_H_
#define PARSER_H_

#include <cstddef>

#include "../common/Ctx.h"
#include "../common/RedoLogRecord.h"
#include "../reader/Reader.h"
#include "../common/types/Time.h"
#include "../common/types/Types.h"
#include "../common/types/Xid.h"

namespace OpenLogReplicator {
    class Builder;
    class Metadata;
    class Transaction;
    class TransactionBuffer;
    class XmlCtx;

    struct LwnMember {
        uint16_t pageOffset;
        Scn scn;
        uint32_t size;
        typeBlk block;
        typeSubScn subScn;

        bool operator<(const LwnMember& other) const {
            if (scn < other.scn)
                return true;
            if (other.scn < scn)
                return false;
            if (subScn < other.subScn)
                return true;
            if (other.subScn < subScn)
                return false;
            if (block < other.block)
                return true;
            if (block > other.block)
                return false;
            return (pageOffset < other.pageOffset);
        }
    };

    class Parser final {
    protected:
        static constexpr uint64_t MAX_LWN_CHUNKS = static_cast<uint64_t>(512 * 2) / Ctx::MEMORY_CHUNK_SIZE_MB;
        static constexpr uint64_t MAX_RECORDS_IN_LWN = 1048576;

        Ctx* ctx;
        Builder* builder;
        Metadata* metadata;
        TransactionBuffer* transactionBuffer;
        RedoLogRecord zero;
        Transaction* lastTransaction{nullptr};
        bool fastFilter{false};
        uint64_t skippedPairs{0};
        uint64_t skippedIndexPairs{0};
        // Why an undo vector was not skipped (TRACE::PERFORMANCE diagnostics)
        uint64_t keptUserTable{0};    // row DML on a configured (user) table
        uint64_t keptDictTable{0};    // row DML on a dictionary table OLR tracks for schema maintenance
        uint64_t keptLobIndex{0};     // index pair on a tracked LOB index
        uint64_t keptLobUndo{0};      // opc 0x1A01, never decided from the head
        uint64_t keptNoPartner{0};    // eligible undo without a matching redo vector next to it
        uint64_t keptOtherOpc{0};     // other undo kinds
        // Ignored data errors (IGNORE_DATA_ERRORS), counted per error code and object so the
        // same broken record shape does not flood the log; see reportIgnoredError()
        std::unordered_map<std::string, uint64_t> ignoredErrors;
        void reportIgnoredError(int code, const std::string& msg);

        uint8_t* lwnChunks[MAX_LWN_CHUNKS]{};
        LwnMember* lwnMembers[MAX_RECORDS_IN_LWN + 1]{};
        uint64_t lwnAllocated{0};
        uint64_t lwnAllocatedMax{0};
        Time lwnTimestamp{0};
        Scn lwnScn;
        typeBlk lwnCheckpointBlock{0};

        void freeLwn();
        void analyzeLwn(LwnMember* lwnMember);
        void appendToTransactionDdl(RedoLogRecord* redoLogRecord1);
        void appendToTransactionBegin(RedoLogRecord* redoLogRecord1);
        void appendToTransactionCommit(RedoLogRecord* redoLogRecord1);
        void appendToTransactionLob(RedoLogRecord* redoLogRecord1);
        void appendToTransactionIndex(RedoLogRecord* redoLogRecord1, RedoLogRecord* redoLogRecord2);
        void appendToTransaction(RedoLogRecord* redoLogRecord1);
        void appendToTransactionRollback(RedoLogRecord* redoLogRecord1);
        void appendToTransaction(RedoLogRecord* redoLogRecord1, RedoLogRecord* redoLogRecord2);
        void appendToTransactionRollback(RedoLogRecord* redoLogRecord1, RedoLogRecord* redoLogRecord2);
        void dumpRedoVector(const uint8_t* data, typeSize recordSize) const;
        void parseVectorHeader(const uint8_t* data, uint32_t recordSize, uint32_t offset, const LwnMember* lwnMember,
                               uint64_t vectorNo, RedoLogRecord* out) const;
        bool trySkipUntrackedPair(const uint8_t* data, uint32_t recordSize, uint32_t& offset, const LwnMember* lwnMember,
                                  uint64_t vectorNo, RedoLogRecord* redoLogRecord1);

    public:
        int group;
        std::string path;
        Seq sequence;
        Scn firstScn{Scn::none()};
        Scn nextScn{Scn::none()};
        Reader* reader{nullptr};

        Parser(Ctx* newCtx, Builder* newBuilder, Metadata* newMetadata, TransactionBuffer* newTransactionBuffer, int newGroup, std::string newPath);
        ~Parser();

        Reader::REDO_CODE parse();
        [[nodiscard]] std::string toString() const;
    };
}

#endif
