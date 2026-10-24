#include "fake_iso.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <span>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/BinaryReader.h"
#include "common/util/FileUtil.h"

#include "game/overlord/common/fake_iso.h"
#include "game/overlord/common/overlord.h"
#include "game/overlord/common/soundcommon.h"
#include "game/overlord/jak1/isocommon.h"
#include "game/overlord/jak1/srpc.h"
#include "game/sound/sndshim.h"

#ifdef __3DS__
#include <malloc.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "game/overlord/common/compressed_iso_file.h"
#include "platform/3ds/port/ctr_port.h"
#endif

namespace jak1 {
IsoFs fake_iso;
LoadStackEntry* FS_Open(FileRecord* fr, int32_t offset);
uint32_t FS_BeginRead(LoadStackEntry* fd, void* buffer, int32_t len);
LoadStackEntry* FS_OpenWad(FileRecord* fr, int32_t offset);
uint32_t FS_LoadSoundBank(char* name, SoundBank* bank);
void FS_PollDrive();
uint32_t FS_SyncRead();
uint32_t FS_LoadMusic(char* name, snd::BankHandle* bank_handle);
void FS_Close(LoadStackEntry* fd);
static LoadStackEntry sLoadStack[MAX_OPEN_FILES];  //! List of all files that are "open"
static LoadStackEntry* sReadInfo;                  // LoadStackEntry for currently reading file
#ifdef __3DS__
namespace {
void close_all_parked();
}
#endif

void fake_iso_init_globals() {
#ifdef __3DS__
  close_all_parked();  // (AI-assisted) files kept open by a runtime that restarted
#endif
  // init API struct
  fake_iso.init = fake_iso_FS_Init;
  fake_iso.find = FS_Find;
  fake_iso.find_in = FS_FindIN;
  fake_iso.get_length = FS_GetLength;
  fake_iso.open = FS_Open;
  fake_iso.open_wad = FS_OpenWad;
  fake_iso.close = FS_Close;
  fake_iso.begin_read = FS_BeginRead;
  fake_iso.sync_read = FS_SyncRead;
  fake_iso.poll_drive = FS_PollDrive;
  fake_iso.load_sound_bank = FS_LoadSoundBank;
  fake_iso.load_music = FS_LoadMusic;

  memset(sLoadStack, 0, sizeof(sLoadStack));
  sReadInfo = nullptr;
}

#ifdef __3DS__
static bool io_checksum() {
  static const bool on = ctr_config_bool("io_checksum", 0);
  return on;
}

bool io_cache_on() {
  static const bool on = ctr_config_bool("io_cache", 1);
  return on;
}

u32 io_heap_free() {
  // ctr_app_mem_free is the heap's size (the application region is committed at startup)
  struct mallinfo mi = mallinfo();
  const u32 heap = ctr_app_mem_free();
  return heap > (u32)mi.uordblks ? heap - (u32)mi.uordblks : 0;
}

namespace {
u32 now_ms() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (u32)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

// (AI-assisted) SD card use by streams (spool chunks, voice lines), for FS_StreamBusy
std::atomic<int> sStreamReads{0};
std::atomic<u32> sStreamLastMs{0};  // the last one (+1, 0: none yet)
void stream_mark() {
  sStreamLastMs.store(now_ms() + 1);
}

// (AI-assisted) Files closed by FS_Close stay open for the next load of the same file. A
// spooled animation is loaded one chunk at a time and the voice stream file once per line of
// dialogue, and every open cost several SD card requests (find, open, header, the close later)
// on the reader thread, in the way of the stream itself. The least recently closed file goes
// first; its close runs on a reader thread.
struct ParkedFile {
  FileRecord* fr = nullptr;
  FILE* fp = nullptr;            // plain file (cz == nullptr)
  iso_cz::Reader* cz = nullptr;  // compressed file: owns its FILE
  uint32_t len = 0;              // length of the (original) file
};
constexpr int kParkedFiles = 6;
// the two most recently closed files keep their last decoded block (the next chunk of a spool
// starts in it); the others free it (up to 48 KB each)
constexpr int kParkedKeepBlock = 2;
std::mutex sParkedMutex;
ParkedFile sParked[kParkedFiles];  // most recently closed first
int sParkedCount = 0;

void close_parked(ParkedFile p) {
  if (p.cz) {
    delete p.cz;  // closes its file
  } else if (p.fp) {
    fclose(p.fp);
  }
}

bool take_parked(FileRecord* fr, ParkedFile* out) {
  std::lock_guard<std::mutex> lock(sParkedMutex);
  for (int i = 0; i < sParkedCount; i++) {
    if (sParked[i].fr == fr) {
      *out = sParked[i];
      for (int j = i; j + 1 < sParkedCount; j++) {
        sParked[j] = sParked[j + 1];
      }
      sParked[--sParkedCount] = ParkedFile();
      return true;
    }
  }
  return false;
}

void park(const ParkedFile& p) {
  ParkedFile out;
  {
    std::lock_guard<std::mutex> lock(sParkedMutex);
    if (sParkedCount == kParkedFiles) {
      out = sParked[--sParkedCount];
    }
    for (int j = sParkedCount; j > 0; j--) {
      sParked[j] = sParked[j - 1];
    }
    sParked[0] = p;
    sParkedCount++;
    if (sParkedCount > kParkedKeepBlock && sParked[kParkedKeepBlock].cz) {
      sParked[kParkedKeepBlock].cz->release_buffers(false);
    }
  }
  if (out.fr) {
    thpool.push_task(close_parked, out);
  }
}

void close_all_parked() {
  std::lock_guard<std::mutex> lock(sParkedMutex);
  for (int i = 0; i < sParkedCount; i++) {
    close_parked(sParked[i]);
    sParked[i] = ParkedFile();
  }
  sParkedCount = 0;
}
}  // namespace

bool FS_StreamBusy(u32 quiet_ms) {
  if (sStreamReads.load() > 0) {
    return true;
  }
  const u32 last = sStreamLastMs.load();
  return last && now_ms() + 1 - last < quiet_ms;
}

void FS_SetReadHint(LoadStackEntry* fd, uint32_t end, bool stream) {
  if (!fd) {
    return;
  }
  fd->read_end = end;
  fd->stream = stream;
  if (stream) {
    stream_mark();
  }
  auto* r = (iso_cz::Reader*)fd->cz;
  if (r && io_cache_on()) {
    // card reads of up to 256 KB (a voice stream: 64 KB, it reads 24 KB at a time for as long as
    // the line lasts), while the heap has room for the window (it replaces the 48 KB block
    // buffer; the heap's level-load peak leaves ~3 MB)
    const u32 ra = (stream && !end) ? (64u << 10) : (256u << 10);
    r->set_read_ahead(io_heap_free() >= (2u << 20) ? ra : 0);
    r->set_read_end(end);
  }
}
#endif

static FILE* open_fr(FileRecord* fr, LoadStackEntry* lse, s32 thread_to_wake) {
  const char* path = get_file_path(fr);
#ifdef __3DS__
  lse->cz = nullptr;
  lse->io_bytes = 0;
  lse->io_us = 0;
  lse->io_reads = 0;
  lse->crc = 0;
  lse->crc_start = 0;
  lse->crc_next = 0;
  lse->file_len = 0;
  lse->read_end = 0;
  lse->stream = false;
  lse->slow_reads = 0;
  lse->slow_ms = 0;
  lse->slow_max_ms = 0;
  FILE* fp = nullptr;
  ParkedFile p;
  if (take_parked(fr, &p)) {
    // (AI-assisted) still open from the last load of this file
    fp = p.cz ? p.cz->file() : p.fp;
    if (p.cz) {
      p.cz->reset_stats();
    }
  } else {
    fp = file_util::open_file(path, "rb");
    if (!fp) {
      lg::error("[OVERLORD] fake iso could not open the file \"{}\"", path);
    } else if (is_file_compressed(fr)) {
      auto* r = new iso_cz::Reader();
      if (r->open(fp, path)) {
        p.cz = r;
        p.len = r->size();
      } else {
        lg::error("[OVERLORD] fake iso: \"{}\" is not a valid compressed file", path);
        delete r;  // closes fp
        fp = nullptr;
      }
    } else {
      // (AI-assisted) Reads are whole pages into the caller's buffer: unbuffered, each one is a
      // single card read (newlib's 1 KB buffer split it in two). The length once, here: asking
      // for it is a request to the SD card.
      setvbuf(fp, nullptr, _IONBF, 0);
      fseek(fp, 0, SEEK_END);
      p.len = (uint32_t)ftell(fp);
      fseek(fp, 0, SEEK_SET);
    }
  }
  if (fp) {
    lse->cz = p.cz;
    lse->file_len = p.len;
    // FS_GetLength's answer (the files don't change while the game runs)
    if (fr->size != p.len) {
      fr->size = p.len;
    }
  }
#else
  FILE* fp = file_util::open_file(path, "rb");
  if (!fp) {
    lg::error("[OVERLORD] fake iso could not open the file \"{}\"", path);
  }
#endif

  iop::iWakeupThread(thread_to_wake);

  return fp;
}

/*!
 * Open a file by putting it on the load stack.
 * Set the offset to 0 or -1 if you do not want to have an offset.
 * This is an ISO FS API Function
 */
LoadStackEntry* FS_Open(FileRecord* fr, int32_t offset) {
  lg::debug("[OVERLORD] FS Open {}", fr->name);
  LoadStackEntry* selected = nullptr;
  // find first unused spot on load stack.
  for (uint32_t i = 0; i < MAX_OPEN_FILES; i++) {
    if (!sLoadStack[i].fr) {
      selected = sLoadStack + i;
      selected->fr = fr;
      selected->location = 0;

      if (offset != -1) {
        selected->location += offset;
      }

      auto future = thpool.submit(open_fr, fr, selected, iop::GetThreadId());
      iop::SleepThread();
      selected->fp = future.get();

      return selected;
    }
  }
  lg::warn("[OVERLORD] Failed to FS Open {}", fr->name);
  ExitIOP();
  return nullptr;
}

/*!
 * Open a file by putting it on the load stack.
 * Like Open, but allows an offset of -1 to be applied.
 * This is an ISO FS API Function
 */
LoadStackEntry* FS_OpenWad(FileRecord* fr, int32_t offset) {
  lg::debug("[OVERLORD] FS_OpenWad {}", fr->name);
  LoadStackEntry* selected = nullptr;
  for (uint32_t i = 0; i < MAX_OPEN_FILES; i++) {
    if (!sLoadStack[i].fr) {
      selected = sLoadStack + i;
      selected->fr = fr;
      selected->location = offset;

      auto future = thpool.submit(open_fr, fr, selected, iop::GetThreadId());
      iop::SleepThread();
      selected->fp = future.get();

      return selected;
    }
  }
  lg::warn("[OVERLORD] Failed to FS_OpenWad {}", fr->name);
  ExitIOP();
  return nullptr;
}

/*!
 * Close an open file.
 * This is an ISO FS API Function
 */
void FS_Close(LoadStackEntry* fd) {
  lg::debug("[OVERLORD] FS_Close {} @ {}/{}", fd->fr->name, fd->fr->location, fd->location);

#ifdef __3DS__
  // (AI-assisted) where the load time of a big file goes: card reads vs decoding
  // (compressed files from 256 KB, or all of them with io_checksum = on: the voice lines)
  if (fd->io_bytes >= (fd->cz ? (io_checksum() ? 1u : (256u << 10)) : (1u << 20))) {
    auto* r = (iso_cz::Reader*)fd->cz;
    std::string crc;
    if (io_checksum() && fd->crc_next != ~0u) {
      crc = fmt::format(", crc32 {:08x} over {} B", fd->crc, fd->crc_next - fd->crc_start);
      if (fd->crc_start) {
        crc += fmt::format(" from {}", fd->crc_start);
      } else if (r && fd->crc_next == r->size()) {
        crc += fd->crc == r->expected_crc32() ? " (ok)" : " (MISMATCH)";
      }
    }
    const double s_total = fd->io_us / 1e6;
    if (r) {
      lg::info("[io] {}: {} B in {} reads, {:.0f} ms ({:.1f} MB/s); card {} B in {} reads, "
               "{:.0f} ms ({:.1f} MB/s), decode {:.0f} ms ({:.1f} MB/s, {} blocks, max {:.2f} "
               "ms){}",
               fd->fr->name, fd->io_bytes, fd->io_reads, s_total * 1e3,
               fd->io_bytes / 1e6 / s_total, r->sd_bytes, r->sd_reads, r->sd_us / 1e3,
               r->sd_bytes / (double)std::max<uint64_t>(r->sd_us, 1), r->decode_us / 1e3,
               r->out_bytes / (double)std::max<uint64_t>(r->decode_us, 1), r->blocks,
               r->max_decode_us / 1e3, crc);
    } else {
      lg::info("[io] {}: {} B in {} reads, {:.0f} ms ({:.1f} MB/s), uncompressed{}", fd->fr->name,
               fd->io_bytes, fd->io_reads, s_total * 1e3, fd->io_bytes / 1e6 / s_total, crc);
    }
  }
  if (fd->slow_reads) {
    lg::warn("[io] {}: {} slow read(s) of 20 ms or more, {} ms in all, the slowest {} ms",
             fd->fr->name, fd->slow_reads, fd->slow_ms, fd->slow_max_ms);
  }
  if (fd->fp && io_cache_on()) {
    // (AI-assisted) kept open for the next load of this file
    ParkedFile p;
    p.fr = fd->fr;
    p.cz = (iso_cz::Reader*)fd->cz;
    p.fp = p.cz ? nullptr : fd->fp;
    p.len = fd->file_len;
    if (p.cz) {
      p.cz->release_buffers(true);
    }
    park(p);
    fd->cz = nullptr;
    fd->fp = nullptr;
  } else if (fd->cz) {
    delete (iso_cz::Reader*)fd->cz;  // closes fp
    fd->cz = nullptr;
    fd->fp = nullptr;
  }
#endif
  // close the FD
  fd->fr = nullptr;
  if (fd->fp) {
    fclose(fd->fp);
  }
  if (fd == sReadInfo) {
    sReadInfo = nullptr;
  }
}

// returns the number of bytes read (0 past the end of the file)
static int32_t fs_read_plain(LoadStackEntry* fd, void* buffer, int32_t real_size,
                             u32 offset_into_file) {
#ifdef __3DS__
  // (AI-assisted) the length found when the file was opened (asking is an SD card request)
  const uint32_t file_len = fd->file_len;
#else
  fseek(fd->fp, 0, SEEK_END);
  uint32_t file_len = ftell(fd->fp);
  rewind(fd->fp);
#endif

  if (offset_into_file < file_len) {
#ifdef __3DS__
    fseek(fd->fp, offset_into_file, SEEK_SET);  // (no rewind above: always)
#else
    if (offset_into_file) {
      fseek(fd->fp, offset_into_file, SEEK_SET);
    }
#endif

    if (offset_into_file + real_size > file_len) {
      real_size = (file_len - offset_into_file);
    }

    if (fread(buffer, real_size, 1, fd->fp) != 1) {
      ASSERT(false);
    }
    return real_size;
  }
  return 0;
}

void fs_read(LoadStackEntry* fd, void* buffer, int32_t len, s32 thread_to_wake) {
  // (AI-assisted) slow SD card reads are logged: a freeze in the game can be the game waiting for
  // one (with the [hitch] lines of kperf)
  const auto t_start = std::chrono::steady_clock::now();
#ifdef __3DS__
  if (fd->stream) {
    sStreamReads++;
  }
#endif
  int32_t real_size = len;
  if (len < 0) {
    // not sure what this is about...
    lg::warn("[OVERLORD ISO CD] Negative length warning!");
    real_size = len + 0x7ff;
  }
  u32 sectors = real_size / SECTOR_SIZE;
  real_size = sectors * SECTOR_SIZE;
  u32 offset_into_file = SECTOR_SIZE * fd->location;

  ASSERT(fd->fp);
#ifdef __3DS__
  if (fd->read_end) {
    // (AI-assisted) only the sectors the load asked for: its last page read (and decoded) up to
    // 48 KB past them, and the page the ISO thread reads ahead after the end is no read at all
    const u32 end = (fd->read_end + SECTOR_SIZE - 1) / SECTOR_SIZE * SECTOR_SIZE;
    if (offset_into_file >= end) {
      real_size = 0;
    } else if (offset_into_file + real_size > end) {
      real_size = (int32_t)(end - offset_into_file);
    }
  }
  if (real_size <= 0) {
    real_size = 0;
  } else if (fd->cz) {
    // (AI-assisted) compressed: decoded here, on the reader thread
    const int64_t n = ((iso_cz::Reader*)fd->cz)->read(offset_into_file, buffer, real_size);
    ASSERT_MSG(n >= 0, fmt::format("[OVERLORD] read error in compressed {}", fd->fr->name));
    real_size = (int32_t)n;
  } else {
    real_size = fs_read_plain(fd, buffer, real_size, offset_into_file);
  }
#else
  real_size = fs_read_plain(fd, buffer, real_size, offset_into_file);
#endif
#ifdef __3DS__
  if (io_checksum() && fd->crc_next != ~0u) {
    // (AI-assisted) debug: the CRC of what the game got, compared with the file's at FS_Close
    if (real_size == 0) {
      // past the end (the DGO loader asks for one more page)
    } else if (fd->io_reads == 0 || offset_into_file == fd->crc_next) {
      if (fd->io_reads == 0) {
        fd->crc_start = fd->crc_next = offset_into_file;  // a WAD is read from its entry on
      }
      fd->crc = iso_cz::crc32_update(fd->crc, buffer, real_size);
      fd->crc_next += real_size;
    } else {
      fd->crc_next = ~0u;
    }
  }
#endif

  if (len < 0) {
    len = len + 0x7ff;
  }

  fd->location += (len / SECTOR_SIZE);
  sReadInfo = fd;
  {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                t_start)
                          .count();
    if (ms >= 20.0) {
#ifdef __3DS__
      // (AI-assisted) A line per slow read made many lines (an SD card write each) while the card
      // was busy: at most one a second, and FS_Close sums them up per file.
      fd->slow_reads++;
      fd->slow_ms += (uint32_t)ms;
      fd->slow_max_ms = std::max(fd->slow_max_ms, (uint32_t)ms);
      static std::atomic<u32> s_last_warn{0};
      const u32 now = now_ms() + 1;
      const u32 last = s_last_warn.load();
      if (!last || now - last >= 1000) {
        s_last_warn.store(now);
        lg::warn("[io] read {} KB of {} in {:.0f} ms", real_size / 1024, fd->fr->name, ms);
      }
#else
      lg::warn("[io] read {} KB of {} in {:.0f} ms", real_size / 1024, fd->fr->name, ms);
#endif
    }
#ifdef __3DS__
    fd->io_us += (uint64_t)(ms * 1000.0);
    fd->io_bytes += real_size;
    fd->io_reads++;
#endif
  }
#ifdef __3DS__
  if (fd->stream) {
    stream_mark();
    sStreamReads--;
  }
#endif

