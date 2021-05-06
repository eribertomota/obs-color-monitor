#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include "plugin-macros.generated.h"

#define debug(format, ...)

#define VS_SIZE 256
#define SOURCE_CHECK_NS 3000000000

gs_effect_t *vss_effect;

struct vss_source
{
	gs_texrender_t *texrender;
	gs_stagesurf_t* stagesurface;
	uint32_t known_width;
	uint32_t known_height;

	gs_texture_t *tex_vs;
	uint8_t *tex_buf;

	pthread_mutex_t target_update_mutex;
	uint64_t target_check_time;
	obs_weak_source_t *weak_target;
	char *target_name;

	int target_scale;
	int intensity;
	int colorspace;
	int colorspace_calc; // get from ovi if auto
	bool colorspace_updated;
	bool bypass_vectorscope;

	bool rendered;
};

static const char *vss_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Vectorscope");
}

static void vss_update(void *, obs_data_t *);

static void *vss_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct vss_source *src = bzalloc(sizeof(struct vss_source));

	obs_enter_graphics();
	src->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!vss_effect) {
		char *f = obs_module_file("vectorscope.effect");
		vss_effect = gs_effect_create_from_file(f, NULL);
		if (!vss_effect)
			blog(LOG_ERROR, "Cannot load '%s'", f);
		bfree(f);
	}
	obs_leave_graphics();
	pthread_mutex_init(&src->target_update_mutex, NULL);

	vss_update(src, settings);

	return src;
}

static void vss_destroy(void *data)
{
	struct vss_source *src = data;

	obs_enter_graphics();
	gs_stagesurface_destroy(src->stagesurface);
	gs_texrender_destroy(src->texrender);

	gs_texture_destroy(src->tex_vs);
	bfree(src->tex_buf);
	obs_leave_graphics();

	bfree(src->target_name);
	pthread_mutex_destroy(&src->target_update_mutex);
	obs_weak_source_release(src->weak_target);

	bfree(src);
}

static void vss_update(void *data, obs_data_t *settings)
{
	struct vss_source *src = data;
	obs_weak_source_t *weak_source_old = NULL;

	const char *target_name = obs_data_get_string(settings, "target_name");
	if (!src->target_name || strcmp(target_name, src->target_name)) {
		pthread_mutex_lock(&src->target_update_mutex);
		bfree(src->target_name);
		src->target_name = bstrdup(target_name);
		weak_source_old = src->weak_target;
		src->weak_target = NULL;
		src->target_check_time = os_gettime_ns() - SOURCE_CHECK_NS;
		pthread_mutex_unlock(&src->target_update_mutex);
	}

	if (weak_source_old) {
		obs_weak_source_release(weak_source_old);
		weak_source_old = NULL;
	}

	src->target_scale = obs_data_get_int(settings, "target_scale");
	if (src->target_scale<1)
		src->target_scale = 1;

	src->intensity = obs_data_get_int(settings, "intensity");
	if (src->intensity<1)
		src->intensity = 1;

	int colorspace = obs_data_get_int(settings, "colorspace");
	if (colorspace!=src->colorspace) {
		src->colorspace = colorspace;
		src->colorspace_updated = 1;
	}
	src->bypass_vectorscope = obs_data_get_bool(settings, "bypass_vectorscope");
}

static void vss_get_defaults(obs_data_t *settings)
{
}

static bool add_sources(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;
	uint32_t caps = obs_source_get_output_flags(source);

	if (~caps & OBS_SOURCE_VIDEO)
		return true;

	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(prop, name, name);
	return true;
}

