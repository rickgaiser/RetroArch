/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2018 - Francisco Javier Trujillo Mata - fjtrujy
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <kernel.h>
#include <gsKit.h>
#include <gsInline.h>

#include "../../driver.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

#include "../../libretro-common/include/libretro_gskit_ps2.h"

#define GS_TEXT GS_SETREG_RGBA(0x80,0x80,0x80,0x80) // turn white GS Screen
#define GS_BLACK GS_SETREG_RGBA(0x00,0x00,0x00,0x80) // turn white GS Screen

//#define HIRES_MODE
#define CRT_FILTER

typedef struct ps2_video
{
   GSGLOBAL *gsGlobal;
   GSTEXTURE *menuTexture;
   GSTEXTURE *coreTexture;
#ifdef CRT_FILTER
   GSTEXTURE *tex_effect;
#endif
   struct retro_hw_render_interface_gskit_ps2 iface; /* Palette in the cores */

   bool menuVisible;
   bool fullscreen;

   bool vsync;

   int PSM;
   bool force_aspect;
   int menu_filter;
   int core_filter;

   bool shader;
} ps2_video_t;

static unsigned int iScaleW;
static unsigned int iScaleH;

// PRIVATE METHODS
#ifdef CRT_FILTER
static void create_tex_effect(GSTEXTURE *eff, u32 height, u32 scaleh) {
	u32 x,y;
	u32 *pPixel;

	eff->Width = 64; // Minimum width?
	eff->Height = height * scaleh;
	eff->PSM = GS_PSM_CT32;
	eff->Mem = malloc(gsKit_texture_size_ee(eff->Width, eff->Height, eff->PSM));
	eff->Vram = 0;
	eff->Filter = GS_FILTER_NEAREST;
	gsKit_setup_tbw(eff);
	pPixel = eff->Mem;

	for(y=0; y<eff->Height; y++) {
		u32 lineoff = y % scaleh;
		u32 linecolor = (lineoff==(scaleh-1)) /* last line ? */ ?
			GS_SETREG_RGBA(0x00,0x00,0x00,0x40) : // Last line, 0x80=black
			GS_SETREG_RGBA(0x00,0x00,0x00,0x00);  // Other line(s), 0x00=transparent
		for(x=0; x<eff->Width; x++) {
			*pPixel++ = linecolor;
		}
	}
}
#endif

static GSGLOBAL *init_GSGlobal(void)
{
   GSGLOBAL *gsGlobal = gsKit_init_global();
	int iPassCount;

#ifdef HIRES
   #if 1
   gsGlobal->Mode = GS_MODE_DTV_720P;
   gsGlobal->Interlace = GS_NONINTERLACED;
   gsGlobal->Field = GS_FRAME;
   gsGlobal->Width = 1280;
   gsGlobal->Height = 720;
   iPassCount = 3;
   iScaleW = 3;
   iScaleH = 3;
   #endif
   #if 0
   gsGlobal->Mode = GS_MODE_DTV_1080I;
   gsGlobal->Interlace = GS_INTERLACED;
   gsGlobal->Field = GS_FRAME;
   gsGlobal->Width = 1920;
   gsGlobal->Height = 540;
   iPassCount = 3;
   iScaleW = 4;
   iScaleH = 2;
   #endif
#else
   #if 0
   // Is this 240p60 ???
   gsGlobal->Mode = GS_MODE_NTSC;
   gsGlobal->Interlace = GS_NONINTERLACED;
   gsGlobal->Field = GS_FRAME;
   gsGlobal->Width = 320;
   gsGlobal->Height = 240;
   iPassCount = 0;
   iScaleW = 1;
   iScaleH = 1;
   #endif
   #if 1
   gsGlobal->Mode = GS_MODE_DTV_480P;
   gsGlobal->Interlace = GS_NONINTERLACED;
   gsGlobal->Field = GS_FRAME;
   gsGlobal->Width = 640;
   gsGlobal->Height = 480;
   iPassCount = 2;
   iScaleW = 2;
   iScaleH = 2;
   #endif
#endif

   gsGlobal->PSM = GS_PSM_CT16;
   gsGlobal->PSMZ = GS_PSMZ_16S;
   gsGlobal->DoubleBuffering = GS_SETTING_ON;
   gsGlobal->ZBuffering = GS_SETTING_OFF;
   gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

   dmaKit_init(D_CTRL_RELE_OFF,D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
               D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);

   /* Initialize the DMAC */
   dmaKit_chan_init(DMA_CHANNEL_GIF);

#ifdef HIRES_MODE
	gsKit_hires_init_screen(gsGlobal, iPassCount);
#else
   gsKit_init_screen(gsGlobal);
   gsKit_mode_switch(gsGlobal, GS_ONESHOT);
   gsKit_clear(gsGlobal, GS_BLACK);
#endif
   gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
   gsKit_set_test(gsGlobal, GS_ATEST_OFF);
   //gsKit_TexManager_setmode(gsGlobal, ETM_DIRECT);

   return gsGlobal;
}