  iop::iWakeupThread(thread_to_wake);
}

/*!
 * Begin reading!  Returns FS_READ_OK on success (always)
 * This is an ISO FS API Function
 *
 * Idea: do the fopen in FS_Open and keep the file open?  It would be faster.
 */
uint32_t FS_BeginRead(LoadStackEntry* fd, void* buffer, int32_t len) {
  ASSERT(fd->fr->location < fake_iso_entry_count);

  auto future = thpool.submit(fs_read, fd, buffer, len, iop::GetThreadId());
  iop::SleepThread();
  future.get();

  return CMD_STATUS_IN_PROGRESS;
}

/*!
 * Block until read completes.
 */
uint32_t FS_SyncRead() {
  // FS_BeginRead is blocking, so this is useless.
  if (sReadInfo) {
    sReadInfo = nullptr;
    return CMD_STATUS_IN_PROGRESS;
  } else {
    return CMD_STATUS_READ_ERR;
  }
}

/*!
 * Poll drive
 */
void FS_PollDrive() {}

uint32_t FS_LoadMusic(char* name, snd::BankHandle* bank_handle) {
  char namebuf[16];
  strcpy(namebuf, name);
  namebuf[8] = 0;
  strcat(namebuf, ".mus");
  auto file = FS_Find(namebuf);
  if (!file)
    return CMD_STATUS_FAILED_TO_OPEN;

  *bank_handle = snd_BankLoadEx(get_file_path(file), 0, 0, 0);
  snd_ResolveBankXREFS();

  return 0;
}

