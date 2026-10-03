/* (AI-assisted) Host test for game/sce/sce_memfile.h, the 3DS's in-memory GOAL file streams: random
 * reads, seeks and writes against the same on a stdio FILE (what the PC uses, and the 3DS did),
 * comparing every return value, the bytes read and the file written. Build from the repo root:
 *   c++ -std=c++17 -O2 -I. platform/3ds/tests/memfile_test.cpp -o /tmp/memfile_test && /tmp/memfile_test
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/sce/sce_memfile.h"

static int g_fail = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      if (g_fail++ < 20) {                                    \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      }                                                       \
    }                                                         \
  } while (0)

static unsigned rnd_state = 777;
static unsigned rnd() {
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return rnd_state >> 8;
}

static const int kWhence[3] = {SEEK_SET, SEEK_CUR, SEEK_END};

// a file opened for reading: kread / kseek / file-stream-length / the settings parser's pattern
static void test_read(const char* path) {
  for (int round = 0; round < 200; round++) {
    std::vector<u8> content(rnd() % 3000);
    for (auto& b : content) {
      b = (u8)rnd();
    }
    FILE* w = fopen(path, "wb");
    if (!content.empty()) {
      fwrite(content.data(), 1, content.size(), w);
    }
    fclose(w);
    FILE* fp = fopen(path, "rb");
    SceMemFile m;
    m.data = content;
    for (int op = 0; op < 500; op++) {
      const unsigned kind = rnd() % 3;
      if (kind == 0) {
        const s32 n = (s32)(rnd() % 1100);  // the parser reads 1024 at a time, getc reads 1
        u8 a[1100], b[1100];
        const s32 ra = (s32)fread(a, 1, (size_t)n, fp);
        const s32 rb = m.read(b, n);
        CHECK(ra == rb);
        CHECK(memcmp(a, b, (size_t)(ra < rb ? ra : rb)) == 0);
      } else {
        const int whence = (int)(rnd() % 3);
        const s32 off = (s32)(rnd() % (2 * content.size() + 40)) - (s32)content.size() - 20;
        fseek(fp, off, kWhence[whence]);
        const s32 pa = (s32)ftell(fp);
        const s32 pb = m.seek(off, whence);
        CHECK(pa == pb);
      }
      CHECK((s32)ftell(fp) == m.seek(0, 1));  // file-stream-tell
    }
    CHECK(m.write("x", 1) == 0);  // opened for reading
    fclose(fp);
  }
}

// a file opened for writing (format to a file-stream, kwrite), with seeks, compared at close
static void test_write(const char* path) {
  for (int round = 0; round < 200; round++) {
    FILE* fp = fopen(path, "wb");
    SceMemFile m;
    m.writable = true;
    for (int op = 0; op < 300; op++) {
      if (rnd() % 4) {
        u8 buf[300];
        const s32 n = (s32)(rnd() % 300);
        for (s32 i = 0; i < n; i++) {
          buf[i] = (u8)rnd();
        }
        const s32 wa = (s32)fwrite(buf, 1, (size_t)n, fp);
        const s32 wb = m.write(buf, n);
        CHECK(wa == wb);
      } else {
        const int whence = (int)(rnd() % 3);
        const s32 size = (s32)m.data.size();
        const s32 off = (s32)(rnd() % (2 * size + 200)) - size - 50;
        fseek(fp, off, kWhence[whence]);
        const s32 pa = (s32)ftell(fp);
        const s32 pb = m.seek(off, whence);
        CHECK(pa == pb);
      }
    }
    u8 tmp[4];
    CHECK(m.read(tmp, 4) == 0);  // opened for writing
    fclose(fp);
    FILE* r = fopen(path, "rb");
    std::vector<u8> got;
    int c;
    while ((c = fgetc(r)) != EOF) {
      got.push_back((u8)c);
    }
    fclose(r);
    CHECK(got == m.data);
  }
}

int main(int argc, char** argv) {
  // argv[1]: a directory for the temporary file (default /tmp)
  char path[1024];
  snprintf(path, sizeof(path), "%s/memfile_test_%u.bin", argc > 1 ? argv[1] : "/tmp", rnd());
  test_read(path);
  test_write(path);
  remove(path);
  printf(g_fail ? "memfile_test: %d FAILED\n" : "memfile_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
