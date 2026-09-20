/* Standalone tests for filters.c -- no Windows, no emulator core needed.
 *
 *   cd win32/tests
 *
 *   Without xBRZ (pure C, the 8 built-in filters):
 *     gcc -std=gnu99 -O1 -g -Wall -Wextra -I.. -fsanitize=address,undefined \
 *         -fno-sanitize-recover=undefined filters_test.c -o filters_test -lm
 *
 *   With the xBRZ add-on unpacked as ../xbrz (adds 5 filters and the
 *   xBRZ-specific checks):
 *     gcc -std=gnu99 -O1 -g -Wall -Wextra -I.. -I../xbrz -DGPGX_XBRZ -c filters_test.c -fsanitize=address,undefined
 *     g++ -std=gnu++11 -O1 -g -I../xbrz -fno-exceptions -fno-rtti -DNDEBUG -fsanitize=address,undefined \
 *         -c ../xbrz/xbrz_glue.cpp ../xbrz/xbrz.cpp
 *     g++ -fsanitize=address,undefined filters_test.o xbrz_glue.o xbrz.o -o filters_test -lm
 *
 *   ./filters_test
 */
#include "../filters.c"
#include <stdio.h>
#include <math.h>

static unsigned long rng_s = 12345;
static unsigned rnd(void){ rng_s = rng_s*6364136223846793005UL + 1442695040888963407UL; return (unsigned)(rng_s>>33); }

static int fails;
#define CHECK(c, ...) do{ if(!(c)){ fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } }while(0)

static void test_mix(void)
{
  int i; long worst = 0;
  for (i = 0; i < 2000000; i++)
  {
    px_t a = rnd() & 0xFFFF, b = rnd() & 0xFFFF; unsigned al = rnd() % 33;
    px_t m = mix(a, b, al);
    int ac[3] = { a>>11, (a>>5)&63, a&31 }, bc[3] = { b>>11, (b>>5)&63, b&31 }, mc[3] = { m>>11, (m>>5)&63, m&31 };
    int k;
    for (k = 0; k < 3; k++)
    {
      double ref = ac[k] + (bc[k]-ac[k]) * (al/32.0);
      long err = lround(fabs(mc[k] - ref) * 1000);
      if (err > worst) worst = err;
      /* result must lie between the two inputs (inclusive) */
      int lo = ac[k] < bc[k] ? ac[k] : bc[k], hi = ac[k] < bc[k] ? bc[k] : ac[k];
      CHECK(mc[k] >= lo && mc[k] <= hi, "mix out of range a=%04x b=%04x al=%u k=%d got=%d", a, b, al, k, mc[k]);
    }
    if (al == 0)  CHECK(m == a, "mix alpha 0 must return a");
    if (al == 32) CHECK(m == b, "mix alpha 32 must return b");
  }
  printf("mix: worst error vs float reference = %.3f LSB (2M random trials)\n", worst/1000.0);
  CHECK(worst < 1000, "mix error >= 1 LSB");
}

static void test_dim(void)
{
  int c; long worst = 0;
  for (c = 0; c < 65536; c++)
  {
    px_t d = dim58((px_t)c);
    int ac[3] = { c>>11, (c>>5)&63, c&31 }, dc[3] = { d>>11, (d>>5)&63, d&31 };
    int k;
    for (k = 0; k < 3; k++)
    {
      double ref = ac[k] * 0.625;
      long err = lround(fabs(dc[k] - ref) * 1000);
      if (err > worst) worst = err;
      CHECK(dc[k] <= ac[k], "dim brightened");
    }
  }
  printf("dim58: worst error vs x0.625 = %.3f LSB (all 65536 colours)\n", worst/1000.0);
}

/* Runs one filter on a WxH frame in exactly-sized heap buffers so ASan catches
   any read or write outside them. */