// original overlord uses good old fread into a struct instance
// lets use BinaryReader instead
static void parseSoundBank(BinaryReader data, SoundBank& bank) {
  bank.name = data.read<std::array<char, 16>>();

  data.read<u32>();
  bank.bank_handle = nullptr;

  bank.sound_count = data.read<u32>();
  bank.sound.resize(bank.sound_count);

  for (auto& snd : bank.sound) {
    snd.name = data.read<std::array<char, 16>>();
    snd.fallof_params = data.read<u32>();
  }
}

uint32_t FS_LoadSoundBank(char* name, SoundBank* bank) {
  char namebuf[16];

  // sector size of sound name list
  int offset = 10 * 2048;
  if (bank->sound_count == 101) {
    offset = 1 * 2048;
  }

  strcpy(namebuf, name);
  namebuf[8] = 0;
  strcat(namebuf, ".sbk");

  auto file = FS_Find(namebuf);
  if (!file) {
    file = FS_Find("empty1.sbk");
    if (!file)  // Might have no files when running tests.
      return 0;
  }

  // auto fp = file_util::open_file(get_file_path(file), "rb");
  // fread(bank, offset, 1, fp);
  // fclose(fp);

  auto data = file_util::read_binary_file(fs::path(get_file_path(file)));
  parseSoundBank(BinaryReader(data), *bank);

  // (AI-assisted) The 989snd bank follows the name list in the same file: load it from this copy
  // instead of reading the whole file again (snd_BankLoadEx).
  snd::BankHandle handle = 0;
  if (data.size() > (size_t)offset) {
    handle = snd_BankLoadFromMemory(std::span<u8>(data).subspan(offset));
  }
  snd_ResolveBankXREFS();
  PrintBankInfo(bank);
  bank->bank_handle = handle;
  KeiraMarkVoices(bank);  // (AI-assisted)

  return 0;
}

