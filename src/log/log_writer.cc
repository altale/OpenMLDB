// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "log/log_writer.h"

#include <stdint.h>
#include "log/coding.h"
#include "log/crc32c.h"
#include "base/glog_wapper.h" // NOLINT
#include "gflags/gflags.h"
#ifdef PZFPGA_ENABLE
#include "pz.h" // NOLINT
#endif

DECLARE_bool(compress_snapshot);

namespace rtidb {
namespace log {

#ifdef PZFPGA_ENABLE
    static FPGA_env * fpga_ctx_;
#endif

static void InitTypeCrc(uint32_t* type_crc) {
    for (int i = 0; i <= kMaxRecordType; i++) {
        char t = static_cast<char>(i);
        type_crc[i] = Value(&t, 1);
    }
}

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
    InitTypeCrc(type_crc_);
#ifdef PZFPGA_ENABLE
          if (FLAGS_compress_snapshot) {
              fpga_ctx_ = gzipfpga_init_titanse();
              buffer_ = new char[kBlockSize];
          }
#endif
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(dest_length % kBlockSize) {
    InitTypeCrc(type_crc_);
#ifdef PZFPGA_ENABLE
          if (FLAGS_compress_snapshot) {
              fpga_ctx_ = gzipfpga_init_titanse();
              buffer_ = new char[kBlockSize];
          }
#endif
}

Writer::~Writer() {
#ifdef PZFPGA_ENABLE
    if (FLAGS_compress_snapshot && fpga_ctx_) {
        gzipfpga_end(fpga_ctx_);
        delete[] buffer_;
    }
#endif
}

Status Writer::EndLog() {
    Slice slice;
    const char* ptr = slice.data();
    size_t left = 0;
    Status s;
    do {
        const int leftover = kBlockSize - block_offset_;
        assert(leftover >= 0);
        if (leftover < kHeaderSize) {
            // Switch to a new block
            if (leftover > 0) {
                // Fill the trailer (literal below relies on kHeaderSize being
                // 7)
                assert(kHeaderSize == 7);
                Slice fill_slice("\x00\x00\x00\x00\x00\x00", leftover);
#ifdef PZFPGA_ENABLE
                if (!FLAGS_compress_snapshot) {
#endif
                    dest_->Append(fill_slice);
#ifdef PZFPGA_ENABLE
                } else {
                    memcpy(buffer_ + block_offset_, fill_slice.data(), leftover);
                    CompressRecord();
                }
#endif
            }
            block_offset_ = 0;
        }
        // Invariant: we never leave < kHeaderSize bytes in a block.
        assert(kBlockSize - block_offset_ - kHeaderSize >= 0);
        const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
        const size_t fragment_length = (left < avail) ? left : avail;
        RecordType type = kEofType;
        PDLOG(INFO, "end log");
        s = EmitPhysicalRecord(type, ptr, fragment_length);
        ptr += fragment_length;
        left -= fragment_length;
    } while (s.ok() && left > 0);
    return s;
}

Status Writer::AddRecord(const Slice& slice) {
    const char* ptr = slice.data();
    size_t left = slice.size();

    // Fragment the record if necessary and emit it.  Note that if slice
    // is empty, we still want to iterate once to emit a single
    // zero-length record
    Status s;
    bool begin = true;
    do {
        const int leftover = kBlockSize - block_offset_;
        assert(leftover >= 0);
        if (leftover < kHeaderSize) {
            // Switch to a new block
            if (leftover > 0) {
                // Fill the trailer (literal below relies on kHeaderSize being
                // 7)
                assert(kHeaderSize == 7);
                Slice fill_slice("\x00\x00\x00\x00\x00\x00", leftover);
#ifdef PZFPGA_ENABLE
                if (!FLAGS_compress_snapshot) {
#endif
                    dest_->Append(fill_slice);
#ifdef PZFPGA_ENABLE
                } else {
                    memcpy(buffer_ + block_offset_, fill_slice.data(), leftover);
                    CompressRecord();
                }
#endif
            }
            block_offset_ = 0;
        }
        // Invariant: we never leave < kHeaderSize bytes in a block.
        assert(kBlockSize - block_offset_ - kHeaderSize >= 0);

        const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
        const size_t fragment_length = (left < avail) ? left : avail;

        RecordType type;
        const bool end = (left == fragment_length);
        if (begin && end) {
            type = kFullType;
        } else if (begin) {
            type = kFirstType;
        } else if (end) {
            type = kLastType;
        } else {
            type = kMiddleType;
        }
        s = EmitPhysicalRecord(type, ptr, fragment_length);
        ptr += fragment_length;
        left -= fragment_length;
        begin = false;
    } while (s.ok() && left > 0);
    return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr, size_t n) {
    assert(n <= 0xffff);  // Must fit in two bytes
    assert(block_offset_ + kHeaderSize + n <= kBlockSize);
    // Format the header
    char buf[kHeaderSize];
    buf[4] = static_cast<char>(n & 0xff);
    buf[5] = static_cast<char>(n >> 8);
    buf[6] = static_cast<char>(t);

    // Compute the crc of the record type and the payload.
    uint32_t crc = Extend(type_crc_[t], ptr, n);
    crc = Mask(crc);  // Adjust for storage
    EncodeFixed32(buf, crc);

#ifdef PZFPGA_ENABLE
    if (!FLAGS_compress_snapshot) {
#endif
        // Write the header and the payload
        Status s = dest_->Append(Slice(buf, kHeaderSize));
        if (s.ok()) {
            s = dest_->Append(Slice(ptr, n));
            if (s.ok()) {
                s = dest_->Flush();
            }
        }
        if (!s.ok()) {
            PDLOG(WARNING, "write error. %s", s.ToString().c_str());
        }

        block_offset_ += kHeaderSize + n;
        return s;
#ifdef PZFPGA_ENABLE
    } else {
        memcpy(buffer_ + block_offset_, &buf, kHeaderSize);
        memcpy(buffer_ + block_offset_ + kHeaderSize, ptr, n);
        block_offset_ += kHeaderSize + n;
        // fill the trailer if kEofType
        if (t == kEofType) {
            memset(buffer_ + block_offset_, 0, kBlockSize - block_offset_);
            block_offset_ = kBlockSize;
        }
        if (block_offset_ == kBlockSize) {
            return CompressRecord();
        }
        return Status::OK();
    }
#endif
}

#ifdef PZFPGA_ENABLE
Status Writer::CompressRecord() {
    Status s;
    // compress
    char compress_record[kBlockSize];
    int compress_len = gzipfpga_compress_nohuff(
            fpga_ctx_, buffer_, compress_record, kBlockSize, kBlockSize, 0);
    if (compress_len < 0)  {
        s = Status::InvalidRecord(Slice("compress failed"));
        PDLOG(WARNING, "write error. %s", s.ToString().c_str());
        return s;
    }
    // fill compressed record's header
    char compress_header[64];
    memcpy(compress_header, &compress_len, sizeof(int));
    // memcpy(compress_header + 8, &kBlockSize, sizeof(int));
    // memset(compress_header + 8, 0, 56);
    memset(compress_header + sizeof(int), 0, 64 - sizeof(int));
    // write header and compressed data
    s = dest_->Append(Slice(compress_header, 64));
    if (s.ok()) {
        s = dest_->Append(Slice(compress_record, compress_len));
        if (s.ok()) {
            s = dest_->Flush();
        }
    }
    if (!s.ok()) {
        PDLOG(WARNING, "write error. %s", s.ToString().c_str());
    }
    return s;
}
#endif

}  // namespace log
}  // namespace rtidb
