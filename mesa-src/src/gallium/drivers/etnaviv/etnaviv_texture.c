/*
 * Copyright (c) 2012-2015 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */

#include "etnaviv_texture.h"

#include "hw/common.xml.h"

#include "etnaviv_clear_blit.h"
#include "etnaviv_context.h"
#include "etnaviv_emit.h"
#include "etnaviv_format.h"
#include "etnaviv_translate.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include <drm_fourcc.h>

static void *
etna_create_sampler_state(struct pipe_context *pipe,
                          const struct pipe_sampler_state *ss)
{
   struct etna_sampler_state *cs = CALLOC_STRUCT(etna_sampler_state);

   if (!cs)
      return NULL;

   cs->TE_SAMPLER_CONFIG0 =
      VIVS_TE_SAMPLER_CONFIG0_UWRAP(translate_texture_wrapmode(ss->wrap_s)) |
      VIVS_TE_SAMPLER_CONFIG0_VWRAP(translate_texture_wrapmode(ss->wrap_t)) |
      VIVS_TE_SAMPLER_CONFIG0_MIN(translate_texture_filter(ss->min_img_filter)) |
      VIVS_TE_SAMPLER_CONFIG0_MIP(translate_texture_mipfilter(ss->min_mip_filter)) |
      VIVS_TE_SAMPLER_CONFIG0_MAG(translate_texture_filter(ss->mag_img_filter)) |
      COND(ss->normalized_coords, VIVS_TE_SAMPLER_CONFIG0_ROUND_UV);
   cs->TE_SAMPLER_CONFIG1 = 0; /* VIVS_TE_SAMPLER_CONFIG1 (swizzle, extended
                                  format) fully determined by sampler view */
   cs->TE_SAMPLER_LOD_CONFIG =
      COND(ss->lod_bias != 0.0, VIVS_TE_SAMPLER_LOD_CONFIG_BIAS_ENABLE) |
      VIVS_TE_SAMPLER_LOD_CONFIG_BIAS(etna_float_to_fixp55(ss->lod_bias));

   if (ss->min_mip_filter != PIPE_TEX_MIPFILTER_NONE) {
      cs->min_lod = etna_float_to_fixp55(ss->min_lod);
      cs->max_lod = etna_float_to_fixp55(ss->max_lod);
   } else {
      /* when not mipmapping, we need to set max/min lod so that always
       * lowest LOD is selected */
      cs->min_lod = cs->max_lod = etna_float_to_fixp55(ss->min_lod);
   }

   return cs;
}

static void
etna_bind_sampler_states(struct pipe_context *pctx, enum pipe_shader_type shader,
                         unsigned start_slot, unsigned num_samplers,
                         void **samplers)
{
   /* bind fragment sampler */
   struct etna_context *ctx = etna_context(pctx);
   int offset;

   switch (shader) {
   case PIPE_SHADER_FRAGMENT:
      offset = 0;
      ctx->num_fragment_samplers = num_samplers;
      break;
   case PIPE_SHADER_VERTEX:
      offset = ctx->specs.vertex_sampler_offset;
      break;
   default:
      assert(!"Invalid shader");
      return;
   }

   uint32_t mask = 1 << offset;
   for (int idx = 0; idx < num_samplers; ++idx, mask <<= 1) {
      ctx->sampler[offset + idx] = samplers[idx];
      if (samplers[idx])
         ctx->active_samplers |= mask;
      else
         ctx->active_samplers &= ~mask;
   }

   ctx->dirty |= ETNA_DIRTY_SAMPLERS;
}

static void
etna_delete_sampler_state(struct pipe_context *pctx, void *ss)
{
   FREE(ss);
}

/* Return true if the GPU can use sampler TS with this sampler view.
 * Sampler TS is an optimization used when rendering to textures, where
 * a resolve-in-place can be avoided when rendering has left a (valid) TS.
 */
static bool
etna_can_use_sampler_ts(struct pipe_sampler_view *view, int num)
{
    /* Can use sampler TS when:
     * - the hardware supports sampler TS.
     * - the sampler view will be bound to sampler <VIVS_TS_SAMPLER__LEN.
     *   HALTI5 adds a mapping from sampler to sampler TS unit, but this is AFAIK
     *   absent on earlier models.
     * - it is a texture, not a buffer.
     * - the sampler view has a supported format for sampler TS.
     * - the sampler will have one LOD, and it happens to be level 0.
     *   (it is not sure if the hw supports it for other levels, but available
     *   state strongly suggests only one at a time).
     * - the resource TS is valid for level 0.
     */
   struct etna_resource *rsc = etna_resource(view->texture);
   struct etna_screen *screen = etna_screen(rsc->base.screen);
   return VIV_FEATURE(screen, chipMinorFeatures2, TEXTURE_TILED_READ) &&
      num < VIVS_TS_SAMPLER__LEN &&
      rsc->base.target != PIPE_BUFFER &&
      translate_ts_sampler_format(rsc->base.format) != ETNA_NO_MATCH &&
      view->u.tex.first_level == 0 && MIN2(view->u.tex.last_level, rsc->base.last_level) == 0 &&
      rsc->levels[0].ts_valid;
}