#ifdef __3DS__
namespace {
// (AI-assisted) Sound bank and music loads (LOAD_SOUND_BANK / LOAD_MUSIC) on a reader thread.
// On the ISO thread (FS_LoadSoundBank / FS_LoadMusic) they read the whole file in one go on the
// IOP thread (a sound bank twice: the name list, then the 989snd bank), and every IOP service
// waited for the SD card meanwhile: sound RPC replies, streams, other loads (~1 s at 2.5 fps
// entering Fire Canyon; COMMON.SBK at boot). Here the file is read once, in pieces (the card
// serves one request at a time: stream reads get their turn in between), the 989snd bank is
// parsed on the reader thread too, and the ISO thread keeps serving until the bank is in. The
// message goes back only then, as before.
constexpr long kSoundLoadPiece = 128 * 1024;
struct SoundLoad {
  IsoMessage* msg = nullptr;  // the load running (nullptr: none)
  std::string path;
  long offset = 0;        // sound bank: where the 989snd bank starts (after the name list)
  // (AI-assisted) the file's bytes before `offset` (the sound bank's name list). The rest is read
  // into a buffer the 989snd bank keeps (its samples aren't copied).
  std::vector<u8> data;
  bool ok = false;  // (AI-assisted) read and parsed
  long size = 0;    // the file's
  double read_ms = 0;
  snd::BankHandle handle = nullptr;
  std::atomic<bool> done{false};
};
SoundLoad sSoundLoad;
// loads waiting for the running one (the loader RPC waits for each, so this stays empty)
std::deque<IsoMessage*> sSoundLoadQueue;

// (AI-assisted) a load that runs out of heap memory (bad_alloc: an exception out of a thread pool
// task is std::terminate, the Spider Cave crash on hardware) waits and tries again, up to
// kSoundLoadRetries times; then the bank stays out (no sound from it) and the game goes on.
// (AI-assisted) The wait doubles from kSoundLoadRetryMs up to kSoundLoadRetryMaxMs: 11 tries over
// about 16 s (before: 41 over 20 s). Every try allocates the whole file again, so frequent tries
// competed with the level load for the heap they waited for. A failed try frees everything it
// allocated (loader.cpp owns the bank until it is in).
constexpr int kSoundLoadRetries = 10;
constexpr int kSoundLoadRetryMs = 250;
constexpr int kSoundLoadRetryMaxMs = 2000;

// one try: whether the file was read (the bank may still have failed to parse: handle)
bool sound_load_try(SoundLoad* s) {
  s->data.clear();
  s->handle = nullptr;
  s->size = 0;
  bool ok = false;
  FILE* fp = file_util::open_file(s->path.c_str(), "rb");
  if (!fp) {
    return false;
  }
  try {
    setvbuf(fp, nullptr, _IONBF, 0);
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size > 0) {
      std::unique_ptr<u8[]> buf(new u8[size]);
      const auto t0 = std::chrono::steady_clock::now();
      ok = true;
      for (long pos = 0; ok && pos < size; pos += kSoundLoadPiece) {
        ok = fread(buf.get() + pos, std::min(kSoundLoadPiece, size - pos), 1, fp) == 1;
      }
      s->read_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                       .count();
      if (ok) {
        s->size = size;
        s->data.assign(buf.get(), buf.get() + std::min(size, s->offset));
        if (size > s->offset) {
          s->handle = snd_BankLoadFromMemory(
              std::span<u8>(buf.get() + s->offset, (size_t)(size - s->offset)), std::move(buf));
        }
      }
    }
  } catch (...) {
    fclose(fp);
    s->data.clear();
    s->handle = nullptr;
    throw;
  }
  fclose(fp);
  return ok;
}

