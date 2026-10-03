/*!
 * @file stream.cpp
 * OVERLORD streaming driver.
 * Supports loading a file directly to the EE, or loading chunks of a chunked file.
 */

#include "stream.h"

#include <cstring>

#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/common/play_rpc_types.h"
#include "game/common/str_rpc_types.h"
#include "game/overlord/common/iso.h"
#include "game/overlord/common/isocommon.h"
#include "game/overlord/jak1/iso.h"
#include "game/overlord/jak1/iso_api.h"
#include "game/overlord/jak1/srpc.h"
#include "game/runtime.h"
#include "game/sce/iop.h"

#ifdef __3DS__
#include <cstdlib>
#include <vector>

#include "game/overlord/common/dma.h"
#include "game/overlord/common/fake_iso.h"
#include "game/overlord/jak1/fake_iso.h"
#endif

using namespace iop;

namespace jak1 {
constexpr int kStrBufSize = sizeof(RPC_Str_Cmd_Jak1);
static RPC_Str_Cmd_Jak1 sSTRBuf;
constexpr int kPlayBufSize = 2 * sizeof(RPC_Play_Cmd_Jak1);
static RPC_Play_Cmd_Jak1 sPLAYBuf[2];

void* RPC_STR(unsigned int fno, void* _cmd, int y);
void* RPC_PLAY(unsigned int fno, void* _cmd, int y);

static constexpr int PLAY_MSG_SIZE = 0x40;

static u32 global_vag_count = 0;

/*!
 * We cache the chunk file headers so we can avoid seeking to the chunk header each time we
 * need to load another chunk, even if we load chunks out of order.
 */
struct CacheEntry {
  // the record for the chunk file described.
  FileRecord* fr = nullptr;
  // counts down from INT32_MAX - 1 each time we have a cache miss.
  s32 countdown = 0;
  // the actual cached data.
  StrFileHeaderJ1 header;
};

// the actual header cache.
#ifdef __3DS__
// (AI-assisted) 16 on the 3DS (8 KB): a level's power cells and characters preload more spools
// than 4, and every miss is a load (a card read and a block to decode) of its own
constexpr int STR_INDEX_CACHE_SIZE = 16;
#else
constexpr int STR_INDEX_CACHE_SIZE = 4;
#endif
CacheEntry sCacheJ1[STR_INDEX_CACHE_SIZE];

#ifdef __3DS__
namespace {
// (AI-assisted) Chunk 0 of short chunked files (spooled animations: a power cell's victory,
// characters' ambient animations), the bytes the game got. The game preloads chunk 0 of the
// spools near the player again each time a level's display toggles, and every preload opened,
// read and decoded the file again (244-317 ms of IOP time in one frame on the beach). Bounded by
// bytes, least recently used out first; nothing is added during a level load (the heap is at its
// fullest then: LoadDGO empties the cache) or while the heap is short.
constexpr u32 kChunkCacheBytes = 1024 * 1024;
constexpr u32 kChunkCacheMaxFile = 512 * 1024;       // "short": the whole (decoded) file
constexpr u32 kChunkCacheHeapMin = 8 * 1024 * 1024;  // free heap needed to add a chunk
struct ChunkEntry {
  FileRecord* fr;
  u8* data;
  u32 len;
  u32 last_use;
};
std::vector<ChunkEntry> sChunkCache;
u32 sChunkCacheBytes = 0;
u32 sChunkCacheClock = 0;

ChunkEntry* chunk_cache_find(FileRecord* fr) {
  for (auto& e : sChunkCache) {
    if (e.fr == fr) {
      e.last_use = ++sChunkCacheClock;
      return &e;
    }
  }
  return nullptr;
}

void chunk_cache_drop(size_t i) {
  sChunkCacheBytes -= sChunkCache[i].len;
  free(sChunkCache[i].data);
  sChunkCache.erase(sChunkCache.begin() + i);
}

// should chunk 0 of this file (len bytes) be kept when it is loaded?
bool chunk_cache_wants(FileRecord* fr, u32 len) {
  return io_cache_on() && len && len <= kChunkCacheBytes / 4 && sLoadDGO.cmd_id == 0 &&
         isofs->get_length(fr) <= kChunkCacheMaxFile && io_heap_free() >= kChunkCacheHeapMin + len;
}

// takes data (malloc'd, len bytes)
void chunk_cache_add(FileRecord* fr, u8* data, u32 len) {
  while (!sChunkCache.empty() && sChunkCacheBytes + len > kChunkCacheBytes) {
    size_t oldest = 0;
    for (size_t i = 1; i < sChunkCache.size(); i++) {
      if (sChunkCache[i].last_use < sChunkCache[oldest].last_use) {
        oldest = i;
      }
    }
    chunk_cache_drop(oldest);
  }
  sChunkCache.push_back({fr, data, len, ++sChunkCacheClock});
  sChunkCacheBytes += len;
}
}  // namespace

void stream_chunk_cache_clear() {
  while (!sChunkCache.empty()) {
    chunk_cache_drop(sChunkCache.size() - 1);
  }
}
#endif

void stream_init_globals() {
  memset(&sSTRBuf, 0, sizeof(RPC_Str_Cmd_Jak1));
  memset(&sPLAYBuf, 0, sizeof(RPC_Play_Cmd_Jak1) * 2);
}

/*!
 * Run the STR RPC handler.
 */
u32 STRThread() {
  sceSifQueueData dq;
  sceSifServeData serve;

  CpuDisableIntr();
  sceSifInitRpc(0);
  sceSifSetRpcQueue(&dq, GetThreadId());
  sceSifRegisterRpc(&serve, STR_RPC_ID[g_game_version], RPC_STR, &sSTRBuf, kStrBufSize, nullptr,
                    nullptr, &dq);
  CpuEnableIntr();
  sceSifRpcLoop(&dq);
  return 0;
}

u32 PLAYThread() {
  sceSifQueueData dq;
  sceSifServeData serve;

  CpuDisableIntr();
  sceSifInitRpc(0);
  sceSifSetRpcQueue(&dq, GetThreadId());
  sceSifRegisterRpc(&serve, PLAY_RPC_ID[g_game_version], RPC_PLAY, sPLAYBuf, kPlayBufSize, nullptr,
                    nullptr, &dq);
  CpuEnableIntr();
  sceSifRpcLoop(&dq);
  return 0;
}

/*!
 * The STR RPC handler.
 */
void* RPC_STR(unsigned int fno, void* _cmd, int y) {
  (void)fno;
  (void)y;
  auto* cmd = (RPC_Str_Cmd_Jak1*)_cmd;
  if (cmd->chunk_id < 0) {
    // it's _not_ a stream file. So we just treat it like a normal load.

    // find the file with the given name
    auto file_record = isofs->find(cmd->name);
    if (file_record == nullptr) {
      // file not found!
      printf("[OVERLORD STR] Failed to find file %s for loading.\n", cmd->name);
      cmd->result = STR_RPC_RESULT_ERROR;
    } else {
      // load directly to the EE
      cmd->length = LoadISOFileToEE(file_record, cmd->ee_addr, cmd->length);
      if (cmd->length) {
        // successful load!
        cmd->result = STR_RPC_RESULT_DONE;
      } else {
        // there was an error loading.
        cmd->result = STR_RPC_RESULT_ERROR;
      }
    }
  } else {
    // it's a chunked file. These are only animations - these have a separate naming scheme.
    char animation_iso_name[128];
    file_util::ISONameFromAnimationName(animation_iso_name, cmd->name);
    auto file_record = isofs->find_in(animation_iso_name);

    if (!file_record) {
      // didn't find the file
      printf("[OVERLORD STR] Failed to find animation %s\n", cmd->name);
      cmd->result = STR_RPC_RESULT_ERROR;
    } else {
#ifdef __3DS__
      // (AI-assisted) chunk 0 of a short file loaded before: from the cache, no load at all
      if (cmd->chunk_id == 0) {
        if (ChunkEntry* e = chunk_cache_find(file_record)) {
          DMA_SendToEE(e->data, e->len, (void*)(uintptr_t)cmd->ee_addr);
          cmd->length = e->len;
          cmd->result = 0;
          return cmd;
        }
      }
#endif
      // found it! See if we've cached this animation's header.
      int cache_entry = 0;
      int oldest = INT32_MAX;
      int oldest_idx = -1;
      while (cache_entry < STR_INDEX_CACHE_SIZE && sCacheJ1[cache_entry].fr != file_record) {
        sCacheJ1[cache_entry].countdown--;
        if (sCacheJ1[cache_entry].countdown < oldest) {
          oldest_idx = cache_entry;
          oldest = sCacheJ1[cache_entry].countdown;
        }
        cache_entry++;
      }

      if (cache_entry == STR_INDEX_CACHE_SIZE) {
        // cache miss, we need to load the header to the header cache on the IOP
        cache_entry = oldest_idx;
        sCacheJ1[oldest_idx].fr = file_record;
        sCacheJ1[oldest_idx].countdown = INT32_MAX - 1;
        if (!LoadISOFileToIOP(file_record, &sCacheJ1[oldest_idx].header, sizeof(StrFileHeaderJ1))) {
          printf("[OVERLORD STR] Failed to load chunk file header for animation %s\n", cmd->name);
          cmd->result = 1;
          return cmd;
        }
      }

      // load data, using the cached header to find the location of the chunk.
#ifdef __3DS__
      // (AI-assisted) chunk 0 of a short file: keep a copy (chunk cache above)
      const u32 chunk_len = sCacheJ1[cache_entry].header.sizes[cmd->chunk_id];
      u8* capture = nullptr;
      if (cmd->chunk_id == 0 && chunk_cache_wants(file_record, chunk_len)) {
        capture = (u8*)malloc(chunk_len);
      }
      const s32 loaded =
          LoadISOFileChunkToEE(file_record, cmd->ee_addr, chunk_len,
                               sCacheJ1[cache_entry].header.sectors[cmd->chunk_id], capture);
      if (capture) {
        if (loaded == (s32)chunk_len) {
          chunk_cache_add(file_record, capture, chunk_len);
        } else {
          free(capture);
        }
      }
      if (!loaded) {
#else
      if (!LoadISOFileChunkToEE(file_record, cmd->ee_addr,
                                sCacheJ1[cache_entry].header.sizes[cmd->chunk_id],
                                sCacheJ1[cache_entry].header.sectors[cmd->chunk_id])) {
#endif
        printf("[OVERLORD STR] Failed to load chunk %d for animation %s\n", cmd->chunk_id,
               cmd->name);
        cmd->result = 1;
      } else {
        // successful load!
        cmd->length = sCacheJ1[cache_entry].header.sizes[cmd->chunk_id];
        cmd->result = 0;
      }
    }
  }

  return cmd;
}

void* RPC_PLAY([[maybe_unused]] unsigned int fno, void* _cmd, int size) {
  s32 n_messages = size / PLAY_MSG_SIZE;
  char namebuf[16];

  auto* cmd = (RPC_Play_Cmd_Jak1*)(_cmd);
  while (n_messages > 0) {
    if (cmd->name[0] == '$') {
      char* name_part = &cmd->name[1];
      size_t name_len = strlen(name_part);

      if (name_len < 9) {
        memset(namebuf, ' ', 8);
        memcpy(namebuf, name_part, name_len);
      } else {
        memcpy(namebuf, name_part, 8);
      }

      // ASCII toupper
      for (int i = 0; i < 8; i++) {
        if (namebuf[i] >= 0x61 && namebuf[i] < 0x7b) {
          namebuf[i] -= 0x20;
        }
      }
    } else {
      file_util::ISONameFromAnimationName(namebuf, cmd->name);
    }

    auto vag = FindVAGFile(namebuf);
    memcpy(namebuf, "VAGWAD  ", 8);
    strcpy(&namebuf[8], gLanguage);

    FileRecord* file = nullptr;

    global_vag_count = (global_vag_count + 1) & 0x3f;
    if (!cmd->result && global_vag_count == 0) {
      namebuf[0] -= 3;
#ifdef __3DS__
      // (AI-assisted) the original looks for "SAGWAD" every 64th play and drops the answer: the
      // file never exists, and the error line for it was an SD card write
      file = FS_FindINQuiet(namebuf);
#else
      file = isofs->find_in(namebuf);
#endif
      namebuf[0] += 3;
    }

    file = isofs->find_in(namebuf);

    if (cmd->result == 0) {
      PlayVAGStream(file, vag, cmd->address, 0x400, 1, nullptr);
    } else if (cmd->result == 1) {
      StopVAGStream(vag, 1);
    } else {
      QueueVAGStream(file, vag, 0, 1);
    }

    n_messages--;
    cmd++;
  }

  return _cmd;
}

}  // namespace jak1
