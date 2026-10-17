#pragma once

/*!
 * @file ctr_config_presets.h
 * (AI-assisted)
 * config.ini's presets and user key aliases, one table for every reader: CtrSettings.cpp (the
 * renderer's settings, 3DS and host) and platform/3ds/port/ctr_port.c (ctr_config_get: sound, I/O,
 * native code, text, ... on the 3DS). C, header only.
 *
 * A key's value, first match wins:
 *   1. its own line in config.ini (the last one),
 *   2. a user key's line that stands for it (ctr_cfg_aliases: particles, text_size),
 *   3. the preset (config.ini preset = normal | safe | bench | breakdown; normal sets nothing),
 *   4. ctr_cfg_defaults (the keys ctr_config_get serves), else the reader's own default
 *      (CtrSettings.h's initializers for the renderer).
 * docs/3ds-port/config_ini.md (the user keys), docs/3ds-port/config_dev.md (the rest).
 */

#include <stdlib.h>
#include <string.h>

typedef struct {
  const char* key;
  const char* value;
} ctr_cfg_kv;

/* safe: the Oct 11 behaviour that held 29.9 fps on hardware (3ds-sdcard-oct12d config-safe.ini):
 * every runtime feature added after it back to its old behaviour; crash fixes stay in the code.
 * Particles come from the particles line (stage_sd.sh --preset safe writes particles = reduced). */
static const ctr_cfg_kv ctr_cfg_preset_safe[] = {
    {"overlap", "off"},
    {"shader_opt", "off"},       {"frame_overlap2", "off"},    {"skin_rows_f24", "off"},
    {"skin_clip_f24", "off"},    {"cmd_dedup", "off"},         {"cmd_dedup_draw", "off"},
    {"fog_alpha", "off"},        {"tex1_gating", "off"},       {"cull_merc", "off"},
    {"sky_last", "off"},         {"tex_cache_clear", "always"}, {"vista_untextured", "off"},
    {"bones_native", "off"},     {"c3l_read_buffer", "0"},     {"fog_stage_skip", "off"},
    {"static_cmdlists", "off"},  {"ocean_tex_half", "off"},    {"opaque_alpha", "off"},
    {"fine_vis", "off"},         {"vcache_reuse", "off"},      {"skip_clear", "off"},
    {"tie_lod", "off"},          {"gpu_micro", "off"},
};
/* bench: normal plus the GPU benchmark at boot (~20-60 s, data/log/bench.txt), the A/B GPU
 * profile, the GSP perf log and the GPU counters (3ds-sdcard-oct12d config-bench.ini) */
static const ctr_cfg_kv ctr_cfg_preset_bench[] = {
    {"pica_bench", "on"}, {"gpu_profile", "ab"}, {"gpu_perflog", "on"},
    {"gpu_counters", "on"}, {"overlap", "off"},
};
/* breakdown: normal plus the per-part GPU times every 4th frame (config-breakdown.ini) */
static const ctr_cfg_kv ctr_cfg_preset_breakdown[] = {
    {"gpu_breakdown", "on"},
};

typedef struct {
  const char* name;
  const ctr_cfg_kv* kv;
  int n;
} ctr_cfg_preset;

#define CTR_CFG_N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const ctr_cfg_preset ctr_cfg_presets[] = {
    {"normal", NULL, 0},
    {"safe", ctr_cfg_preset_safe, CTR_CFG_N(ctr_cfg_preset_safe)},
    {"bench", ctr_cfg_preset_bench, CTR_CFG_N(ctr_cfg_preset_bench)},
    {"breakdown", ctr_cfg_preset_breakdown, CTR_CFG_N(ctr_cfg_preset_breakdown)},
};

/* A user key's line stands for these keys. user_value NULL: any value, passed on as it is. */
typedef struct {
  const char* user_key;
  const char* user_value;
  const char* key;
  const char* value;
} ctr_cfg_alias;

static const ctr_cfg_alias ctr_cfg_aliases[] = {
    {"particles", "full", "max_sprites", "2176"},
    {"particles", "full", "sprite_max_size", "0"},
    {"particles", "full", "sprite_min_alpha", "2"},
    {"particles", "reduced", "max_sprites", "1000"},
    {"particles", "reduced", "sprite_max_size", "0.5"},
    {"particles", "reduced", "sprite_min_alpha", "6"},
    {"text_size", NULL, "text_scale", NULL},
};

/* The defaults of the keys read through ctr_config_get (3DS; the renderer's own keys have theirs
 * in CtrSettings.h). The callers' own fallbacks only matter without this table. */
