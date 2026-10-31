#pragma once

/*!
 * @file texshare.h
 * The texture sharing table of a folder of .c3l files (c3l::TexShareHeader, texshare.c3t): the
 * content keys (c3l::ExtTexKey) that more than one level has, grouped by the levels that have them.
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace texshare {

struct LevelKeys {
  std::string name;
  std::vector<uint64_t> keys;   // by texture index (0: not shared)
  std::vector<uint32_t> bytes;  // the texture's bytes with its mask's
  std::vector<uint64_t> desc;   // c3l::texture_share_desc
};

/*! A level's texture keys and sizes (false: no key table). */
bool read_level_keys(const std::filesystem::path& c3l, LevelKeys* out);

/*! Writes dir/texshare.c3t from the .c3l files in dir (not KEIRA.c3l). */
bool write_table(const std::filesystem::path& dir);

}  // namespace texshare
