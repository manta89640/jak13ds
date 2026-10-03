#pragma once

/*!
 * @file sce_memfile.h
 * (AI-assisted) 3DS: a GOAL file stream held in memory (sif_ee.cpp). Reads, writes and seeks
 * behave like fread / fwrite / fseek + ftell on the file it stands for (seeking past the end is
 * fine, a write there fills the gap with zeros, seeking before the start fails and leaves the
 * position). Host test: platform/3ds/tests/memfile_test.cpp.
 */

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/common_types.h"

struct SceMemFile {
  std::vector<u8> data;
  size_t pos = 0;
  bool writable = false;  // opened for writing (reads return 0), else for reading (writes do)

  s32 read(void* buf, s32 nbyte) {
    if (writable || nbyte <= 0 || pos >= data.size()) {
      return 0;
    }
    const size_t n = std::min((size_t)nbyte, data.size() - pos);
    memcpy(buf, data.data() + pos, n);
    pos += n;
    return (s32)n;
  }

  s32 write(const void* buf, s32 nbyte) {
    if (!writable || nbyte <= 0) {
      return 0;
    }
    if (pos + (size_t)nbyte > data.size()) {
      data.resize(pos + (size_t)nbyte);
    }
    memcpy(data.data() + pos, buf, (size_t)nbyte);
    pos += (size_t)nbyte;
    return nbyte;
  }

  // whence: 0 set, 1 current, 2 end (SCE_SEEK_*, SEEK_*). Returns the position, or -1.
  s32 seek(s32 offset, int whence) {
    s64 base = 0;
    if (whence == 1) {
      base = (s64)pos;
    } else if (whence == 2) {
      base = (s64)data.size();
    } else if (whence != 0) {
      return -1;
    }
    if (base + offset >= 0) {
      pos = (size_t)(base + offset);
    }
    return (s32)pos;
  }
};