static void run_size(int fi, int w, int h, int mode)
{
  const filter_def_t *f = filter_get(fi);
  size_t sp = (size_t)w * 2, dp = (size_t)w * f->scale * 2;
  px_t *src = malloc(sp * h), *dst = malloc(dp * h * f->scale), *copy = malloc(sp * h);
  int x, y, k, r;
  for (y = 0; y < h; y++) for (x = 0; x < w; x++)
  {
    px_t v;
    switch (mode) {
      case 0: v = 0x1234; break;                           /* flat */
      case 1: v = rnd() & 0xFFFF; break;                   /* noise */
      case 2: v = ((x/3 + y/3) & 1) ? 0xFFFF : 0x0000; break; /* checker */
      default: v = (x + y) % 5 == 0 ? 0xF800 : 0x001F;     /* diagonal-ish */
    }
    src[y*w + x] = v;
  }
  memcpy(copy, src, sp * h);
  {
    /* Run twice into buffers pre-filled with different bytes. Any pixel the
       filter fails to write keeps its (different) fill value and shows up as
       a mismatch; a legitimately random pixel value cannot fool this. */
    size_t bytes = dp * h * f->scale;
    px_t *dst2 = malloc(bytes);
    memset(dst,  0x00, bytes);
    memset(dst2, 0xFF, bytes);
    r = filter_apply(fi, src, (int)sp, w, h, dst, (int)dp);
    CHECK(r == f->scale, "%s %dx%d returned scale %d", f->name, w, h, r);
    filter_apply(fi, src, (int)sp, w, h, dst2, (int)dp);
    CHECK(!memcmp(dst, dst2, bytes), "%s %dx%d mode %d: output not fully/deterministically written", f->name, w, h, mode);
    free(dst2);
  }
  CHECK(!memcmp(src, copy, sp * h), "%s modified its input", f->name);
  if (mode == 0 && fi != filter_find("Scanlines 2x") && fi != filter_find("CRT 3x (RGB mask)"))
  {
    for (k = 0; k < w * f->scale * h * f->scale; k++)
      if (dst[k] != 0x1234) { CHECK(0, "%s: flat input changed at %d (%04x)", f->name, k, dst[k]); break; }
  }
  free(src); free(dst); free(copy);
}


/* Behavioural checks for the Smooth filters: things that must NOT be smeared. */
static void test_smooth_preserves(void)
{
  int variant;
  for (variant = 0; variant < 2; variant++)
  {
    int fi = filter_find(variant ? "Smooth 4x (xBR-style)" : "Smooth 2x (xBR-style)");
    int sc = variant ? 4 : 2;
    enum { W = 24, H = 24 };
    px_t src[H][W], *dst = malloc((size_t)W*sc*H*sc*2);
    int x, y, bad;

    /* 1. isolated single pixel on a flat field must come out as a pure block */
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) src[y][x] = 0x4A9F;
    src[12][12] = 0x0000;
    filter_apply(fi, &src[0][0], W*2, W, H, dst, W*sc*2);
    bad = 0;
    for (y = 0; y < H*sc; y++) for (x = 0; x < W*sc; x++)
    {
      int inside = (x/sc == 12 && y/sc == 12);
      px_t want = inside ? 0x0000 : 0x4A9F;
      if (dst[y*W*sc + x] != want) bad++;
    }
    /* 2x: must be exact. 4x: the cascade may round the block's four corner
       sub-pixels (same as Scale2x does to any 2x2+ square) and nothing else. */
    CHECK(bad <= (sc == 4 ? 4 : 0), "%s eroded an isolated pixel (%d bad)", filter_get(fi)->name, bad);
    if (sc == 4)
    {
      int inner_bad = 0;
      for (y = 12*sc; y < 13*sc; y++) for (x = 12*sc; x < 13*sc; x++)
      {
        int corner = ((x == 12*sc || x == 13*sc-1) && (y == 12*sc || y == 13*sc-1));
        if (!corner && dst[y*W*sc + x] != 0x0000) inner_bad++;
      }
      CHECK(inner_bad == 0, "%s damaged the interior of an isolated pixel", filter_get(fi)->name);
    }

    /* 2. dither checkerboard of two close-but-distinct colours is untouched */
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) src[y][x] = ((x+y)&1) ? 0xF800 : 0xA000;
    filter_apply(fi, &src[0][0], W*2, W, H, dst, W*sc*2);
    bad = 0;
    for (y = 0; y < H*sc; y++) for (x = 0; x < W*sc; x++)
      if (dst[y*W*sc + x] != src[y/sc][x/sc]) bad++;
    CHECK(bad == 0, "%s altered a dither checkerboard (%d bad)", filter_get(fi)->name, bad);

    /* 3. a straight vertical edge stays perfectly straight and hard */
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) src[y][x] = (x < 12) ? 0x001F : 0xFFE0;
    filter_apply(fi, &src[0][0], W*2, W, H, dst, W*sc*2);
    bad = 0;
    for (y = 0; y < H*sc; y++) for (x = 0; x < W*sc; x++)
      if (dst[y*W*sc + x] != src[y/sc][x/sc]) bad++;
    CHECK(bad == 0, "%s changed a straight vertical edge (%d bad)", filter_get(fi)->name, bad);

    /* 4. ...and so does a straight horizontal one */
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) src[y][x] = (y < 12) ? 0x001F : 0xFFE0;
    filter_apply(fi, &src[0][0], W*2, W, H, dst, W*sc*2);
    bad = 0;
    for (y = 0; y < H*sc; y++) for (x = 0; x < W*sc; x++)
      if (dst[y*W*sc + x] != src[y/sc][x/sc]) bad++;
    CHECK(bad == 0, "%s changed a straight horizontal edge (%d bad)", filter_get(fi)->name, bad);

    /* 5. a 45-degree staircase must actually change (otherwise it does nothing) */
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) src[y][x] = (x < y) ? 0x001F : 0xFFE0;
    filter_apply(fi, &src[0][0], W*2, W, H, dst, W*sc*2);
    bad = 0;
    for (y = 0; y < H*sc; y++) for (x = 0; x < W*sc; x++)
      if (dst[y*W*sc + x] != src[y/sc][x/sc]) bad++;
    CHECK(bad > 0, "%s left a 45-degree edge completely unsmoothed", filter_get(fi)->name);
    printf("%s: 45-degree edge modified %d sub-pixels\n", filter_get(fi)->name, bad);
    free(dst);
  }
}

