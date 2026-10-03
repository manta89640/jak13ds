#include "sif_ee.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/runtime.h"
#include "game/system/iop_thread.h"

#ifdef __3DS__
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "common/log/log.h"

#include "game/sce/sce_memfile.h"

#include "platform/3ds/port/ctr_port.h"
#endif

namespace ee {

namespace {
::IOP* iop;
#ifndef __3DS__
std::unordered_map<s32, FILE*> sce_fds;
#else
// (AI-assisted) 3DS: the GOAL file streams (kopen / kread / kwrite, FileLoad) in memory. A file
// opened for reading is read whole at open (up to kMemFileMax; bigger ones keep the FILE path) and
// reads and seeks are served from memory: the pc-settings parser reads 1 KB and seeks back for
// every token, ~500 SD card requests that took ~3.5 s of the boot on hardware. A file opened for
// writing is collected in memory and written in one go by the log writer thread at close
// (ctr_bg_run), not at all when the file already holds the same (as read or last written): the
// autosave wrote pc-settings.gc with ~100 small writes on the game thread, a 100-240 ms hitch.
constexpr size_t kMemFileMax = 4u << 20;

struct SceFile {
  FILE* fp = nullptr;  // too big to keep in memory, or appending: the FILE path
  SceMemFile mem;      // else this (mem.writable: written to `path` at close)
  std::string path;
};
std::unordered_map<s32, std::unique_ptr<SceFile>> sce_files;
s32 sce_next_fd = 1;

// what a file holds, as read or written by these streams (the writer thread finishes the writes)
struct KnownFile {
  u64 hash = 0;
  size_t size = 0;
  std::shared_ptr<const std::vector<u8>> pending;  // queued to be written: reads get this
};
std::mutex g_known_mutex;
std::unordered_map<std::string, KnownFile> g_known;

u64 content_hash(const u8* p, size_t n) {
  u64 h = 1469598103934665603ull;  // FNV-1a
  for (size_t i = 0; i < n; i++) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h;
}

struct WriteJob {
  std::string path;
  std::shared_ptr<const std::vector<u8>> data;
};

// on the log writer thread (or right away without it)
void write_job(void* arg) {
  std::unique_ptr<WriteJob> job((WriteJob*)arg);
  std::error_code ec;
  fs::create_directories(fs::path(job->path).parent_path(), ec);
  FILE* fp = file_util::open_file(job->path.c_str(), "wb");
  bool ok = fp != nullptr;
  if (fp) {
    setvbuf(fp, nullptr, _IONBF, 0);  // one SD card write
    const auto& d = *job->data;
    ok = d.empty() || fwrite(d.data(), 1, d.size(), fp) == d.size();
    ok = fclose(fp) == 0 && ok;
  }
  {
    std::lock_guard<std::mutex> lock(g_known_mutex);
    auto it = g_known.find(job->path);
    if (it != g_known.end() && it->second.pending == job->data) {
      it->second.pending.reset();
      if (!ok) {
        g_known.erase(it);  // not known now: the next write of it goes to the card
      }
    }
  }
  if (!ok) {
    lg::error("[sce] writing {} failed", job->path);
  }
}
#endif
}  // namespace

void LIBRARY_sceSif_register(::IOP* i) {
  iop = i;
}

void LIBRARY_INIT_sceSif() {
  iop = nullptr;
#ifndef __3DS__
  for (auto& kv : sce_fds) {
    fclose(kv.second);
  }
  sce_fds.clear();
#else
  // closed as fclose would: what was written goes to the file
  std::vector<s32> open;
  for (auto& kv : sce_files) {
    open.push_back(kv.first);
  }
  for (s32 fd : open) {
    sceClose(fd);
  }
#endif
}
void sceSifInitRpc(unsigned int mode) {
  (void)mode;
}

int sceSifRebootIop(const char* imgfile) {
  (void)imgfile;
  return 1;
}

int sceSifSyncIop() {
  return 1;
}

void sceFsReset() {}

int sceSifLoadModule(const char* name, int arg_size, const char* args) {
  if (!strcmp(name, "cdrom0:\\\\DRIVERS\\\\OVERLORD.IRX;1") ||
      !strcmp(name, "host0:binee/overlord.irx") || !strcmp(name, "host0:bin/overlord.irx")) {
    const char* src = args;
    char* dst = iop->overlord_arg_data;
    int cnt;
    iop->overlord_argv[0] = nullptr;
    for (cnt = 1; src - args < arg_size; cnt++) {
      auto len = strlen(src);
      memcpy(dst, src, len + 1);
      iop->overlord_argv[cnt] = dst;
      dst += len + 1;
      src += len + 1;
    }
    iop->overlord_argc = cnt;

    for (int i = 0; i < cnt; i++) {
      if (iop->overlord_argv[i])
        printf("arg %d : %s\n", i, iop->overlord_argv[i]);
    }
    iop->set_ee_main_mem(g_ee_main_mem);
    iop->send_status(IOP_Status::IOP_OVERLORD_INIT);
    iop->wait_for_overlord_init_finish();
  }

  return 1;
}

s32 sceSifCallRpc(sceSifClientData* bd,
                  u32 fno,
                  u32 mode,
                  void* send,
                  s32 ssize,
                  void* recv,
                  s32 rsize,
                  void* end_func,
                  void* end_para) {
  ASSERT(!end_func);
  ASSERT(!end_para);
  ASSERT(mode == 1);  // async
  iop->kernel.sif_rpc(bd->rpcd.id, fno, mode, send, ssize, recv, rsize);
  iop->signal_run_iop();
  return 0;
}

s32 sceSifCheckStatRpc(sceSifRpcData* bd) {
  iop->signal_run_iop();
  return iop->kernel.sif_busy(bd->id);
}

s32 sceSifBindRpc(sceSifClientData* bd, u32 request, u32 mode) {
  ASSERT(mode == 1);  // async
  bd->rpcd.id = request;
  bd->serve = (sceSifServeData*)1;
  return 0;
}

#ifndef __3DS__
s32 sceOpen(const char* filename, s32 flag) {
  FILE* fp = nullptr;
  auto name = file_util::get_file_path({filename});
  switch (flag) {
    case SCE_RDONLY: {
      fp = file_util::open_file(name.c_str(), "rb");
    } break;

    default: {
      // either append or truncate
      file_util::create_dir_if_needed_for_file(name);
      if (flag & SCE_TRUNC) {
        fp = file_util::open_file(name.c_str(), "w");
      } else {
        fp = file_util::open_file(name.c_str(), "a+");
      }
    } break;
  }
  if (!fp) {
    printf("[SCE] sceOpen(%s) failed.\n", name.c_str());
    return -1;
  }

  s32 fp_idx = sce_fds.size() + 1;
  sce_fds[fp_idx] = fp;
  return fp_idx;
}
#endif

s32 sceMkDir(const char* filename, s32 flag) {
  return -1;
}

#ifndef __3DS__
s32 sceClose(s32 fd) {
  if (fd < 0) {
    // todo, what should we really return?
    return 0;
  }

  auto kv = sce_fds.find(fd);
  if (kv != sce_fds.end()) {
    fclose(kv->second);
    sce_fds.erase(fd);
    return 0;
  } else {
    printf("[SCE] sceClose called on invalid fd\n");
    return 0;
  }
}

s32 sceRead(s32 fd, void* buf, s32 nbyte) {
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    return -1;
  } else {
    return fread(buf, 1, nbyte, kv->second);
  }
}