static obs_properties_t *vss_get_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props;
	obs_property_t *prop;
	props = obs_properties_create();

	prop = obs_properties_add_list(props, "target_name", obs_module_text("Source"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_enum_scenes(add_sources, prop);
	obs_enum_sources(add_sources, prop);

	obs_properties_add_int(props, "target_scale", obs_module_text("Scale"), 1, 128, 1);
	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);
	prop = obs_properties_add_list(props, "colorspace", obs_module_text("Color space"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, "Auto", 0);
	obs_property_list_add_int(prop, "601", 1);
	obs_property_list_add_int(prop, "709", 2);

	obs_properties_add_bool(props, "bypass_vectorscope", obs_module_text("Bypass"));

	return props;
}

static uint32_t vss_get_width(void *data)
{
	struct vss_source *src = data;
	return src->bypass_vectorscope ? src->known_width : VS_SIZE;
}

static uint32_t vss_get_height(void *data)
{
	struct vss_source *src = data;
	return src->bypass_vectorscope ? src->known_height : VS_SIZE;
}

static inline void vss_draw_vectorscope(struct vss_source *src, uint8_t *video_data, uint32_t video_line)
{
	if (!src->tex_buf)
		src->tex_buf = bzalloc(VS_SIZE*VS_SIZE);
	uint8_t *dbuf = src->tex_buf;

	for (int i=0; i<VS_SIZE*VS_SIZE; i++)
		dbuf[i] = 0;

	const uint32_t height = src->known_height;
	const uint32_t width = src->known_width;
	for (uint32_t y=0; y<height; y++) {
		uint8_t *v = video_data + video_line * y;
		for (uint32_t x=0; x<width; x++) {
			const uint8_t b = *v++;
			const uint8_t g = *v++;
			const uint8_t r = *v++;
			const uint8_t a = *v++;
			if (!a) continue;
			int u;
			int v;
			switch (src->colorspace_calc) {
				case 1:// BT.601
					// y = (+306*r +601*g +117*b)/1024 +0;
					u = (-172*r -338*g +512*b)/1024 +128;
					v = (+512*r -428*g -82*b)/1024 +128;
					break;
				case 2: // BT.709
					// y = (+218*r +732*g +74*b)/1024 +16;
					u = (-89*r -301*g +392*b)/1024 +128;
					v = (+553*r -501*g -50*b)/1024 +128;
					break;
					u = -1;
					v = -1;
			}
			if (u<0 || 255<u || v<0 || 255<v)
				continue;
			uint8_t *c = dbuf + (u + VS_SIZE*(255-v));
			if (*c<255) ++*c;
		}
	}

	gs_texture_destroy(src->tex_vs);
	src->tex_vs = gs_texture_create(VS_SIZE, VS_SIZE, GS_R8, 1, (const uint8_t**)&src->tex_buf, 0);
}

static void vss_render_target(struct vss_source *src)
{
	if (src->rendered)
		return;
	src->rendered = 1;

	gs_texrender_reset(src->texrender);

	obs_source_t *target = obs_weak_source_get_source(src->weak_target);
	if (!target)
		return;

	int target_width = obs_source_get_width(target);
	int target_height = obs_source_get_height(target);
	int width = target_width / src->target_scale;
	int height = target_height / src->target_scale;
	if (width<=0 || height<=0)
		goto end;

	if (gs_texrender_begin(src->texrender, width, height)) {
		struct vec4 background;
		vec4_zero(&background);

		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, (float)target_width, 0.0f, (float)target_height, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		obs_source_video_render(target);

		gs_blend_state_pop();

		gs_texrender_end(src->texrender);

		if (width != src->known_width || height != src->known_height) {
			gs_stagesurface_destroy(src->stagesurface);
			src->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
			src->known_width = width;
			src->known_height = height;
		}

		gs_stage_texture(src->stagesurface, gs_texrender_get_texture(src->texrender));
		uint8_t *video_data = NULL;
		uint32_t video_linesize;
		if (gs_stagesurface_map(src->stagesurface, &video_data, &video_linesize)) {
			if (src->bypass_vectorscope) {
				gs_texture_destroy(src->tex_vs);
				src->tex_vs = gs_texture_create(width, height, GS_BGRA, 1, (const uint8_t**)&video_data, 0);
			}
			else
				vss_draw_vectorscope(src, video_data, video_linesize);
		}
		gs_stagesurface_unmap(src->stagesurface);

	}

end:
	obs_source_release(target);
}

static void vss_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct vss_source *src = data;

	if (src->colorspace_updated || src->colorspace_calc<1) {
		src->colorspace_calc = src->colorspace;
		src->colorspace_updated = 0;
		if (src->colorspace_calc<1 || 2<src->colorspace_calc) {
			struct obs_video_info ovi;
			if (obs_get_video_info(&ovi)) {
				switch (ovi.colorspace) {
					case VIDEO_CS_601:
						src->colorspace_calc = 1;
						break;
					case VIDEO_CS_709:
					default:
						src->colorspace_calc = 2;
						break;
				}
			}
		}
	}

	vss_render_target(src);

	if (src->bypass_vectorscope && src->tex_vs) {
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src->tex_vs);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(src->tex_vs, 0, src->known_width, src->known_height);
		}
	}
	else if (src->tex_vs) {
		gs_effect_t *effect = vss_effect ? vss_effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src->tex_vs);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "intensity"), src->intensity);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(src->tex_vs, 0, VS_SIZE, VS_SIZE);
		}
	}
}

static void vss_tick(void *data, float unused)
{
	UNUSED_PARAMETER(unused);
	struct vss_source *src = data;

	pthread_mutex_lock(&src->target_update_mutex);
	if (src->target_name && *src->target_name && !src->weak_target && src->target_check_time) {
		uint64_t t = os_gettime_ns();
		if (t - src->target_check_time > SOURCE_CHECK_NS) {
			src->target_check_time = t;
			obs_source_t *target = obs_get_source_by_name(src->target_name);
			src->weak_target = target ? obs_source_get_weak_source(target) : NULL;
			obs_source_release(target);
		}
	}
	pthread_mutex_unlock(&src->target_update_mutex);

	src->rendered = 0;
}

struct obs_source_info colormonitor_vectorscope = {
	.id = "vectorscope_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = vss_get_name,
	.create = vss_create,
	.destroy = vss_destroy,
	.update = vss_update,
	.get_defaults = vss_get_defaults,
	.get_properties = vss_get_properties,
	.get_width = vss_get_width,
	.get_height = vss_get_height,
	.video_render = vss_render,
	.video_tick = vss_tick,
};
