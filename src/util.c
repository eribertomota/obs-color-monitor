#include <obs-module.h>
#include "plugin-macros.generated.h"
#include "util.h"

gs_effect_t *create_effect_from_module_file(const char *basename)
{
	char *f = obs_module_file(basename);
	gs_effect_t *effect = gs_effect_create_from_file(f, NULL);
	if (!effect)
		blog(LOG_ERROR, "Cannot load '%s' '%s'", basename, f);
	bfree(f);
	return effect;
}

obs_property_t *properties_add_colorspace(obs_properties_t *props, const char *name, const char *description)
{
	obs_property_t *prop =
		obs_properties_add_list(props, name, description, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Auto"), 0);
	obs_property_list_add_int(prop, obs_module_text("601"), 1);
	obs_property_list_add_int(prop, obs_module_text("709"), 2);
	return prop;
}

int calc_colorspace(int colorspace)
{
	if (1 <= colorspace && colorspace <= 2)
		return colorspace;
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		switch (ovi.colorspace) {
		case VIDEO_CS_601:
			return 1;
		case VIDEO_CS_709:
			return 2;
		default:
			return 2; // TODO: Implement
		}
	}
	return 2; // default
}
