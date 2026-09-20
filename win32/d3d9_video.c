/****************************************************************************
 *  Genesis Plus GX -- Win32 GUI frontend
 *
 *  d3d9_video.c -- see d3d9_video.h for the rationale.
 *
 *  Always windowed (D3DPRESENT_PARAMETERS.Windowed = TRUE), always
 *  D3DPRESENT_INTERVAL_IMMEDIATE -- the existing audio-driven frame
 *  pacing already controls timing the same way it did through the GDI
 *  path, so this doesn't add its own vsync wait on top of that.
 *
 *  D3DSWAPEFFECT_COPY, not the more common DISCARD: this window's client
 *  area is bigger than the back buffer (the status bar lives below the
 *  content area, which is all the back buffer covers), and DISCARD
 *  requires Present()'s source/dest rects to be NULL, which defaults to
 *  stretching the back buffer across the *entire* client area -- meaning
 *  every single Present() was painting over the status bar and forcing
 *  it to redraw itself on top, visible as constant flashing. COPY is the
 *  swap effect that actually supports a non-NULL dest rect, letting
 *  Present() stay confined to just the content area it's meant to cover.
 *
 *  The texture is D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC, locked with
 *  D3DLOCK_DISCARD every frame -- the standard, correct pattern for a
 *  resource that's fully rewritten every frame. The first version of
 *  this file used D3DPOOL_MANAGED at a single fixed maximum size instead,
 *  reasoning that avoiding a manual "recreate on device reset" path was
 *  worth the cost. That reasoning was wrong: MANAGED keeps its own
 *  system-memory copy and syncs it to video memory on the driver's own
 *  schedule, and plenty of drivers don't handle that efficiently for a
 *  large resource rewritten every single frame -- exactly the "slow even
 *  with everything off" symptom this turned out to cause. DYNAMIC +
 *  DISCARD avoids that sync entirely (the driver just hands back a fresh
 *  write-only buffer each lock), at the cost of having to recreate the
 *  texture manually after a real device loss -- handled here by sizing
 *  it to the actual current content on every upload rather than a fixed
 *  worst-case maximum, so recreation is already the normal path on a
 *  filter/scale change and costs nothing extra to also use after reset.
 ****************************************************************************/

#include <string.h>
#include <d3d9.h>
#include "d3d9_video.h"

typedef struct
{
  float x, y, z, rhw;
  DWORD color;
  float u, v;
} d3d9_vertex_t;

#define D3D9_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static IDirect3D9        *d3d;
static IDirect3DDevice9  *device;
static IDirect3DTexture9 *tex;
static int                tex_w, tex_h;   /* currently allocated texture size, 0 if none */
static IDirect3DTexture9 *overlay_tex;
static int                overlay_tex_w, overlay_tex_h;
static HWND               d3d_hwnd;
static int                device_ok;
static int                vsync_on;
static D3DPRESENT_PARAMETERS pp;

static void release_device(void)
{
  if (tex)         { IDirect3DTexture9_Release(tex);         tex = NULL;         tex_w = 0;         tex_h = 0; }
  if (overlay_tex) { IDirect3DTexture9_Release(overlay_tex); overlay_tex = NULL; overlay_tex_w = 0; overlay_tex_h = 0; }
  if (device)      { IDirect3DDevice9_Release(device);       device = NULL; }
}


static int create_device(int w, int h)
{
  ZeroMemory(&pp, sizeof(pp));
  pp.Windowed               = TRUE;
  pp.SwapEffect              = D3DSWAPEFFECT_COPY;
  pp.BackBufferFormat        = D3DFMT_UNKNOWN;   /* match the desktop format */
  pp.BackBufferWidth         = w;
  pp.BackBufferHeight        = h;
  pp.PresentationInterval    = vsync_on ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
  pp.hDeviceWindow           = d3d_hwnd;

  if (pp.BackBufferWidth  < 1) pp.BackBufferWidth  = 1;
  if (pp.BackBufferHeight < 1) pp.BackBufferHeight = 1;

  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d3d_hwnd,
             D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device)))
  {
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d3d_hwnd,
               D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device)))
    {
      device = NULL;
      return 0;
    }
  }

  IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetFVF(device, D3D9_FVF);

  return 1;
}