s32 sceWrite(s32 fd, const void* buf, s32 nbyte) {
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    ASSERT(false);
    return -1;
  } else {
    return fwrite(buf, 1, nbyte, kv->second);
  }
}

s32 sceLseek(s32 fd, s32 offset, s32 where) {
  auto kv = sce_fds.find(fd);
  if (kv == sce_fds.end()) {
    return -1;
  } else {
    switch (where) {
      case SCE_SEEK_CUR:
        fseek(kv->second, offset, SEEK_CUR);
        return ftell(kv->second);
      case SCE_SEEK_END:
        fseek(kv->second, offset, SEEK_END);
        return ftell(kv->second);
      case SCE_SEEK_SET:
        fseek(kv->second, offset, SEEK_SET);
        return ftell(kv->second);
      default:
        ASSERT(false);
        return -1;
    }
  }
}
#else
// (AI-assisted) 3DS: memory backed, see SceFile
s32 sceOpen(const char* filename, s32 flag) {
  auto name = file_util::get_file_path({filename});
  auto f = std::make_unique<SceFile>();
  if (flag == SCE_RDONLY) {
    std::shared_ptr<const std::vector<u8>> pending;
    {
      std::lock_guard<std::mutex> lock(g_known_mutex);
      auto it = g_known.find(name);
      if (it != g_known.end()) {
        pending = it->second.pending;
      }
    }
    if (pending) {
      f->mem.data = *pending;  // still to be written: what it will hold
    } else {
      FILE* fp = file_util::open_file(name.c_str(), "rb");
      if (!fp) {
        printf("[SCE] sceOpen(%s) failed.\n", name.c_str());
        return -1;
      }
      struct stat st;
      bool whole = fstat(fileno(fp), &st) == 0 && st.st_size >= 0 &&
                   (unsigned long long)st.st_size <= kMemFileMax;
      if (whole) {
        try {
          f->mem.data.resize((size_t)st.st_size);
        } catch (const std::bad_alloc&) {
          whole = false;
        }
      }
      if (whole) {
        auto& data = f->mem.data;
        setvbuf(fp, nullptr, _IONBF, 0);  // one read, straight into data
        const size_t got = data.empty() ? 0 : fread(data.data(), 1, data.size(), fp);
        fclose(fp);
        data.resize(got);
        std::lock_guard<std::mutex> lock(g_known_mutex);
        auto& known = g_known[name];
        known.hash = content_hash(data.data(), data.size());
        known.size = data.size();
      } else {
        f->fp = fp;
      }
    }
  } else if (flag & SCE_TRUNC) {
    f->mem.writable = true;
    f->path = name;
  } else {
    // appending (not done by Jak 1): the FILE path
    file_util::create_dir_if_needed_for_file(name);
    f->fp = file_util::open_file(name.c_str(), "a+");
    if (!f->fp) {
      printf("[SCE] sceOpen(%s) failed.\n", name.c_str());
      return -1;
    }
  }
  const s32 fd = sce_next_fd++;
  sce_files[fd] = std::move(f);
  return fd;
}