void sound_load_job(SoundLoad* s) {
  const auto t0 = std::chrono::steady_clock::now();
  bool ok = false;
  for (int attempt = 0;; attempt++) {
    try {
      ok = sound_load_try(s);
      if (attempt) {
        lg::info("[io] sound bank {}: loaded after {} tries", s->path, attempt + 1);
      }
      break;
    } catch (std::exception& e) {
      if (attempt == kSoundLoadRetries) {
        lg::error("[io] sound bank {}: {}, given up after {} tries (no sound from it)", s->path,
                  e.what(), attempt + 1);
        break;
      }
      if (attempt == 0) {
        lg::warn("[io] sound bank {}: {}, trying again in {} ms (up to {} ms between tries)",
                 s->path, e.what(), kSoundLoadRetryMs, kSoundLoadRetryMaxMs);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(
          std::min(kSoundLoadRetryMs << std::min(attempt, 4), kSoundLoadRetryMaxMs)));
    }
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!ok) {
    lg::error("[OVERLORD] fake iso could not read \"{}\"", s->path);
    s->data.clear();
    s->handle = nullptr;
  }
  s->ok = ok;
  lg::info("[io] sound bank {}: {} KB read in {:.0f} ms, loaded in {:.0f} ms", s->path,
           s->size / 1024, s->read_ms, ms);
  s->done.store(true);
}