static const ctr_cfg_kv ctr_cfg_defaults[] = {
    {"sound", "on"},
    {"gpu_micro", "on"}, {"shader_resident", "on"}, {"gpu_micro_ident", "off"}, /* micro-gpu */
    {"sound_core", "auto"}, /* 2 on a New 3DS, else 1 */
    {"args", "-boot -cbackend"},
    {"debug_log", "off"},
    {"listener", "off"},
    {"screenshots", "0"},
    {"pad_script", ""},
    {"perf_sections", "off"},
    {"mips2c_native_off", "off"},
    {"asm_mem", "on"},
    {"fast_clock", "on"},
    {"log_async", "on"},
    {"io_cache", "on"},
    {"io_checksum", "off"},
    {"c3l_read_buffer", "256"},
    {"bones_native", "on"},
    {"particle_density", "0.7"},
    {"collide_native", "on"},
    {"collide_frag_cache", "on"},
    {"anim_lod", "on"},
    {"anim_native", "on"},
    {"merc_lod_scale", "1"},
    {"ocean", "full"},
    {"shadows", "full"},
    {"depth16", "off"},
    {"text_scale", "1"},
    {"text_min_height", "9"},
    {"button_art", "off"},
};

/* Keys that were removed (with their code): ignored with a note, not an "unknown setting" */
static const char* const ctr_cfg_removed[] = {
    "new_shaders", "shader_groups", "shader_mesh", "shader_skin", "cb_read_opaque",
    "io_on_system_core",
};

/* config.ini's value of a key (its last line), NULL: no line. */
typedef const char* (*ctr_cfg_get_fn)(void* ctx, const char* key);

static inline const ctr_cfg_preset* ctr_cfg_preset_named(const char* name) {
  for (int i = 0; name && i < CTR_CFG_N(ctr_cfg_presets); i++) {
    if (strcmp(ctr_cfg_presets[i].name, name) == 0) {
      return &ctr_cfg_presets[i];
    }
  }
  return NULL;
}

/* The preset config.ini names (normal without a line or with an unknown name). */
static inline const ctr_cfg_preset* ctr_cfg_preset_of(ctr_cfg_get_fn get, void* ctx) {
  const ctr_cfg_preset* p = ctr_cfg_preset_named(get(ctx, "preset"));
  return p ? p : &ctr_cfg_presets[0];
}

static inline int ctr_cfg_is_user_alias(const char* key) {
  for (int i = 0; i < CTR_CFG_N(ctr_cfg_aliases); i++) {
    if (strcmp(ctr_cfg_aliases[i].user_key, key) == 0) {
      return 1;
    }
  }
  return 0;
}

static inline int ctr_cfg_is_removed(const char* key) {
  for (int i = 0; i < CTR_CFG_N(ctr_cfg_removed); i++) {
    if (strcmp(ctr_cfg_removed[i], key) == 0) {
      return 1;
    }
  }
  return 0;
}

/* A key's value (see the top). from_file 0: without config.ini's lines for it (theirs and its
 * user key's): what the preset and the defaults give. NULL: none of them sets it. */
static inline const char* ctr_cfg_resolve(const char* key, ctr_cfg_get_fn get, void* ctx,
                                          int from_file) {
  if (from_file) {
    const char* v = get(ctx, key);
    if (v) {
      return v;
    }
    for (int i = 0; i < CTR_CFG_N(ctr_cfg_aliases); i++) {
      const ctr_cfg_alias* a = &ctr_cfg_aliases[i];
      if (strcmp(a->key, key) == 0) {
        const char* uv = get(ctx, a->user_key);
        if (uv && (!a->user_value || strcmp(uv, a->user_value) == 0)) {
          return a->value ? a->value : uv;
        }
      }
    }
  }
  const ctr_cfg_preset* p = ctr_cfg_preset_of(get, ctx);
  for (int i = 0; i < p->n; i++) {
    if (strcmp(p->kv[i].key, key) == 0) {
      return p->kv[i].value;
    }
  }
  for (int i = 0; i < CTR_CFG_N(ctr_cfg_defaults); i++) {
    if (strcmp(ctr_cfg_defaults[i].key, key) == 0) {
      return ctr_cfg_defaults[i].value;
    }
  }
  return NULL;
}

/* A user key's value is one its aliases know (any for text_size). */
static inline int ctr_cfg_alias_value_ok(const char* user_key, const char* value) {
  for (int i = 0; i < CTR_CFG_N(ctr_cfg_aliases); i++) {
    const ctr_cfg_alias* a = &ctr_cfg_aliases[i];
    if (strcmp(a->user_key, user_key) == 0 && (!a->user_value || strcmp(a->user_value, value) == 0)) {
      return 1;
    }
  }
  return 0;
}

/* The same setting: on / 1 / true / yes alike, off / 0 / false / no alike, numbers by value. */
static inline int ctr_cfg_same(const char* a, const char* b) {
  if (!a || !b) {
    return a == b;
  }
  const char* const on[] = {"on", "1", "true", "yes"};
  const char* const off[] = {"off", "0", "false", "no"};
  int ka = 0, kb = 0;
  for (int i = 0; i < 4; i++) {
    ka = ka ? ka : (strcmp(a, on[i]) == 0 ? 1 : (strcmp(a, off[i]) == 0 ? 2 : 0));
    kb = kb ? kb : (strcmp(b, on[i]) == 0 ? 1 : (strcmp(b, off[i]) == 0 ? 2 : 0));
  }
  if (ka && kb) {
    return ka == kb;
  }
  char *ea, *eb;
  const double da = strtod(a, &ea), db = strtod(b, &eb);
  if (ea != a && !*ea && eb != b && !*eb) {
    return da == db;
  }
  return strcmp(a, b) == 0;
}
