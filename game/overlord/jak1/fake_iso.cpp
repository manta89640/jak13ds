#include "fake_iso.h"

#include <chrono>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/BinaryReader.h"
#include "common/util/FileUtil.h"

#include "game/overlord/common/fake_iso.h"
#include "game/overlord/common/overlord.h"
#include "game/overlord/common/soundcommon.h"
#include "game/overlord/jak1/isocommon.h"
#include "game/sound/sndshim.h"

#ifdef __3DS__
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

void fake_iso_init_globals() {
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
#endif

static FILE* open_fr(FileRecord* fr, LoadStackEntry* lse, s32 thread_to_wake) {
  const char* path = get_file_path(fr);
  FILE* fp = file_util::open_file(path, "rb");
  if (!fp) {
    lg::error("[OVERLORD] fake iso could not open the file \"{}\"", path);
  }
#ifdef __3DS__
  lse->cz = nullptr;
  lse->io_bytes = 0;
  lse->io_us = 0;
  lse->io_reads = 0;
  lse->crc = 0;
  lse->crc_start = 0;
  lse->crc_next = 0;
  if (fp && is_file_compressed(fr)) {
    auto* r = new iso_cz::Reader();
    if (r->open(fp, path)) {
      lse->cz = r;
    } else {
      lg::error("[OVERLORD] fake iso: \"{}\" is not a valid compressed file", path);
      delete r;  // closes fp
      fp = nullptr;
    }
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
      lg::info("[io] {}: {} B in {} reads, {:.0f} ms ({:.1f} MB/s); card {} B in {:.0f} ms "
               "({:.1f} MB/s), decode {:.0f} ms ({:.1f} MB/s, {} blocks, max {:.2f} ms){}",
               fd->fr->name, fd->io_bytes, fd->io_reads, s_total * 1e3,
               fd->io_bytes / 1e6 / s_total, r->sd_bytes, r->sd_us / 1e3,
               r->sd_bytes / (double)std::max<uint64_t>(r->sd_us, 1), r->decode_us / 1e3,
               r->out_bytes / (double)std::max<uint64_t>(r->decode_us, 1), r->blocks,
               r->max_decode_us / 1e3, crc);
    } else {
      lg::info("[io] {}: {} B in {} reads, {:.0f} ms ({:.1f} MB/s), uncompressed{}", fd->fr->name,
               fd->io_bytes, fd->io_reads, s_total * 1e3, fd->io_bytes / 1e6 / s_total, crc);
    }
  }
  if (fd->cz) {
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
  fseek(fd->fp, 0, SEEK_END);
  uint32_t file_len = ftell(fd->fp);
  rewind(fd->fp);

  if (offset_into_file < file_len) {
    if (offset_into_file) {
      fseek(fd->fp, offset_into_file, SEEK_SET);
    }

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
  if (fd->cz) {
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
      lg::warn("[io] read {} KB of {} in {:.0f} ms", real_size / 1024, fd->fr->name, ms);
    }
#ifdef __3DS__
    fd->io_us += (uint64_t)(ms * 1000.0);
    fd->io_bytes += real_size;
    fd->io_reads++;
#endif
  }

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

  snd::BankHandle handle = snd_BankLoadEx(get_file_path(file), offset, 0, 0);
  snd_ResolveBankXREFS();
  PrintBankInfo(bank);
  bank->bank_handle = handle;

  return 0;
}
}  // namespace jak1
