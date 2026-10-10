/* SPDX-License-Identifier: MIT */
/* Included in d3d12_resource.cpp after Mesa's copy_texture_region helper. */
#include "nxbox_view_cast.h"
#include "d3d12_query.h"

struct nxbox_srv_shadow {
   enum pipe_format format;
   struct pipe_resource *texture;
   struct pipe_resource *buffer;
   struct nxbox_srv_shadow *next;
};

struct pipe_resource *
nxbox_srv_shadow_texture(struct nxbox_srv_shadow *shadow)
{
   return shadow->texture;
}

void
nxbox_destroy_srv_shadows(struct d3d12_resource *res)
{
   while (res->nxbox_srv_shadows) {
      struct nxbox_srv_shadow *shadow = res->nxbox_srv_shadows;
      res->nxbox_srv_shadows = shadow->next;
      pipe_resource_reference(&shadow->texture, NULL);
      pipe_resource_reference(&shadow->buffer, NULL);
      FREE(shadow);
   }
}

struct nxbox_srv_shadow *
nxbox_get_srv_shadow(struct d3d12_context *ctx, struct d3d12_resource *res, enum pipe_format format)
{
   struct pipe_resource *source = &res->base.b;
   if (!nxbox_view_cast_copy_enabled() || source->target == PIPE_BUFFER || source->nr_samples > 1 ||
       source->nr_storage_samples > 1 || util_format_is_compressed(format) ||
       util_format_is_compressed(res->overall_format) || util_format_is_depth_or_stencil(format) ||
       util_format_is_depth_or_stencil(res->overall_format) ||
       util_format_get_num_planes(format) != 1 ||
       util_format_get_num_planes(res->overall_format) != 1 ||
       util_format_get_blockwidth(format) != 1 || util_format_get_blockheight(format) != 1 ||
       util_format_get_blockwidth(res->overall_format) != 1 ||
       util_format_get_blockheight(res->overall_format) != 1 ||
       util_format_get_blocksize(format) != util_format_get_blocksize(res->overall_format))
      return NULL;

   /* Shared resources can have views created from more than one pipe context. */
   static std::mutex mutex;
   std::lock_guard<std::mutex> lock(mutex);
   for (struct nxbox_srv_shadow *shadow = res->nxbox_srv_shadows; shadow; shadow = shadow->next)
      if (shadow->format == format)
         return shadow;

   struct pipe_screen *screen = ctx->base.screen;
   if (!screen->is_format_supported(screen, format, source->target, 0, 0, PIPE_BIND_SAMPLER_VIEW))
      return NULL;
   struct nxbox_srv_shadow *shadow = CALLOC_STRUCT(nxbox_srv_shadow);
   if (!shadow)
      return NULL;
   struct pipe_resource templ = *source;
   templ.format = format;
   templ.bind = PIPE_BIND_SAMPLER_VIEW;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.flags = 0;
   templ.next = NULL;
   shadow->texture = screen->resource_create(screen, &templ);
   if (!shadow->texture) {
      FREE(shadow);
      return NULL;
   }

   /* Subresource zero is the largest mip; reuse one GPU buffer for all mips/layers. */
   D3D12_RESOURCE_DESC desc = GetDesc(d3d12_resource_resource(res));
   D3D12_RESOURCE_DESC shadow_desc =
      GetDesc(d3d12_resource_resource(d3d12_resource(shadow->texture)));
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint, shadow_footprint;
   UINT rows = 0, shadow_rows = 0;
   UINT64 size = 0, row_bytes = 0, shadow_row_bytes = 0;
   d3d12_screen(screen)->dev->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes,
                                                    &size);
   d3d12_screen(screen)->dev->GetCopyableFootprints(&shadow_desc, 0, 1, 0, &shadow_footprint,
                                                    &shadow_rows, &shadow_row_bytes, nullptr);
   /* Pipe formats with emulated storage must also have byte-identical native layouts. */
   if (size && size <= UINT32_MAX && row_bytes == shadow_row_bytes && rows == shadow_rows &&
       footprint.Footprint.RowPitch == shadow_footprint.Footprint.RowPitch &&
       footprint.Footprint.Width == shadow_footprint.Footprint.Width &&
       footprint.Footprint.Height == shadow_footprint.Footprint.Height &&
       footprint.Footprint.Depth == shadow_footprint.Footprint.Depth)
      shadow->buffer = pipe_buffer_create(screen, 0, PIPE_USAGE_DEFAULT, (unsigned)size);
   if (!shadow->buffer) {
      pipe_resource_reference(&shadow->texture, NULL);
      FREE(shadow);
      return NULL;
   }
   shadow->format = format;
   shadow->next = res->nxbox_srv_shadows;
   res->nxbox_srv_shadows = shadow;
   return shadow;
}