static void init_ps2_video(ps2_video_t *ps2)
{
   ps2->gsGlobal = init_GSGlobal();
   gsKit_TexManager_init(ps2->gsGlobal);

   ps2->menuTexture = calloc(1, sizeof(GSTEXTURE));
   ps2->coreTexture = calloc(1, sizeof(GSTEXTURE));
#ifdef CRT_FILTER
   ps2->tex_effect = calloc(1, sizeof(GSTEXTURE));
#endif

   /* Used for cores that supports palette */
   ps2->iface.interface_type = RETRO_HW_RENDER_INTERFACE_GSKIT_PS2;
   ps2->iface.interface_version = RETRO_HW_RENDER_INTERFACE_GSKIT_PS2_VERSION;
   ps2->iface.coreTexture = ps2->coreTexture;
}

static void prim_texture(GSGLOBAL *gsGlobal, GSTEXTURE *texture)
{
    // Pixel perfect texture mapping:
    // - Move to the upper-left corner of the pixel
    float fOffsetX = -0.5f;
    float fOffsetY = -0.5f;
    float fOffsetU = 0.0f;
    float fOffsetV = 0.0f;
    unsigned int iDispWidth  = texture->Width  * iScaleW;
    unsigned int iDispHeight = texture->Height * iScaleH;

    // Center on screen
    float fPosX = (gsGlobal->Width  - iDispWidth)  / 2;
    float fPosY = (gsGlobal->Height - iDispHeight) / 2;

    gsKit_TexManager_bind(gsGlobal, texture);
    gsKit_prim_sprite_texture(gsGlobal, texture,
        fOffsetX+fPosX,             // X1
        fOffsetY+fPosY,             // Y2
        fOffsetU+0.0f,              // U1
        fOffsetV+0.0f,              // V1
        fOffsetX+fPosX+iDispWidth,  // X2
        fOffsetY+fPosY+iDispHeight, // Y2
        fOffsetU+texture->Width,    // U2
        fOffsetV+texture->Height,   // V2
        2,
        GS_TEXT);
}

static void refreshScreen(ps2_video_t *ps2)
{
#ifdef HIRES_MODE
   if (ps2->vsync)
      gsKit_hires_sync(ps2->gsGlobal);
	gsKit_hires_flip(ps2->gsGlobal);
#else
   if (ps2->vsync) {
      gsKit_sync_flip(ps2->gsGlobal);
   }
   gsKit_queue_exec(ps2->gsGlobal);
#endif
   gsKit_TexManager_nextFrame(ps2->gsGlobal);
}

static void *ps2_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   void *ps2input = NULL;
   *input_data = NULL;
   (void)video;

   ps2_video_t *ps2 = (ps2_video_t*)calloc(1, sizeof(ps2_video_t));

   if (!ps2)
      return NULL;

   init_ps2_video(ps2);
   if (video->font_enable) {
      font_driver_init_osd(ps2, false, video->is_threaded, FONT_DRIVER_RENDER_PS2);
   }
   ps2->PSM = (video->rgb32 ? GS_PSM_CT32 : GS_PSM_CT16);
   ps2->fullscreen = video->fullscreen;
   ps2->core_filter = video->smooth ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
   ps2->force_aspect = video->force_aspect;
   ps2->vsync = video->vsync;

   if (input && input_data) {
      settings_t *settings = config_get_ptr();
      ps2input = input_ps2.init(settings->arrays.input_joypad_driver);
      *input = ps2input ? &input_ps2 : NULL;
      *input_data  = ps2input;
   }

   return ps2;
}