static void
etna_configure_sampler_ts(struct pipe_sampler_view *pview, bool enable)
{
   struct etna_sampler_view *view = etna_sampler_view(pview);
   if (enable) {
      struct etna_resource *rsc = etna_resource(view->base.texture);
      struct etna_resource_level *lev = &rsc->levels[0];
      assert(rsc->ts_bo && lev->ts_valid);

      view->TE_SAMPLER_CONFIG1 |= VIVS_TE_SAMPLER_CONFIG1_USE_TS;
      view->TS_SAMPLER_CONFIG =
         VIVS_TS_SAMPLER_CONFIG_ENABLE(1) |
         VIVS_TS_SAMPLER_CONFIG_FORMAT(translate_ts_sampler_format(rsc->base.format));
      view->TS_SAMPLER_CLEAR_VALUE = lev->clear_value;
      view->TS_SAMPLER_CLEAR_VALUE2 = lev->clear_value; /* To handle 64-bit formats this needs a different value */
      view->TS_SAMPLER_STATUS_BASE.bo = rsc->ts_bo;
      view->TS_SAMPLER_STATUS_BASE.offset = lev->ts_offset;
      view->TS_SAMPLER_STATUS_BASE.flags = ETNA_RELOC_READ;
   } else {
      view->TE_SAMPLER_CONFIG1 &= ~VIVS_TE_SAMPLER_CONFIG1_USE_TS;
      view->TS_SAMPLER_CONFIG = 0;
      view->TS_SAMPLER_STATUS_BASE.bo = NULL;
   }
   /* n.b.: relies on caller to mark ETNA_DIRTY_SAMPLER_VIEWS */
}

static void
etna_update_sampler_source(struct pipe_sampler_view *view, int num)
{
   struct etna_resource *base = etna_resource(view->texture);
   struct etna_resource *to = base, *from = base;
   bool enable_sampler_ts = false;

   if (base->external && etna_resource_newer(etna_resource(base->external), base))
      from = etna_resource(base->external);

   if (base->texture)
      to = etna_resource(base->texture);

   if ((to != from) && etna_resource_older(to, from)) {
      etna_copy_resource(view->context, &to->base, &from->base, 0,
                         view->texture->last_level);
      to->seqno = from->seqno;
   } else if ((to == from) && etna_resource_needs_flush(to)) {
      if (etna_can_use_sampler_ts(view, num)) {
         enable_sampler_ts = true;
         /* Do not set flush_seqno because the resolve-to-self was bypassed */
      } else {
         /* Resolve TS if needed */
         etna_copy_resource(view->context, &to->base, &from->base, 0,
                            view->texture->last_level);
         to->flush_seqno = from->seqno;
      }
   }
   etna_configure_sampler_ts(view, enable_sampler_ts);
}

static bool
etna_resource_sampler_compatible(struct etna_resource *res)
{
   if (util_format_is_compressed(res->base.format))
      return true;

   struct etna_screen *screen = etna_screen(res->base.screen);
   /* This GPU supports texturing from supertiled textures? */
   if (res->layout == ETNA_LAYOUT_SUPER_TILED && VIV_FEATURE(screen, chipMinorFeatures2, SUPERTILED_TEXTURE))
      return true;

   /* TODO: LINEAR_TEXTURE_SUPPORT */

   /* Otherwise, only support tiled layouts */
   if (res->layout != ETNA_LAYOUT_TILED)
      return false;

   /* If we have HALIGN support, we can allow for the RS padding */
   if (VIV_FEATURE(screen, chipMinorFeatures1, TEXTURE_HALIGN))
      return true;

   /* Non-HALIGN GPUs only accept 4x4 tile-aligned textures */
   if (res->halign != TEXTURE_HALIGN_FOUR)
      return false;

   return true;
}