#ifdef GPGX_XBRZ
/* Every possible RGB565 value, as a flat block, must come out of xBRZ exactly
   as it went in: the 565 -> 888 -> 565 round trip is the identity, and xBRZ
   itself leaves flat areas alone. */
static void test_xbrz_roundtrip(void)
{
  int fi = filter_find("xBRZ 2x"), c, bad = 0;
  enum { N = 6 };
  px_t src[N * N], dst[(2 * N) * (2 * N)];
  int i;

  for (c = 0; c < 65536; c++)
  {
    for (i = 0; i < N * N; i++) src[i] = (px_t)c;
    memset(dst, 0, sizeof(dst));
    filter_apply(fi, src, N * 2, N, N, dst, 2 * N * 2);
    for (i = 0; i < (2 * N) * (2 * N); i++)
      if (dst[i] != (px_t)c) { bad++; break; }
  }
  CHECK(bad == 0, "xBRZ flat-colour round trip changed %d of 65536 colours", bad);
  printf("xBRZ: all 65536 RGB565 colours survive the round trip (bad = %d)\n", bad);
}

/* Selecting filters must give the memory back when moving away from xBRZ. */
static void test_xbrz_prepare(void)
{
  int x = filter_find("xBRZ 3x"), s = filter_find("Scale2x");
  CHECK(filter_prepare(x) == 1, "prepare xBRZ");
  CHECK(filter_prepare(s) == 1, "prepare non-xBRZ (releases the table)");
  CHECK(filter_prepare(-1) == 1, "prepare none");
  CHECK(filter_prepare(x) == 1, "prepare xBRZ again after a release");
  {
    px_t src[16*16], dst[48*48]; int i;
    for (i = 0; i < 16*16; i++) src[i] = (i % 16 < 8) ? 0x001F : 0xFFE0;
    CHECK(filter_apply(x, src, 32, 16, 16, dst, 96) == 3, "xBRZ works after re-prepare");
  }
}
#endif

int main(void)
{
  int fi, mode, i;
  static const int sizes[][2] = { {1,1},{1,7},{7,1},{2,2},{3,5},{13,11},{256,192},{320,224},{320,240},{256,224},{320,448},{720,576} };
  test_mix();
  test_dim();
  test_smooth_preserves();
#ifdef GPGX_XBRZ
  test_xbrz_roundtrip();
  test_xbrz_prepare();
#endif
#ifdef GPGX_XBRZ
  CHECK(filter_count() == 13, "filter_count (with xBRZ)");
#else
  CHECK(filter_count() == 8, "filter_count");
#endif
  CHECK(filter_find("scale2x.RPI") == 0, "legacy .rpi name lookup");
  CHECK(filter_find("Smooth 2x (xBR-style)") == 3, "exact lookup");
  CHECK(filter_find("nonsense") == -1 && filter_find("") == -1 && filter_find(NULL) == -1, "unknown names");
  CHECK(filter_apply(0, NULL, 0, 4, 4, NULL, 0) == 0, "null args");
  CHECK(filter_apply(99, (void*)1, 0, 4, 4, (void*)1, 0) == 0, "bad index");
  CHECK(filter_apply(0, (void*)1, 0, FILTER_MAX_W + 1, 4, (void*)1, 0) == 0, "oversize rejected");
  for (fi = 0; fi < filter_count(); fi++)
  {
    for (mode = 0; mode < 4; mode++)
      for (i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); i++)
        run_size(fi, sizes[i][0], sizes[i][1], mode);
    printf("ran %-24s on %d sizes x 4 patterns\n", filter_get(fi)->name, (int)(sizeof(sizes)/sizeof(sizes[0])));
  }
  filter_shutdown();
  printf(fails ? "\n%d FAILURES\n" : "\nALL PASSED\n", fails);
  return fails != 0;
}
