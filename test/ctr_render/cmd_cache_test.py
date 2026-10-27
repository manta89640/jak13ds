#!/usr/bin/env python3
"""(AI-assisted) Compile the actual command-cache dispatcher with CPU-only GPU-state stubs.

Checks growth past eight stretches, reuse, freeing and allocation failure under ASan/UBSan.
No GPU backend, renderer, game or emulator is executed.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "game/graphics/ctr/ctr_gpu_citro3d.c").read_text()


def function(name):
    start = source.index("void " + name + "(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


page_size = re.search(r"#define CL_SEGS_PER_PAGE (\d+)", source).group(0)
pages = source[source.index("typedef struct ClPage {"):source.index("static struct {", source.index("typedef struct ClPage {"))]
prelude = r"""
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef unsigned int u32;
typedef struct { int hits; } ClSeg;
typedef struct { int tex; } ctr_draw_state;
typedef struct { const ctr_draw_state* state; int prog, plain; } ctr_mesh_call;
typedef struct ctr_cmd_cache ctr_cmd_cache;
static struct { int ready, in_frame; struct { int used, pool; } textures[1]; } g;
static struct { int on; } s_raw;
static struct { int mode; u32 head_why[6], normal_calls; } s_cl;
static int s_sky_hook, allocated, freed, fail_on, drawn, segments;
static void* test_calloc(size_t n, size_t bytes) {
  allocated++;
  return allocated == fail_on ? NULL : calloc(n, bytes);
}
static void test_free(void* p) { if (p) freed++; free(p); }
static int tex_res(int t) { return t; }
static int cl_plain(const ctr_mesh_call* c) { return c->plain; }
static int cl_prog(const ctr_mesh_call* c) { return c->prog; }
static int keeps_sky(const ctr_draw_state* s) { (void)s; return 0; }
static void cl_draw(const ctr_mesh_call* c, int a, int b) { (void)c; drawn += b - a; }
static void cl_segment(ClSeg* seg, const ctr_mesh_call* c, int a, int b) {
  (void)c; seg->hits++; segments++; drawn += b - a;
}
#define MAX_TEXTURES 1
#define calloc test_calloc
#define free test_free
"""
checks = r"""
#undef calloc
#undef free
int main(void) {
  ctr_draw_state state = {0};
  ctr_mesh_call calls[80];
  for (int i = 0; i < 80; i++) calls[i] = (ctr_mesh_call){&state, i / 4 % 2, 1};
  g.ready = g.in_frame = s_raw.on = s_cl.mode = 1;
  g.textures[0].used = 1;
  ctr_cmd_cache* cache = NULL;
  ctr_gpu_draw_mesh_calls(calls, 80, &cache);
  assert(drawn == 80 && segments == 20);
  int alloc0 = allocated, index = 0;
  for (ClPage* p = cache->pages; p; p = p->next) {
    for (int k = 0; k < CL_SEGS_PER_PAGE && index < 20; k++, index++) assert(p->seg[k].hits == 1);
  }
  assert(index == 20);
  drawn = segments = 0;
  ctr_gpu_draw_mesh_calls(calls, 80, &cache);
  assert(drawn == 80 && segments == 20 && allocated == alloc0);
  ctr_gpu_cmd_cache_free(&cache);
  assert(cache == NULL && freed == allocated);

  // Root + first page succeed; second page fails. Every remaining draw still executes.
  allocated = freed = drawn = segments = 0;
  fail_on = 3;
  ctr_gpu_draw_mesh_calls(calls, 80, &cache);
  assert(drawn == 80 && segments == CL_SEGS_PER_PAGE && allocated == 3);
  fail_on = 0;
  drawn = segments = 0;
  ctr_gpu_draw_mesh_calls(calls, 80, &cache);
  assert(drawn == 80 && segments == 20);
  ctr_gpu_cmd_cache_free(&cache);
  assert(cache == NULL && freed == allocated - 1);
  ctr_gpu_cmd_cache_free(&cache);

  // A failed root allocation falls back too, and can recover next call.
  allocated = freed = drawn = segments = 0;
  fail_on = 1;
  ctr_gpu_draw_mesh_calls(calls, 80, &cache);
  assert(cache == NULL && drawn == 80 && segments == 0);
  fail_on = 0;
  drawn = 0;
  ctr_gpu_draw_mesh_calls(calls, 80, &cache);
  assert(cache && drawn == 80 && segments == 20);
  ctr_gpu_cmd_cache_free(&cache);
  assert(freed == allocated - 1);
  puts("Command-cache paging regressions passed (CPU only, ASan/UBSan).");
  return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="ctr-cmd-cache-") as directory:
    path = Path(directory)
    code = prelude + page_size + "\n" + pages + function("ctr_gpu_draw_mesh_calls") + "\n"
    code += function("ctr_gpu_cmd_cache_free") + checks
    (path / "test.c").write_text(code)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")],
                   check=True)
    subprocess.run([str(path / "test")], check=True)