int d3d9_init(HWND hwnd)
{
  RECT rc;

  d3d_hwnd  = hwnd;
  device_ok = 0;

  d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) return 0;

  GetClientRect(hwnd, &rc);
  if (!create_device(rc.right - rc.left, rc.bottom - rc.top))
  {
    IDirect3D9_Release(d3d);
    d3d = NULL;
    return 0;
  }

  device_ok = 1;
  return 1;
}

void d3d9_shutdown(void)
{
  release_device();
  if (d3d) { IDirect3D9_Release(d3d); d3d = NULL; }
  device_ok = 0;
}

int d3d9_available(void)
{
  return device_ok;
}

void d3d9_notify_resize(void)
{
  /* Handled lazily in d3d9_render_frame() -- it already checks the
     client size against pp every frame and resets if it no longer
     matches, which covers both an explicit resize notification and any
     size change this didn't get told about directly. */
}

void d3d9_set_vsync(int on)
{
  vsync_on = on;

  /* PresentationInterval is part of D3DPRESENT_PARAMETERS, fixed at
     device creation -- there's no way to change it on an existing device
     short of recreating it, same as a genuine window resize does. */
  if (device_ok)
  {
    int w = pp.BackBufferWidth, h = pp.BackBufferHeight;
    release_device();
    device_ok = create_device(w, h);
  }
}

/* (Re)creates the texture only when the size actually needs to change --
   the common case (same content size frame after frame) does nothing
   here at all. */
static int ensure_texture(int w, int h)
{
  if (tex && tex_w == w && tex_h == h) return 1;

  if (tex) { IDirect3DTexture9_Release(tex); tex = NULL; }

  if (FAILED(IDirect3DDevice9_CreateTexture(device, w, h, 1, D3DUSAGE_DYNAMIC,
             D3DFMT_R5G6B5, D3DPOOL_DEFAULT, &tex, NULL)))
  {
    tex_w = 0; tex_h = 0;
    return 0;
  }

  tex_w = w;
  tex_h = h;
  return 1;
}

/* Copies src_w x src_h pixels from src_pixels (RGB565, src_pitch bytes
   per row) into a texture sized to exactly match. */
static int upload_texture(const uint16 *src_pixels, int src_pitch, int src_w, int src_h)
{
  D3DLOCKED_RECT locked;
  int y;

  if (!ensure_texture(src_w, src_h)) return 0;

  /* D3DLOCK_DISCARD is what makes this cheap: it tells the driver not to
     preserve or sync the texture's previous contents, so it can just
     hand back a fresh write-only buffer (often a different one than last
     frame's) instead of stalling to synchronize with whatever the GPU
     might still be reading from the old one. */
  if (FAILED(IDirect3DTexture9_LockRect(tex, 0, &locked, NULL, D3DLOCK_DISCARD))) return 0;

  for (y = 0; y < src_h; y++)
  {
    const uint8 *srow = (const uint8 *)src_pixels + (size_t)y * src_pitch;
    uint8 *drow = (uint8 *)locked.pBits + (size_t)y * locked.Pitch;
    memcpy(drow, srow, (size_t)src_w * 2);
  }

  IDirect3DTexture9_UnlockRect(tex, 0);
  return 1;
}

static void draw_quad(const RECT *dest)
{
  /* The well-documented D3D9 "directly mapping texels to pixels" -0.5
     offset -- without it, a screen-space (XYZRHW) quad samples half a
     pixel off from where you'd expect, blurring hard edges slightly. */
  float x0 = (float)dest->left  - 0.5f;
  float y0 = (float)dest->top   - 0.5f;
  float x1 = (float)dest->right - 0.5f;
  float y1 = (float)dest->bottom - 0.5f;
  d3d9_vertex_t verts[4];

  verts[0].x = x0; verts[0].y = y0; verts[0].z = 0; verts[0].rhw = 1;
  verts[0].color = 0xFFFFFFFF; verts[0].u = 0; verts[0].v = 0;

  verts[1].x = x1; verts[1].y = y0; verts[1].z = 0; verts[1].rhw = 1;
  verts[1].color = 0xFFFFFFFF; verts[1].u = 1; verts[1].v = 0;

  verts[2].x = x0; verts[2].y = y1; verts[2].z = 0; verts[2].rhw = 1;
  verts[2].color = 0xFFFFFFFF; verts[2].u = 0; verts[2].v = 1;

  verts[3].x = x1; verts[3].y = y1; verts[3].z = 0; verts[3].rhw = 1;
  verts[3].color = 0xFFFFFFFF; verts[3].u = 1; verts[3].v = 1;

  IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, verts, sizeof(d3d9_vertex_t));
}