static bool ps2_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   ps2_video_t *ps2 = (ps2_video_t*)data;

   if (!width || !height)
      return false;

#if defined(DEBUG)
   if (frame_count%60==0) {
      printf("ps2_gfx_frame %lu\n", frame_count);
   }
#endif

   gsKit_set_primalpha(ps2->gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
   gsKit_set_test(ps2->gsGlobal, GS_ATEST_OFF);

   if (frame) {
      struct retro_hw_ps2_insets padding = empty_ps2_insets;
      if (frame != RETRO_HW_FRAME_BUFFER_VALID){ /* Checking if the transfer is done in the core */
          int PSM = GS_PSM_CT16;
          u32 size = gsKit_texture_size_ee(width, height, PSM);
          if ((width  != ps2->coreTexture->Width) ||
              (height != ps2->coreTexture->Height) ||
              (PSM    != ps2->coreTexture->PSM)) {
             if (ps2->coreTexture->Mem == NULL)
                free(ps2->coreTexture->Mem);
             ps2->coreTexture->Mem = malloc(size);
             ps2->coreTexture->Width = width;
             ps2->coreTexture->Height = height;
             ps2->coreTexture->PSM = PSM;
             ps2->coreTexture->Filter = GS_FILTER_NEAREST;
          }
         memcpy(ps2->coreTexture->Mem, frame, size);
      } else {
         ps2->iface.updatedPalette = false;
         if (ps2->iface.clearTexture) {
            ps2->iface.clearTexture = false;
         }
      }

      gsKit_TexManager_invalidate(ps2->gsGlobal, ps2->coreTexture);
      prim_texture(ps2->gsGlobal, ps2->coreTexture);
   }

   if (ps2->menuVisible) {
      bool texture_empty = !ps2->menuTexture->Width || !ps2->menuTexture->Height;
      if (!texture_empty) {
         prim_texture(ps2->gsGlobal, ps2->menuTexture);
      }
   } else if (video_info->statistics_show) {
      struct font_params *osd_params = (struct font_params*)
         &video_info->osd_stat_params;

      if (osd_params) {
         font_driver_render_msg(video_info, NULL, video_info->stat_text,
               (const struct font_params*)&video_info->osd_stat_params);
      }
   }

   if(!string_is_empty(msg)) {
      font_driver_render_msg(video_info, NULL, msg, NULL);
   }

#ifdef CRT_FILTER
//   if (ps2->shader) {
       if (ps2->tex_effect->Mem == NULL)
          create_tex_effect(ps2->tex_effect, 240, iScaleH);
       prim_texture(ps2->gsGlobal, ps2->tex_effect);
//   }
#endif

   refreshScreen(ps2);

   return true;
}

static void ps2_gfx_set_nonblock_state(void *data, bool toggle)
{
   ps2_video_t *ps2 = (ps2_video_t*)data;

   if (ps2)
      ps2->vsync = !toggle;
}

static bool ps2_gfx_alive(void *data)
{
   (void)data;
   return true;
}

static bool ps2_gfx_focus(void *data)
{
   (void)data;
   return true;
}

static bool ps2_gfx_suppress_screensaver(void *data, bool enable)
{
   (void)data;
   (void)enable;
   return false;
}

static bool ps2_gfx_has_windowed(void *data)
{
   (void)data;
   return true;
}

static void deinitTexture(ps2_video_t *ps2, GSTEXTURE *texture)
{
   gsKit_TexManager_free(ps2->gsGlobal, texture);

   if (texture->Mem != NULL) {
      free(texture->Mem);
      texture->Mem = NULL;
   }
   if (texture->Clut != NULL) {
      free(texture->Clut);
      texture->Clut = NULL;
   }
}

static void ps2_gfx_free(void *data)
{
   ps2_video_t *ps2 = (ps2_video_t*)data;

   font_driver_free_osd();

   deinitTexture(ps2, ps2->menuTexture);
   free(ps2->menuTexture);
   deinitTexture(ps2, ps2->coreTexture);
   free(ps2->coreTexture);
#ifdef CRT_FILTER
   deinitTexture(ps2, ps2->tex_effect);
   free(ps2->tex_effect);
#endif

#ifdef HIRES_MODE
   gsKit_hires_deinit_global(ps2->gsGlobal);
#else
   gsKit_deinit_global(ps2->gsGlobal);
#endif

   free(data);
}

