#include "texshare.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/c3l_reader.h"

namespace fs = std::filesystem;

namespace texshare {

bool read_level_keys(const fs::path& path, LevelKeys* out) {
  out->name = path.stem().string();
  out->keys.clear();
  out->bytes.clear();
  c3l::Reader f(path.string().c_str());
  c3l::Header hdr;
  c3l::ExtHeader eh{};
  c3l::ExtTexKey tk{};
  std::vector<c3l::Texture> texs;
  if (!f.ok() || !f.read(0, &hdr, sizeof(hdr)) || memcmp(hdr.magic, c3l::kMagic, 4) ||
      hdr.version < c3l::kTexMaskVersion || !hdr.ext_offset ||
      !f.read(hdr.ext_offset, &eh, sizeof(eh)) || memcmp(eh.magic, c3l::kExtMagic, 4) ||
      eh.size < c3l::kExtHeaderTexKeySize ||
      !f.read(hdr.ext_offset + c3l::kExtHeaderV23Size, &tk, sizeof(tk)) ||
      tk.count != hdr.num_textures ||
      !f.read_array(hdr.textures_offset, hdr.num_textures, &texs)) {
    return false;
  }
  out->keys.resize(tk.count);
  if (tk.count && !f.read(tk.offset, out->keys.data(), tk.count * sizeof(uint64_t))) {
    return false;
  }
  out->bytes.assign(texs.size(), 0);
  for (size_t i = 0; i < texs.size(); i++) {
    const size_t owner = c3l::texture_is_mask(texs[i]) ? c3l::texture_mask_of(texs[i]) : i;
    if (owner < texs.size()) {
      out->bytes[owner] += (texs[i].data_size + 127) & ~127u;
    }
  }
  return true;
}

bool write_table(const fs::path& dir) {
  std::vector<LevelKeys> levels;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() != ".c3l" || e.path().stem() == "KEIRA") {
      continue;
    }
    LevelKeys k;
    if (read_level_keys(e.path(), &k)) {
      levels.push_back(std::move(k));
    }
  }
  std::sort(levels.begin(), levels.end(),
            [](const LevelKeys& a, const LevelKeys& b) { return a.name < b.name; });
  std::map<uint64_t, std::vector<uint32_t>> where;
  for (uint32_t l = 0; l < levels.size(); l++) {
    for (uint64_t k : levels[l].keys) {
      if (k && (where[k].empty() || where[k].back() != l)) {
        where[k].push_back(l);
      }
    }
  }
  std::map<std::vector<uint32_t>, uint32_t> group_of;
  std::vector<c3l::TexShareEntry> entries;
  for (const auto& [k, ls] : where) {
    if (ls.size() < 2) {
      continue;
    }
    auto it = group_of.emplace(ls, (uint32_t)group_of.size()).first;
    entries.push_back(c3l::TexShareEntry{k, it->second, 0});
  }
  c3l::TexShareHeader h{};
  memcpy(h.magic, c3l::kTexShareMagic, 4);
  h.version = 1;
  h.count = (uint32_t)entries.size();
  h.groups = (uint32_t)group_of.size();
  const fs::path out = dir / "texshare.c3t";
  FILE* fp = fopen(out.string().c_str(), "wb");
  if (!fp) {
    return false;
  }
  const bool ok = fwrite(&h, sizeof(h), 1, fp) == 1 &&
                  (entries.empty() ||
                   fwrite(entries.data(), sizeof(entries[0]), entries.size(), fp) == entries.size());
  fclose(fp);
  printf("texshare.c3t: %zu levels, %zu textures in more than one of them, %u groups\n",
         levels.size(), entries.size(), h.groups);
  return ok;
}

}  // namespace texshare