void
nxbox_refresh_srv_shadow(struct d3d12_context *ctx, struct d3d12_resource *source,
                         struct nxbox_srv_shadow *shadow)
{
   /* Ponytail: without a complete write-generation audit, recopy on every bind and before
    * each draw/dispatch. This costs two GPU copies per mip/layer even for unchanged textures.
    * Keep the pre-draw refresh: a bound source can be written without rebinding its SRV. */
   struct d3d12_resource *destination = d3d12_resource(shadow->texture);
   struct d3d12_resource *buffer = d3d12_resource(shadow->buffer);
   struct d3d12_screen *screen = d3d12_screen(ctx->base.screen);
   D3D12_RESOURCE_DESC source_desc = GetDesc(d3d12_resource_resource(source));
   D3D12_RESOURCE_DESC destination_desc = GetDesc(d3d12_resource_resource(destination));
   const unsigned layers = source->base.b.target == PIPE_TEXTURE_3D ? 1 : source->base.b.array_size;
   const unsigned count = (source->base.b.last_level + 1) * layers;
   uint64_t offset = 0;
   ID3D12Resource *buffer_resource = d3d12_resource_underlying(buffer, &offset);
   assert(offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);

   /* Internal maintenance copies must not inherit the application's render condition. */
   if (ctx->current_predication)
      nxbox_api(ctx->cmdlist, "srv-shadow")
         .SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
   for (unsigned subresource = 0; subresource < count; ++subresource) {
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = d3d12_resource_resource(source);
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = subresource;
      D3D12_TEXTURE_COPY_LOCATION dst = src;
      dst.pResource = d3d12_resource_resource(destination);
      D3D12_TEXTURE_COPY_LOCATION source_buffer = {};
      source_buffer.pResource = buffer_resource;
      source_buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      screen->dev->GetCopyableFootprints(&source_desc, subresource, 1, offset,
                                         &source_buffer.PlacedFootprint, nullptr, nullptr, nullptr);
      D3D12_TEXTURE_COPY_LOCATION destination_buffer = source_buffer;
      screen->dev->GetCopyableFootprints(&destination_desc, subresource, 1, offset,
                                         &destination_buffer.PlacedFootprint, nullptr, nullptr,
                                         nullptr);
      /* Equal texel sizes give identical padded rows and slices; only Format changes. */
      assert(source_buffer.PlacedFootprint.Footprint.RowPitch ==
             destination_buffer.PlacedFootprint.Footprint.RowPitch);
      struct copy_info info = {};
      info.src = source;
      info.src_loc = src;
      info.dst = buffer;
      info.dst_loc = source_buffer;
      copy_texture_region(ctx, info);
      info.src = buffer;
      info.src_loc = destination_buffer;
      info.dst = destination;
      info.dst_loc = dst;
      copy_texture_region(ctx, info);
   }
   if (ctx->current_predication)
      d3d12_enable_predication(ctx);
   nxbox_count_view_cast("srv_copy", (unsigned)shadow->format, (unsigned)source->overall_format);
}