static bool ps2_gfx_set_shader(void *data,
      enum rarch_shader_type type, const char *path)
{
    (void)type;
    ps2_video_t *ps2 = (ps2_video_t*)data;

   ps2->shader = path != NULL;

   return true;
}

static void ps2_set_filtering(void *data, unsigned index, bool smooth)
{
   ps2_video_t *ps2 = (ps2_video_t*)data;

   ps2->menu_filter = smooth ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
}

static void ps2_set_texture_frame(void *data, const void *frame, bool rgb32,
                               unsigned width, unsigned height, float alpha)
{
   ps2_video_t *ps2 = (ps2_video_t*)data;

   int PSM = (rgb32 ? GS_PSM_CT32 : GS_PSM_CT16);
   u32 size = gsKit_texture_size_ee(width, height, PSM);
   if ((width  != ps2->menuTexture->Width) ||
       (height != ps2->menuTexture->Height) ||
       (PSM    != ps2->menuTexture->PSM)) {
      if (ps2->menuTexture->Mem == NULL)
          free(ps2->menuTexture->Mem);
      ps2->menuTexture->Mem = malloc(size);
      ps2->menuTexture->Width = width;
      ps2->menuTexture->Height = height;
      ps2->menuTexture->PSM = PSM;
      ps2->menuTexture->Filter = GS_FILTER_NEAREST;
   }

   memcpy(ps2->menuTexture->Mem, frame, size);
   gsKit_TexManager_invalidate(ps2->gsGlobal, ps2->menuTexture);
}

static void ps2_set_texture_enable(void *data, bool enable, bool fullscreen)
{
   ps2_video_t *ps2 = (ps2_video_t*)data;
   if (ps2->menuVisible != enable) {
      ps2->iface.clearTexture = true;
      ps2->iface.updatedPalette = true;
   }
   ps2->menuVisible = enable;
   ps2->fullscreen = fullscreen;
}

static void ps2_set_osd_msg(void *data,
      video_frame_info_t *video_info,
      const char *msg,
      const void *params, void *font)
{
   font_driver_render_msg(video_info, font, msg, params);
}

static bool ps2_get_hw_render_interface(void* data,
      const struct retro_hw_render_interface** iface)
{
   ps2_video_t* ps2 = (ps2_video_t*)data;
   ps2->iface.clearTexture = false;
   ps2->iface.updatedPalette = true;
   ps2->iface.padding = empty_ps2_insets;
   *iface = (const struct retro_hw_render_interface*)&ps2->iface;
   return true;
}

static const video_poke_interface_t ps2_poke_interface = {
   NULL,          /* get_flags  */
   NULL,
   NULL,
   NULL,
   NULL, /* get_refresh_rate */
   ps2_set_filtering,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   NULL, /* set_aspect_ratio */
   NULL, /* apply_state_changes */
   ps2_set_texture_frame,
   ps2_set_texture_enable,
   ps2_set_osd_msg,             /* set_osd_msg */
   NULL,                        /* show_mouse  */
   NULL,                        /* grab_mouse_toggle */
   NULL,                        /* get_current_shader */
   NULL,                        /* get_current_software_framebuffer */
   ps2_get_hw_render_interface  /* get_hw_render_interface */
};

static void ps2_gfx_get_poke_interface(void *data,
      const video_poke_interface_t **iface)
{
   (void)data;
   *iface = &ps2_poke_interface;
}

video_driver_t video_ps2 = {
   ps2_gfx_init,
   ps2_gfx_frame,
   ps2_gfx_set_nonblock_state,
   ps2_gfx_alive,
   ps2_gfx_focus,
   ps2_gfx_suppress_screensaver,
   ps2_gfx_has_windowed,
   ps2_gfx_set_shader,
   ps2_gfx_free,
   "ps2",
   NULL, /* set_viewport */
   NULL, /* set_rotation */
   NULL, /* viewport_info */
   NULL, /* read_viewport  */
   NULL, /* read_frame_raw */

#ifdef HAVE_OVERLAY
  NULL, /* overlay_interface */
#endif
#ifdef HAVE_VIDEO_LAYOUT
  NULL,
#endif
  ps2_gfx_get_poke_interface,
};
