/* (AI-assisted) draw_cost: the level draws of ctr_gpu_citro3d.c under Unicorn (run.sh): the raw
 * path (mode 0 the state alone, 1 whole draws) and the wind draws (mode 2) on made-up states,
 * textures and chunks; the instructions per call (h_mark) and the command words written. */
#include GPU_C
#include <reent.h>
extern const u8 ctr_mesh_wind_shbin[];
extern const size_t ctr_mesh_wind_shbin_size;

#define HJOB ((volatile u32*)0x0f000000)
static u32 s_cmd[1 << 20] __attribute__((aligned(16)));
static u8 s_texels[64 * 1024] __attribute__((aligned(128)));
static ctr_draw_state s_states[64];
static ctr_mesh_matrix s_mtx[16];

__attribute__((noinline, used)) void h_mark(int k) {
  asm volatile("" ::"r"(k) : "memory");
}
__attribute__((noinline, used)) void h_done(void) {
  for (;;) {
    asm volatile("" ::: "memory");
  }
}
static u32 s_rng = 12345;
static u32 rnd(void) {
  s_rng = s_rng * 1664525u + 1013904223u;
  return s_rng >> 8;
}

static void setup(void) {
  memset(&__C3D_Context, 0, sizeof(__C3D_Context));
  __C3D_Context.flags = C3DiF_Active;
  __C3D_Context.bufInfo.base_paddr = RAW_BUF_BASE;
  gpuCmdBuf = s_cmd;
  gpuCmdBufSize = sizeof(s_cmd) / 4;
  gpuCmdBufOffset = 0;
  g.ready = 1;
  s_raw.dedup_draw = 1;
  g.in_frame = 1;
  g.ps = &s_ps[1];
  s_ps[1].ok = 1;
  s_ps[1].opt = 1;
  s_ps[1].fa_ok = 1;
  s_ps[1].uloc_clip = 0;
  s_ps[1].uloc_scales = 6;
  s_ps[1].uloc_fog1 = 7;
  g.cur_prog = PROG_MESH_FA;
  g.fog_tex_valid = 1;
  g.fog_abgr = 0xff406080u;
  g.fog_gen = 1;
  g_fog_alpha = 1;
  g_tex1_gating = 1;
  g_cull_enable = 1;
  __C3D_Context.texConfig = 3; /* units 0, 1 on (the fog ramp) */
  for (int t = 0; t < 24; t++) {
    TexSlot* s = &g.textures[t];
    s->used = 1;
    s->pool = t & 1 ? 0 : -1;
    s->levels = 1 + (t % 3);
    s->alpha_full = (t >> 2) & 1;
    s->tex.data = s_texels + 2048 * t;
    s->tex.fmt = t & 2 ? GPU_ETC1A4 : GPU_RGBA8;
    s->tex.width = s->tex.height = 32;
    s->tex.param = GPU_TEXTURE_MAG_FILTER(GPU_LINEAR);
  }
  for (int m = 0; m < 16; m++) {
    MeshSlot* ms = &g.meshes[m];
    ms->used = 1;
    ms->verts = (void*)(0x30100000u + 0x10000u * (u32)m);
    ms->indices = (uint16_t*)((u8*)ms->verts + 0x8000u);
    ms->verts_pa = osConvertVirtToPhys(ms->verts);
    ms->indices_pa = osConvertVirtToPhys(ms->indices);
    ms->col_off = 0x4000u;
    float clip[16];
    for (int k = 0; k < 16; k++) {
      clip[k] = (float)(int)(rnd() % 2000) / 100.f - 10.f;
    }
    memcpy(s_mtx[m].m, clip, sizeof(clip));
    for (int r = 0; r < 4; r++) {
      raw_pack_f24(s_mtx[m].m + 4 * r, s_mtx[m].f24 + 3 * r);
    }
    s_mtx[m].id = (u32)m + 1;
    s_mtx[m].fog_gen = 0;
  }
  /* level states: the fog in the alpha (mesh 5) or the ramp (1), opaque or alpha tested, cull,
   * blended ones */
  for (int i = 0; i < 64; i++) {
    ctr_draw_state* st = &s_states[i];
    memset(st, 0, sizeof(*st));
    st->tex = (int)(rnd() % 24);
    st->tcc = (u8)((rnd() & 1) | ((rnd() & 1) ? CTR_TCC_CULL_BACK : 0) |
                   ((rnd() % 4) ? CTR_TCC_ALPHA80 : 0) | ((rnd() & 1) ? CTR_TCC_TRILINEAR : 0));
    st->filter = 1;
    st->clamp_s = (u8)(rnd() & 1);
    st->clamp_t = st->clamp_s;
    st->blend = (u8)((rnd() % 5) ? CTR_BLEND_OFF : CTR_BLEND_ALPHA);
    st->atest = (u8)((rnd() % 3) ? CTR_TEST_ALWAYS : CTR_TEST_GEQUAL);
    st->aref = 0x26;
    st->ztest = CTR_TEST_GEQUAL;
    st->zwrite = st->blend == CTR_BLEND_OFF;
    st->flags = (u8)((rnd() & 1) ? CTR_STATE_EARLY_DEPTH : 0);
  }
}

