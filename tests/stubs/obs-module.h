/* Заглушка API libobs ТОЛЬКО для проверки логики плагина без OBS. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#define MAKE_SEMANTIC_VERSION(major, minor, patch) (((major) << 24) | ((minor) << 16) | (patch))
#define LIBOBS_API_VER MAKE_SEMANTIC_VERSION(31, 0, 0)
#define UNUSED_PARAMETER(x) ((void)(x))
#define MODULE_EXPORT
#define OBS_DECLARE_MODULE()
#define OBS_MODULE_USE_DEFAULT_LOCALE(a, b)

#define LOG_ERROR 100
#define LOG_WARNING 200
#define LOG_INFO 300

enum video_format {
	VIDEO_FORMAT_NONE, VIDEO_FORMAT_I420, VIDEO_FORMAT_NV12, VIDEO_FORMAT_YVYU, VIDEO_FORMAT_YUY2,
	VIDEO_FORMAT_UYVY, VIDEO_FORMAT_RGBA, VIDEO_FORMAT_BGRA, VIDEO_FORMAT_BGRX, VIDEO_FORMAT_Y800,
	VIDEO_FORMAT_I444, VIDEO_FORMAT_BGR3, VIDEO_FORMAT_I422, VIDEO_FORMAT_I010, VIDEO_FORMAT_P010,
	VIDEO_FORMAT_I210,
};

struct obs_source_frame {
	uint8_t *data[8];
	uint32_t linesize[8];
	uint32_t width;
	uint32_t height;
	uint64_t timestamp;
	enum video_format format;
	float color_matrix[16];
	bool full_range;
	float color_range_min[3];
	float color_range_max[3];
	bool flip;
	volatile long refs;
	bool prev_frame;
};

typedef struct obs_data obs_data_t;
typedef struct obs_source obs_source_t;
typedef struct obs_properties obs_properties_t;
typedef struct obs_property obs_property_t;
typedef size_t obs_hotkey_id;
typedef struct obs_hotkey obs_hotkey_t;

enum obs_source_type { OBS_SOURCE_TYPE_INPUT, OBS_SOURCE_TYPE_FILTER };
#define OBS_SOURCE_ASYNC_VIDEO 1
enum { OBS_TEXT_DEFAULT, OBS_TEXT_INFO };
enum { OBS_COMBO_TYPE_LIST };
enum { OBS_COMBO_FORMAT_INT };

struct obs_source_info {
	const char *id;
	enum obs_source_type type;
	uint32_t output_flags;
	const char *(*get_name)(void *);
	void *(*create)(obs_data_t *, obs_source_t *);
	void (*destroy)(void *);
	void (*update)(void *, obs_data_t *);
	void (*get_defaults)(obs_data_t *);
	obs_properties_t *(*get_properties)(void *);
	struct obs_source_frame *(*filter_video)(void *, struct obs_source_frame *);
	void (*video_tick)(void *, float);
};

typedef bool (*obs_property_clicked_t)(obs_properties_t *, obs_property_t *, void *);

void *bmalloc(size_t);
void bfree(void *);
void *bzalloc(size_t);
char *bstrdup(const char *);
void blog(int level, const char *fmt, ...);
uint64_t os_gettime_ns(void);
long os_atomic_inc_long(volatile long *);
long os_atomic_dec_long(volatile long *);

const char *obs_module_text(const char *);

void obs_data_set_default_string(obs_data_t *, const char *, const char *);
void obs_data_set_default_int(obs_data_t *, const char *, long long);
void obs_data_set_default_double(obs_data_t *, const char *, double);
void obs_data_set_default_bool(obs_data_t *, const char *, bool);
const char *obs_data_get_string(obs_data_t *, const char *);
long long obs_data_get_int(obs_data_t *, const char *);
double obs_data_get_double(obs_data_t *, const char *);
bool obs_data_get_bool(obs_data_t *, const char *);
void obs_data_set_int(obs_data_t *, const char *, long long);
void obs_data_set_double(obs_data_t *, const char *, double);
void obs_data_release(obs_data_t *);

obs_data_t *obs_source_get_settings(const obs_source_t *);
const char *obs_source_get_name(const obs_source_t *);
void obs_source_output_video(obs_source_t *, const struct obs_source_frame *);
void obs_source_set_async_unbuffered(obs_source_t *, bool);
void obs_register_source(struct obs_source_info *);

obs_properties_t *obs_properties_create(void);
obs_property_t *obs_properties_add_text(obs_properties_t *, const char *, const char *, int);
obs_property_t *obs_properties_add_list(obs_properties_t *, const char *, const char *, int, int);
size_t obs_property_list_add_int(obs_property_t *, const char *, long long);
obs_property_t *obs_properties_add_int_slider(obs_properties_t *, const char *, const char *, int, int, int);
obs_property_t *obs_properties_add_int(obs_properties_t *, const char *, const char *, int, int, int);
obs_property_t *obs_properties_add_float_slider(obs_properties_t *, const char *, const char *, double, double, double);
obs_property_t *obs_properties_add_bool(obs_properties_t *, const char *, const char *);
obs_property_t *obs_properties_add_button2(obs_properties_t *, const char *, const char *, obs_property_clicked_t, void *);
void obs_property_int_set_suffix(obs_property_t *, const char *);
void obs_property_float_set_suffix(obs_property_t *, const char *);

obs_hotkey_id obs_hotkey_register_source(obs_source_t *, const char *, const char *,
					 void (*)(void *, obs_hotkey_id, obs_hotkey_t *, bool), void *);
void obs_hotkey_unregister(obs_hotkey_id);