void start_sound_load(IsoMessage* msg) {
  SoundLoad& s = sSoundLoad;
  s.msg = msg;
  s.path.clear();
  s.offset = 0;
  s.data.clear();
  s.ok = false;
  s.size = 0;
  s.read_ms = 0;
  s.handle = nullptr;
  s.done.store(false);
  char namebuf[16];
  FileRecord* file = nullptr;
  if (msg->cmd_id == LOAD_SOUND_BANK) {
    auto* cmd = (SoundBankLoadCommand*)msg;
    // sector size of sound name list (as FS_LoadSoundBank)
    s.offset = cmd->bank->sound_count == 101 ? 1 * 2048 : 10 * 2048;
    strcpy(namebuf, cmd->bank_name);
    namebuf[8] = 0;
    strcat(namebuf, ".sbk");
    file = FS_Find(namebuf);
    if (!file) {
      file = FS_Find("empty1.sbk");
    }
  } else {
    auto* cmd = (MusicLoadCommand*)msg;
    strcpy(namebuf, cmd->music_name);
    namebuf[8] = 0;
    strcat(namebuf, ".mus");
    file = FS_Find(namebuf);
  }
  if (!file) {
    s.done.store(true);  // nothing loaded (as FS_LoadSoundBank / FS_LoadMusic)
    return;
  }
  s.path = get_file_path(file);
  thpool.push_task(sound_load_job, &s);
}
}  // namespace

