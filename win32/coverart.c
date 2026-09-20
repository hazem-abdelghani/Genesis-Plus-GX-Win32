/****************************************************************************
 *  Genesis Plus GX -- Win32 GUI frontend
 *
 *  coverart.c -- see coverart.h. Decoding is stb_image (public domain,
 *  single header, bundled in this directory) rather than GDI+/WIC: both
 *  of those are COM/C++-flavored APIs that are painful to drive cleanly
 *  from plain C under MinGW, where this whole frontend already lives.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>

#include "shared.h"
#include "gui.h"
#include "coverart.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "stb_image.h"

/* Tried in this order against the same base name; whichever exists
   first wins. PNG first since that's what the feature was asked for
   and what coverart_set writes by default. */
static const char *cover_extensions[] = { ".png", ".jpg", ".jpeg", ".bmp", ".gif" };
#define COVER_EXT_COUNT (int)(sizeof(cover_extensions) / sizeof(cover_extensions[0]))

/* Same base-name extraction main.c's split_basename does (strip
   directory and extension), kept as its own small copy here rather
   than exposing that static function across files for one caller. */
static void base_name_only(const char *path, char *out, int out_len)
{
  const char *slash, *dot;
  const char *name;
  int len;

  slash = strrchr(path, '\\');
  name = slash ? slash + 1 : path;

  dot = strrchr(name, '.');
  len = dot ? (int)(dot - name) : (int)lstrlenA(name);
  if (len >= out_len) len = out_len - 1;

  memcpy(out, name, (size_t)len);
  out[len] = '\0';
}

static void covers_dir(char *out, int out_len)
{
  lstrcpynA(out, osd_path("Covers"), out_len);
}

void coverart_path_for_rom(const char *rom_full_path, char *out, int out_len)
{
  char dir[GUI_PATH_LEN];
  char base[160];

  covers_dir(dir, sizeof(dir));
  base_name_only(rom_full_path, base, sizeof(base));
  wsprintfA(out, "%s\\%s.png", dir, base);
  (void)out_len;
}

static void build_candidate(const char *dir, const char *base, int ext_index, char *out, int out_len)
{
  wsprintfA(out, "%s\\%s%s", dir, base, cover_extensions[ext_index]);
  (void)out_len;
}

int coverart_find_cached(const char *rom_full_path, signed char *cache, char *found_path, int out_len)
{
  char dir[GUI_PATH_LEN];
  char base[160];
  int i;

  if (*cache == -1) return 0;   /* already known: no cover on disk for this ROM */

  if (*cache >= 0)   /* already known: which extension matched -- just rebuild the path, no disk touch */
  {
    covers_dir(dir, sizeof(dir));
    base_name_only(rom_full_path, base, sizeof(base));
    build_candidate(dir, base, *cache, found_path, out_len);
    return 1;
  }

  /* COVERART_UNCHECKED: first time this ROM has been looked up -- do the
     real filesystem search once and remember the result either way. */
  covers_dir(dir, sizeof(dir));
  base_name_only(rom_full_path, base, sizeof(base));

  for (i = 0; i < COVER_EXT_COUNT; i++)
  {
    char candidate[GUI_PATH_LEN];
    build_candidate(dir, base, i, candidate, sizeof(candidate));
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES)
    {
      *cache = (signed char)i;
      lstrcpynA(found_path, candidate, out_len);
      return 1;
    }
  }

  *cache = -1;
  return 0;
}

int coverart_find(const char *rom_full_path, char *found_path, int out_len)
{
  signed char throwaway = COVERART_UNCHECKED;
  return coverart_find_cached(rom_full_path, &throwaway, found_path, out_len);
}

/* Simple nearest-neighbor letterbox fit into a size x size square --
   thumbnails in a grid are small enough that this looks fine, and it
   keeps this file's one job (get a cover into a control) from growing
   into a general-purpose image resampler. Padding is left fully
   transparent (alpha 0) so the control's own background shows through
   whatever the source image's own aspect ratio doesn't fill. */
static HBITMAP load_and_fit(const char *path, int size)
{
  int src_w, src_h, channels;
  unsigned char *pixels;
  HBITMAP hbmp;
  BITMAPINFO bmi;
  unsigned char *dib_bits;
  int fit_w, fit_h, off_x, off_y;
  int x, y;

  pixels = stbi_load(path, &src_w, &src_h, &channels, 4);
  if (!pixels) return NULL;

  if (src_w >= src_h)
  {
    fit_w = size;
    fit_h = (int)((long)size * src_h / src_w);
  }
  else
  {
    fit_h = size;
    fit_w = (int)((long)size * src_w / src_h);
  }
  if (fit_w < 1) fit_w = 1;
  if (fit_h < 1) fit_h = 1;
  off_x = (size - fit_w) / 2;
  off_y = (size - fit_h) / 2;

  ZeroMemory(&bmi, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = size;
  bmi.bmiHeader.biHeight = -size;   /* negative = top-down, matches how we fill it below */
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&dib_bits, NULL, 0);
  if (!hbmp)
  {
    stbi_image_free(pixels);
    return NULL;
  }

  ZeroMemory(dib_bits, (size_t)size * size * 4);   /* transparent letterbox bars */

  for (y = 0; y < fit_h; y++)
  {
    int src_y = y * src_h / fit_h;
    unsigned char *dst_row = dib_bits + ((size_t)(off_y + y) * size + off_x) * 4;
    const unsigned char *src_row = pixels + (size_t)src_y * src_w * 4;

    for (x = 0; x < fit_w; x++)
    {
      int src_x = x * src_w / fit_w;
      const unsigned char *sp = src_row + (size_t)src_x * 4;
      unsigned char *dp = dst_row + (size_t)x * 4;

      /* stb_image gives RGBA; DIBs want BGRA. */
      dp[0] = sp[2];
      dp[1] = sp[1];
      dp[2] = sp[0];
      dp[3] = sp[3];
    }
  }

  stbi_image_free(pixels);
  return hbmp;
}

HBITMAP coverart_load(const char *rom_full_path, int size)
{
  char path[GUI_PATH_LEN];
  if (!coverart_find(rom_full_path, path, sizeof(path))) return NULL;
  return load_and_fit(path, size);
}

HBITMAP coverart_load_cached(const char *rom_full_path, int size, signed char *cache)
{
  char path[GUI_PATH_LEN];
  if (!coverart_find_cached(rom_full_path, cache, path, sizeof(path))) return NULL;
  return load_and_fit(path, size);
}

int coverart_set(const char *rom_full_path, const char *source_image_path)
{
  char dir[GUI_PATH_LEN];
  char base[160];
  const char *src_dot;
  char dest[GUI_PATH_LEN];

  covers_dir(dir, sizeof(dir));
  CreateDirectoryA(dir, NULL);   /* fine if it already exists -- error is only checked on the copy itself */

  coverart_remove(rom_full_path);   /* clear any existing cover under a different extension first */

  base_name_only(rom_full_path, base, sizeof(base));
  src_dot = strrchr(source_image_path, '.');
  wsprintfA(dest, "%s\\%s%s", dir, base, src_dot ? src_dot : ".png");

  return CopyFileA(source_image_path, dest, FALSE) ? 1 : 0;
}

int coverart_remove(const char *rom_full_path)
{
  char path[GUI_PATH_LEN];

  if (!coverart_find(rom_full_path, path, sizeof(path))) return 0;
  return DeleteFileA(path) ? 1 : 0;
}