static float s_rows[16][4 * 17];
/* wind draws: [4] 1 = a chunk's draws together (sorted), 0 = the chunks in turn */
static void wind_seq(int n, int sorted) {
  DVLB_s* d = DVLB_ParseFile((u32*)ctr_mesh_wind_shbin, (u32)ctr_mesh_wind_shbin_size);
  shaderProgramInit(&s_ps[1].mesh_wind);
  shaderProgramSetVsh(&s_ps[1].mesh_wind, &d->DVLE[0]);
  s_ps[1].uloc_wind = shaderInstanceGetUniformLocation(s_ps[1].mesh_wind.vertexShader, "wind");
  s_ps[1].uloc_clip = shaderInstanceGetUniformLocation(s_ps[1].mesh_wind.vertexShader, "clip");
  s_ps[1].uloc_scales = shaderInstanceGetUniformLocation(s_ps[1].mesh_wind.vertexShader, "scales");
  s_ps[1].wind_ok = 1;
  g.cur_prog = PROG_NONE;
  for (int m = 0; m < 16; m++) {
    for (int k = 0; k < 4 * 17; k++) {
      s_rows[m][k] = (float)(int)(rnd() % 100) / 50.f;
    }
  }
  for (int i = 0; i < n; i++) {
    const int chunk = sorted ? (i / 8) % 16 : i % 16;
    const ctr_draw_state* st = &s_states[rnd() % 40];
    h_mark(1);
    ctr_gpu_draw_mesh_wind(st, &s_mtx[chunk], s_rows[chunk], 17, (u32)chunk + 1, chunk, 15,
                           0x7000, 3 * (int)(rnd() % 100), 300);
    h_mark(2);
    h_mark(12);
  }
}

/* job: [0] mode (0: apply_state_tint sequence, 1: level calls), [1] count, [2] seed, [3] states */
void h_main(void) {
  setup();
  s_rng = HJOB[2];
  const int mode = (int)HJOB[0], n = (int)HJOB[1], nst = (int)HJOB[3];
  if (mode == 2) {
    wind_seq(n, (int)HJOB[4]);
    HJOB[8] = gpuCmdBufOffset;
    HJOB[9] = (u32)(uintptr_t)gpuCmdBuf;
    return;
  }
  int cur = 0, mesh = 0;
  for (int i = 0; i < n; i++) {
    /* runs: mostly the same state, sometimes another (texture or state), chunks switching */
    const u32 r = rnd() % 100;
    if (r < 35) {
      cur = (int)(rnd() % (u32)nst);
    } else if (r < 45) {
      int t = s_states[cur].tex;
      s_states[cur].tex = (int)(rnd() % 24);
      (void)t;
    }
    if (rnd() % 100 < 70) {
      mesh = (int)(rnd() % 16);
    }
    const ctr_draw_state* st = &s_states[cur];
    const int fa0 = (st->tcc & CTR_TCC_ALPHA80) != 0;
    const int same = g.last_state_valid && g.last_state_mesh == (fa0 ? 5 : 1) &&
                     g.last_tint == 0xffffffffu && words_equal(&g.last_state, st, 4);
    const u32 fast0 = s_state_fast;
    if (mode == 0) {
      const int fa = (st->tcc & CTR_TCC_ALPHA80) != 0;
      g.cur_prog = fa ? PROG_MESH_FA : PROG_MESH;
      h_mark(1);
      apply_state_tint(st, fa ? 5 : 1, 0xffffffffu, 1);
      h_mark(2);
    } else {
      if (!(st->tcc & CTR_TCC_ALPHA80)) {
        continue;
      }
      h_mark(1);
      ctr_gpu_draw_mesh_prepared(st, &s_mtx[mesh], mesh, 3 * (int)(rnd() % 100), 300);
      h_mark(2);
    }
    const int kind = same ? 0 : s_state_fast != fast0 ? 1 : 2;
    h_mark(10 + kind);
  }
  HJOB[8] = gpuCmdBufOffset;
  HJOB[9] = (u32)(uintptr_t)gpuCmdBuf;
  /* the final context the GPU should have */
  memcpy((void*)(HJOB + 16), &__C3D_Context.texEnv[0], 4 * sizeof(C3D_TexEnv));
  memcpy((void*)(HJOB + 64), &__C3D_Context.effect, sizeof(C3Di_Effect));
  HJOB[10] = __C3D_Context.texConfig;
}

__attribute__((naked, used, section(".text.start"))) void _start(void) {
  asm volatile(
      "ldr sp, =0x0e100000\n"
      "bl h_main\n"
      "b h_done\n");
}
struct _reent* __getreent(void) {
  return _impure_ptr;
}