void FS_StartSoundLoad(IsoMessage* msg) {
  if (sSoundLoad.msg) {
    sSoundLoadQueue.push_back(msg);
  } else {
    start_sound_load(msg);
  }
}

bool FS_SoundLoadPending() {
  return sSoundLoad.msg != nullptr;
}

IsoMessage* FS_FinishSoundLoad() {
  SoundLoad& s = sSoundLoad;
  if (!s.msg || !s.done.load()) {
    return nullptr;
  }
  IsoMessage* msg = s.msg;
  if (s.ok) {
    if (msg->cmd_id == LOAD_SOUND_BANK) {
      auto* cmd = (SoundBankLoadCommand*)msg;
      parseSoundBank(BinaryReader(s.data), *cmd->bank);
      snd_ResolveBankXREFS();
      PrintBankInfo(cmd->bank);
      cmd->bank->bank_handle = s.handle;
      KeiraMarkVoices(cmd->bank);  // (AI-assisted)
    } else {
      auto* cmd = (MusicLoadCommand*)msg;
      *cmd->music_handle = s.handle;
      snd_ResolveBankXREFS();
    }
  }
  std::vector<u8>().swap(s.data);  // free it
  s.msg = nullptr;
  if (!sSoundLoadQueue.empty()) {
    IsoMessage* next = sSoundLoadQueue.front();
    sSoundLoadQueue.pop_front();
    start_sound_load(next);
  }
  return msg;
}
#endif
}  // namespace jak1
