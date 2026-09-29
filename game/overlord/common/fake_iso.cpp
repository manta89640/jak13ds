/*!
 * @file fake_iso.cpp
 * This provides an implementation of IsoFs for reading a "fake iso".
 * A "fake iso" is just a map file which maps 8.3 ISO file names to files in the source folder.
 * This way we don't need to actually create an ISO.
 *
 * The game has this compilation unit, but there is nothing in it. Probably it is removed to save
 * IOP memory and was only included on TOOL-only builds.  So this is my interpretation of how it
 * should work.
 */

#include "fake_iso.h"

#include <cstring>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/common/overlord_common.h"
#include "game/overlord/common/isocommon.h"
#include "game/overlord/common/overlord.h"
#include "game/overlord/common/sbank.h"
#include "game/overlord/common/soundcommon.h"
#include "game/overlord/common/srpc.h"
#include "game/runtime.h"
#include "game/sce/iop.h"
#include "game/sound/sndshim.h"

#ifdef __3DS__
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "platform/3ds/port/ctr_port.h"
#endif

using namespace iop;

BS::thread_pool thpool(4);

/*!
 * Map from iso file name to file path in the src folder.
 */
struct FakeIsoEntry {
  char iso_name[16];
  std::string full_path;
};

FakeIsoEntry fake_iso_entries[MAX_ISO_FILES];  //! List of all known files
static FileRecord sFiles[MAX_ISO_FILES];       //! List of "FileRecords" for IsoFs API consumers
u32 fake_iso_entry_count;                      //! Total count of fake iso files

void fake_iso_init_globals() {
  // init file lists
  memset(fake_iso_entries, 0, sizeof(fake_iso_entries));
  memset(sFiles, 0, sizeof(sFiles));

  fake_iso_entry_count = 0;

#ifdef __3DS__
  // The reader threads must be able to run while the IOP polls for their results: raise their
  // priority (no time slicing between equal priorities on the 3DS). One task per pool thread,
  // held until all of them started so that each thread takes exactly one.
  static bool s_prio_set = false;
  if (!s_prio_set) {
    s_prio_set = true;
    const int n = (int)thpool.get_thread_count();
    std::atomic<int> started{0};
    std::vector<std::future<void>> tasks;
    for (int i = 0; i < n; i++) {
      tasks.push_back(thpool.submit([&started, n]() {
        ctr_thread_set_priority(CTR_PRIO_IO);
        started++;
        while (started.load() < n) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }));
    }
    for (auto& t : tasks) {
      t.wait();
    }
  }
#endif
}

/*!
 * Initialize the file system.
 */
int fake_iso_FS_Init() {
  for (const auto& f : fs::directory_iterator(file_util::get_jak_project_dir() / "out" /
                                              game_version_names[g_game_version] / "iso")) {
    if (f.is_regular_file() || f.is_symlink()) {
      ASSERT(fake_iso_entry_count < MAX_ISO_FILES);
      FakeIsoEntry* e = &fake_iso_entries[fake_iso_entry_count];
      std::string file_name = f.path().filename().string();
      ASSERT(file_name.length() < 16);  // should be 8.3.
      ASSERT_MSG(f.exists(),            // should only happen if the file is a symlink, afaik
                 fmt::format("[FAKEISO] couldn't find {} -- broken symlink?", file_name));
      strcpy(e->iso_name, file_name.c_str());
      e->full_path = fmt::format("{}/out/{}/iso/{}", file_util::get_jak_project_dir().string(),
                                 game_version_names[g_game_version], file_name);
      fake_iso_entry_count++;
    }
  }

  for (u32 i = 0; i < fake_iso_entry_count; i++) {
    MakeISOName(sFiles[i].name, fake_iso_entries[i].iso_name);
    // we don't figure out the size yet.
    // this is so you can change the file without restarting the game.
    sFiles[i].size = -1;
    // repurpose "location" as the index.
    sFiles[i].location = i;
  }

  LoadMusicTweaks();

  return 0;
}

/*!
 * Find a file on the disc and return a FileRecord.
 * Find using a "normal" 8.3 name.
 * This is an ISO FS API Function
 */
FileRecord* FS_Find(const char* name) {
  char name_buff[16];
  MakeISOName(name_buff, name);
  return FS_FindIN(name_buff);
}

/*!
 * Find a file on the disc.  Uses the "ISO name" of the file, which is different from the normal 8.3
 * name.  This can be generated with MakeISOFile.
 * This is an ISO FS API Function.
 */
FileRecord* FS_FindIN(const char* iso_name) {
  const uint32_t* buff = (const uint32_t*)iso_name;
  uint32_t count = 0;
  while (count < fake_iso_entry_count) {
    const uint32_t* ref = (uint32_t*)sFiles[count].name;
    if (ref[0] == buff[0] && ref[1] == buff[1] && ref[2] == buff[2]) {
      return sFiles + count;
    }
    count++;
  }
  lg::error("[FAKEISO] failed to find {}\n", iso_name);
  return nullptr;
}

/*!
 * Build a full file path for a FileRecord.
 */
const char* get_file_path(FileRecord* fr) {
  ASSERT(fr->location < fake_iso_entry_count);
  return fake_iso_entries[fr->location].full_path.c_str();
}

/*!
 * Determine the length of a file. This isn't very fast, but nobody checks file sizes extremely
 * quickly. This is an ISO FS API Function
 */
uint32_t FS_GetLength(FileRecord* fr) {
  const char* path = get_file_path(fr);
  file_util::assert_file_exists(path, "fake_iso FS_GetLength");
  FILE* fp = file_util::open_file(path, "rb");
  ASSERT(fp);
  fseek(fp, 0, SEEK_END);
  uint32_t len = ftell(fp);
  rewind(fp);
  fclose(fp);
  return len;
}

void LoadMusicTweaks() {
  char tweakname[16];
  MakeISOName(tweakname, "TWEAKVAL.MUS");
  auto file = FS_FindIN(tweakname);
  if (file) {
    auto fp = file_util::open_file(get_file_path(file), "rb");
    fread(&gMusicTweakInfo, sizeof(gMusicTweakInfo), 1, fp);
    fclose(fp);
  } else {
    gMusicTweakInfo.TweakCount = 0;
  }
}