static struct pipe_sampler_view *
etna_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *prsc,
                         const struct pipe_sampler_view *so)
{
   struct etna_sampler_view *sv = CALLOC_STRUCT(etna_sampler_view);
   struct etna_resource *res = etna_resource(prsc);
   struct etna_context *ctx = etna_context(pctx);
   const uint32_t format = translate_texture_format(so->format);
   const bool ext = !!(format & EXT_FORMAT);
   const bool astc = !!(format & ASTC_FORMAT);
   const uint32_t swiz = get_texture_swiz(so->format, so->swizzle_r,
                                          so->swizzle_g, so->swizzle_b,
                                          so->swizzle_a);

   if (!sv)
      return NULL;

   if (!etna_resource_sampler_compatible(res)) {
      /* The original resource is not compatible with the sampler.
       * Allocate an appropriately tiled texture. */
      if (!res->texture) {
         struct pipe_resource templat = *prsc;

         templat.bind &= ~(PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_RENDER_TARGET |
                           PIPE_BIND_BLENDABLE);
         res->texture =
            etna_resource_alloc(pctx->screen, ETNA_LAYOUT_TILED,
                                DRM_FORMAT_MOD_LINEAR, &templat);
      }

      if (!res->texture) {
         free(sv);
         return NULL;
      }
      res = etna_resource(res->texture);
   }

   sv->base = *so;
   pipe_reference_init(&sv->base.reference, 1);
   sv->base.texture = NULL;
   pipe_resource_reference(&sv->base.texture, prsc);
   sv->base.context = pctx;

   /* merged with sampler state */
   sv->TE_SAMPLER_CONFIG0 = COND(!ext && !astc, VIVS_TE_SAMPLER_CONFIG0_FORMAT(format));
   sv->TE_SAMPLER_CONFIG0_MASK = 0xffffffff;

   switch (sv->base.target) {
   case PIPE_TEXTURE_1D:
      /* For 1D textures, we will have a height of 1, so we can use 2D
       * but set T wrap to repeat */
      sv->TE_SAMPLER_CONFIG0_MASK = ~VIVS_TE_SAMPLER_CONFIG0_VWRAP__MASK;
      sv->TE_SAMPLER_CONFIG0 |= VIVS_TE_SAMPLER_CONFIG0_VWRAP(TEXTURE_WRAPMODE_REPEAT);
      /* fallthrough */
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
      sv->TE_SAMPLER_CONFIG0 |= VIVS_TE_SAMPLER_CONFIG0_TYPE(TEXTURE_TYPE_2D);
      break;
   case PIPE_TEXTURE_CUBE:
      sv->TE_SAMPLER_CONFIG0 |= VIVS_TE_SAMPLER_CONFIG0_TYPE(TEXTURE_TYPE_CUBE_MAP);
      break;
   default:
      BUG("Unhandled texture target");
      free(sv);
      return NULL;
   }

   sv->TE_SAMPLER_CONFIG1 = COND(ext, VIVS_TE_SAMPLER_CONFIG1_FORMAT_EXT(format)) |
                            COND(astc, VIVS_TE_SAMPLER_CONFIG1_FORMAT_EXT(TEXTURE_FORMAT_EXT_ASTC)) |
                            VIVS_TE_SAMPLER_CONFIG1_HALIGN(res->halign) | swiz;
   sv->TE_SAMPLER_ASTC0 = COND(astc, VIVS_NTE_SAMPLER_ASTC0_ASTC_FORMAT(format)) |
                          VIVS_NTE_SAMPLER_ASTC0_UNK8(0xc) |
                          VIVS_NTE_SAMPLER_ASTC0_UNK16(0xc) |
                          VIVS_NTE_SAMPLER_ASTC0_UNK24(0xc);
   sv->TE_SAMPLER_SIZE = VIVS_TE_SAMPLER_SIZE_WIDTH(res->base.width0) |
                         VIVS_TE_SAMPLER_SIZE_HEIGHT(res->base.height0);
   sv->TE_SAMPLER_LOG_SIZE =
      VIVS_TE_SAMPLER_LOG_SIZE_WIDTH(etna_log2_fixp55(res->base.width0)) |
      VIVS_TE_SAMPLER_LOG_SIZE_HEIGHT(etna_log2_fixp55(res->base.height0)) |
      COND(util_format_is_srgb(so->format) && !astc, VIVS_TE_SAMPLER_LOG_SIZE_SRGB) |
      COND(astc, VIVS_TE_SAMPLER_LOG_SIZE_ASTC);

   /* Set up levels-of-detail */
   for (int lod = 0; lod <= res->base.last_level; ++lod) {
      sv->TE_SAMPLER_LOD_ADDR[lod].bo = res->bo;
      sv->TE_SAMPLER_LOD_ADDR[lod].offset = res->levels[lod].offset;
      sv->TE_SAMPLER_LOD_ADDR[lod].flags = ETNA_RELOC_READ;
   }
   sv->min_lod = sv->base.u.tex.first_level << 5;
   sv->max_lod = MIN2(sv->base.u.tex.last_level, res->base.last_level) << 5;

   /* Workaround for npot textures -- it appears that only CLAMP_TO_EDGE is
    * supported when the appropriate capability is not set. */
   if (!ctx->specs.npot_tex_any_wrap &&
       (!util_is_power_of_two(res->base.width0) || !util_is_power_of_two(res->base.height0))) {
      sv->TE_SAMPLER_CONFIG0_MASK = ~(VIVS_TE_SAMPLER_CONFIG0_UWRAP__MASK |
                                      VIVS_TE_SAMPLER_CONFIG0_VWRAP__MASK);
      sv->TE_SAMPLER_CONFIG0 |=
         VIVS_TE_SAMPLER_CONFIG0_UWRAP(TEXTURE_WRAPMODE_CLAMP_TO_EDGE) |
         VIVS_TE_SAMPLER_CONFIG0_VWRAP(TEXTURE_WRAPMODE_CLAMP_TO_EDGE);
   }

   return &sv->base;
}