s32 sceClose(s32 fd) {
  if (fd < 0) {
    return 0;
  }
  auto kv = sce_files.find(fd);
  if (kv == sce_files.end()) {
    printf("[SCE] sceClose called on invalid fd\n");
    return 0;
  }
  std::unique_ptr<SceFile> f = std::move(kv->second);
  sce_files.erase(kv);
  if (f->fp) {
    fclose(f->fp);
  } else if (f->mem.writable) {
    const u64 hash = content_hash(f->mem.data.data(), f->mem.data.size());
    auto data = std::make_shared<const std::vector<u8>>(std::move(f->mem.data));
    {
      std::lock_guard<std::mutex> lock(g_known_mutex);
      auto it = g_known.find(f->path);
      if (it != g_known.end() && it->second.size == data->size() && it->second.hash == hash) {
        lg::debug("[sce] {} unchanged, not written", f->path);
        return 0;  // the file holds this already, or will (queued)
      }
      auto& known = g_known[f->path];
      known.hash = hash;
      known.size = data->size();
      known.pending = data;
    }
    ctr_bg_run(write_job, new WriteJob{f->path, std::move(data)});
  }
  return 0;
}

s32 sceRead(s32 fd, void* buf, s32 nbyte) {
  auto kv = sce_files.find(fd);
  if (kv == sce_files.end()) {
    return -1;
  }
  SceFile& f = *kv->second;
  return f.fp ? (s32)fread(buf, 1, nbyte, f.fp) : f.mem.read(buf, nbyte);
}

s32 sceWrite(s32 fd, const void* buf, s32 nbyte) {
  auto kv = sce_files.find(fd);
  if (kv == sce_files.end()) {
    ASSERT(false);
    return -1;
  }
  SceFile& f = *kv->second;
  return f.fp ? (s32)fwrite(buf, 1, nbyte, f.fp) : f.mem.write(buf, nbyte);
}

s32 sceLseek(s32 fd, s32 offset, s32 where) {
  auto kv = sce_files.find(fd);
  if (kv == sce_files.end()) {
    return -1;
  }
  SceFile& f = *kv->second;
  if (where != SCE_SEEK_SET && where != SCE_SEEK_CUR && where != SCE_SEEK_END) {
    ASSERT(false);
    return -1;
  }
  if (!f.fp) {
    return f.mem.seek(offset, where);
  }
  static const int kWhence[3] = {SEEK_SET, SEEK_CUR, SEEK_END};  // SCE_SEEK_SET, _CUR, _END
  fseek(f.fp, offset, kWhence[where]);
  return ftell(f.fp);
}
#endif

}  // namespace ee