int d3d9_render_frame(const uint16 *src_pixels, int src_pitch,
                       int src_w, int src_h, const RECT *dest,
                       int client_w, int client_h, int smooth)
{
  HRESULT hr;

  if (!device_ok) return 0;

  hr = IDirect3DDevice9_TestCooperativeLevel(device);
  if (hr == D3DERR_DEVICELOST) return 0;   /* wait for it to come back on its own */
  if (hr == D3DERR_DEVICENOTRESET)
  {
    release_device();
    if (!create_device(client_w, client_h)) { device_ok = 0; return 0; }
  }
  else if (client_w != pp.BackBufferWidth || client_h != pp.BackBufferHeight)
  {
    /* Size changed (window resize, fullscreen toggle) without the device
       being lost -- Reset() to match, same recovery path either way. */
    release_device();
    if (!create_device(client_w, client_h)) { device_ok = 0; return 0; }
  }

  if (client_w < 1 || client_h < 1) return 0;
  /* Sanity bound, not a real hardware limit -- catches garbage input
     without hardcoding a max the render filter's own multiplier already
     governs (largest possible is 720 * 4). */
  if (src_w < 1 || src_h < 1 || src_w > 8192 || src_h > 8192) return 0;

  if (FAILED(IDirect3DDevice9_BeginScene(device))) return 0;

  IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 0, 0);

  if (upload_texture(src_pixels, src_pitch, src_w, src_h))
  {
    D3DTEXTUREFILTERTYPE filter = smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT;

    IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, filter);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, filter);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    draw_quad(dest);
  }

  IDirect3DDevice9_EndScene(device);
  return 1;
}

static int ensure_overlay_texture(int w, int h)
{
  if (overlay_tex && overlay_tex_w == w && overlay_tex_h == h) return 1;

  if (overlay_tex) { IDirect3DTexture9_Release(overlay_tex); overlay_tex = NULL; }

  if (FAILED(IDirect3DDevice9_CreateTexture(device, w, h, 1, D3DUSAGE_DYNAMIC,
             D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &overlay_tex, NULL)))
  {
    overlay_tex_w = 0; overlay_tex_h = 0;
    return 0;
  }

  overlay_tex_w = w;
  overlay_tex_h = h;
  return 1;
}

int d3d9_draw_overlay(const uint32 *argb_pixels, int argb_pitch,
                       int w, int h, const RECT *screen_area)
{
  D3DLOCKED_RECT locked;
  int y;

  if (!device_ok) return 0;
  if (w < 1 || h < 1) return 0;
  if (!ensure_overlay_texture(w, h)) return 0;

  if (FAILED(IDirect3DTexture9_LockRect(overlay_tex, 0, &locked, NULL, D3DLOCK_DISCARD))) return 0;

  for (y = 0; y < h; y++)
  {
    const uint8 *srow = (const uint8 *)argb_pixels + (size_t)y * argb_pitch;
    uint8 *drow = (uint8 *)locked.pBits + (size_t)y * locked.Pitch;
    memcpy(drow, srow, (size_t)w * 4);
  }

  IDirect3DTexture9_UnlockRect(overlay_tex, 0);

  IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
  IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

  IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)overlay_tex);
  IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

  draw_quad(screen_area);

  /* Reset for the next frame's main game-content quad, which must never
     be alpha-blended -- that quad has no alpha channel of its own and
     this state doesn't reset itself between frames. */
  IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);

  return 1;
}

void d3d9_present(void)
{
  RECT dst;

  if (!device_ok) return;

  /* Present(NULL, NULL, ...) defaults to stretching the back buffer to
     fill the *entire* window client area -- but the back buffer is
     deliberately sized to the content area only (it excludes the status
     bar height, matching what present() in video.c actually asked for).
     Left as NULL, every frame was painting over the status bar's screen
     region and forcing it to redraw itself on top, which is what
     "flashing, DirectX only, during normal gameplay" was: this call
     running 60 times a second, each one briefly overwriting a sibling
     window that had nothing to do with this frame at all. An explicit
     rect matching the back buffer's own size keeps Present() confined to
     where the content actually belongs. */
  dst.left = 0; dst.top = 0;
  dst.right = pp.BackBufferWidth; dst.bottom = pp.BackBufferHeight;

  IDirect3DDevice9_Present(device, NULL, &dst, NULL, NULL);
}