static void
etna_sampler_view_destroy(struct pipe_context *pctx,
                          struct pipe_sampler_view *view)
{
   pipe_resource_reference(&view->texture, NULL);
   FREE(view);
}

static void
set_sampler_views(struct etna_context *ctx, unsigned start, unsigned end,
                  unsigned nr, struct pipe_sampler_view **views)
{
   unsigned i, j;
   uint32_t mask = 1 << start;

   for (i = start, j = 0; j < nr; i++, j++, mask <<= 1) {
      pipe_sampler_view_reference(&ctx->sampler_view[i], views[j]);
      if (views[j])
         ctx->active_sampler_views |= mask;
      else
         ctx->active_sampler_views &= ~mask;
   }

   for (; i < end; i++, mask <<= 1) {
      pipe_sampler_view_reference(&ctx->sampler_view[i], NULL);
      ctx->active_sampler_views &= ~mask;
   }
}

static inline void
etna_fragtex_set_sampler_views(struct etna_context *ctx, unsigned nr,
                               struct pipe_sampler_view **views)
{
   unsigned start = 0;
   unsigned end = start + ctx->specs.fragment_sampler_count;

   set_sampler_views(ctx, start, end, nr, views);
   ctx->num_fragment_sampler_views = nr;
}


static inline void
etna_vertex_set_sampler_views(struct etna_context *ctx, unsigned nr,
                              struct pipe_sampler_view **views)
{
   unsigned start = ctx->specs.vertex_sampler_offset;
   unsigned end = start + ctx->specs.vertex_sampler_count;

   set_sampler_views(ctx, start, end, nr, views);
}

static void
etna_set_sampler_views(struct pipe_context *pctx, enum pipe_shader_type shader,
                       unsigned start_slot, unsigned num_views,
                       struct pipe_sampler_view **views)
{
   struct etna_context *ctx = etna_context(pctx);
   assert(start_slot == 0);

   ctx->dirty |= ETNA_DIRTY_SAMPLER_VIEWS | ETNA_DIRTY_TEXTURE_CACHES;

   for (unsigned idx = 0; idx < num_views; ++idx) {
      if (views[idx])
         etna_update_sampler_source(views[idx], idx);
   }

   switch (shader) {
   case PIPE_SHADER_FRAGMENT:
      etna_fragtex_set_sampler_views(ctx, num_views, views);
      break;
   case PIPE_SHADER_VERTEX:
      etna_vertex_set_sampler_views(ctx, num_views, views);
      break;
   default:;
   }
}

static void
etna_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
   struct etna_context *ctx = etna_context(pctx);
   /* clear color and texture cache to make sure that texture unit reads
    * what has been written */
   etna_set_state(ctx->stream, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_COLOR | VIVS_GL_FLUSH_CACHE_TEXTURE);
}

void
etna_texture_init(struct pipe_context *pctx)
{
   pctx->create_sampler_state = etna_create_sampler_state;
   pctx->bind_sampler_states = etna_bind_sampler_states;
   pctx->delete_sampler_state = etna_delete_sampler_state;
   pctx->set_sampler_views = etna_set_sampler_views;
   pctx->create_sampler_view = etna_create_sampler_view;
   pctx->sampler_view_destroy = etna_sampler_view_destroy;
   pctx->texture_barrier = etna_texture_barrier;
}
