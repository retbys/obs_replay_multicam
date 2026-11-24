/*
 * Источник «Multicam Replay»: хранит клипы (снимки всех камер группы) и
 * проигрывает выбранную камеру с регулируемой скоростью. Одновременно
 * воспроизводится только одна камера, поэтому нагрузка равна нагрузке от
 * обычного видеоисточника. Переключение камер сохраняет позицию на таймлайне.
 */
#include "mc-common.h"
#include "mc-api.h"
#include <string.h>
#include <stdio.h>

#define T(s) obs_module_text(s)

#define S_GROUP "group"
#define S_CLIP_SEC "clip_seconds"
#define S_POST_MS "post_ms"
#define S_SPEED "speed"
#define S_CAMERA "camera"
#define S_END "end_action"
#define S_MAX_CLIPS "max_clips"
#define S_MEM_MB "mem_mb"
#define S_AUTOPLAY "autoplay"

enum { END_HOLD = 0, END_LOOP = 1, END_HIDE = 2 };

#define HK_MAX 32

struct mc_replay;
typedef void (*mc_action_fn)(struct mc_replay *, int);

struct mc_hk_ctx {
	struct mc_replay *r;
	mc_action_fn fn;
	int arg;
};

struct mc_replay {
	obs_source_t *source;
	pthread_mutex_t mutex;

	/* настройки */
	char *group;
	int clip_seconds;
	int post_ms;
	double speed; /* проценты, 100 = реальное время */
	int cam;      /* 0..3 */
	int end_action;
	int max_clips;
	size_t mem_limit;
	bool autoplay;

	/* клипы, старые первыми */
	struct mc_clip *clips;
	size_t nclips, cap_clips;
	size_t cur;

	/* состояние воспроизведения */
	bool playing, backward, hidden, shown, need_output;
	int64_t pos; /* смещение от clips[cur].t0, нс */
	uint64_t pending_at;
	struct mc_frame *last_out;

	struct mc_hk_ctx hk[HK_MAX];
	obs_hotkey_id hk_ids[HK_MAX];
	size_t hk_count;

	struct mc_replay *next_reg; /* реестр экземпляров для окна управления */
};

static pthread_mutex_t g_reg_mutex;
static struct mc_replay *g_reg;

/* ---------- вспомогательное (mutex уже взят) ---------- */

static inline int64_t clip_duration(const struct mc_clip *c)
{
	return (int64_t)(c->t1 - c->t0);
}

static size_t find_idx(struct mc_frame **fr, size_t n, uint64_t t)
{
	size_t lo = 0, hi = n; /* первый кадр с ts > t */
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (fr[mid]->ts <= t)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo ? lo - 1 : 0;
}

static void drop_oldest(struct mc_replay *r)
{
	if (!r->nclips)
		return;
	mc_clip_free(&r->clips[0]);
	memmove(&r->clips[0], &r->clips[1], (r->nclips - 1) * sizeof(r->clips[0]));
	r->nclips--;
	if (r->cur > 0)
		r->cur--;
	r->last_out = NULL;
	r->need_output = true;
}

static void start_play(struct mc_replay *r)
{
	if (!r->nclips)
		return;
	r->pos = r->backward ? clip_duration(&r->clips[r->cur]) : 0;
	r->playing = true;
	r->hidden = false;
	r->need_output = true;
}

static void do_snapshot(struct mc_replay *r, uint64_t t_end)
{
	struct mc_clip clip;
	if (!mc_snapshot_group(r->group, t_end, (uint64_t)r->clip_seconds * 1000000000ULL, &clip)) {
		blog(LOG_WARNING, "[multicam-replay] group '%s': no frames to save (is the capture filter added and enabled?)",
		     r->group);
		return;
	}

	if (r->nclips == r->cap_clips) {
		const size_t ncap = r->cap_clips ? r->cap_clips * 2 : 8;
		struct mc_clip *n = bmalloc(sizeof(*n) * ncap);
		if (r->nclips)
			memcpy(n, r->clips, sizeof(*n) * r->nclips);
		bfree(r->clips);
		r->clips = n;
		r->cap_clips = ncap;
	}
	r->clips[r->nclips++] = clip;

	while ((int)r->nclips > r->max_clips && r->nclips > 1)
		drop_oldest(r);
	while (r->nclips > 1 && mc_mem_in_use() > r->mem_limit)
		drop_oldest(r);

	r->cur = r->nclips - 1;
	r->last_out = NULL;
	if (r->autoplay) {
		start_play(r);
	} else {
		r->pos = r->backward ? clip_duration(&r->clips[r->cur]) : 0;
		r->playing = false;
		r->hidden = false;
		r->need_output = true;
	}

	size_t frames = 0;
	for (int i = 0; i < MC_MAX_CAMS; i++)
		frames += clip.count[i];
	blog(LOG_INFO, "[multicam-replay] saved clip %d/%d: %.2f s, %d frames, %.0f MB in use", (int)r->nclips,
	     r->max_clips, (double)clip_duration(&r->clips[r->cur]) / 1e9, (int)frames,
	     (double)mc_mem_in_use() / (1024.0 * 1024.0));
}

static void end_reached(struct mc_replay *r, int64_t dur, bool backward)
{
	int action = r->end_action;
	if (dur <= 0 && action == END_LOOP)
		action = END_HOLD;

	r->pos = backward ? 0 : dur;
	switch (action) {
	case END_LOOP:
		r->pos = backward ? dur : 0;
		break;
	case END_HIDE:
		r->playing = false;
		r->hidden = true;
		break;
	default:
		r->playing = false;
		break;
	}
	r->need_output = true;
}

static void set_setting_double(struct mc_replay *r, const char *key, double v)
{
	obs_data_t *s = obs_source_get_settings(r->source);
	obs_data_set_double(s, key, v);
	obs_data_release(s);
}

static void set_setting_int(struct mc_replay *r, const char *key, long long v)
{
	obs_data_t *s = obs_source_get_settings(r->source);
	obs_data_set_int(s, key, v);
	obs_data_release(s);
}

/* ---------- действия (вызываются из хоткеев и кнопок) ---------- */

static void act_save(struct mc_replay *r, int arg)
{
	UNUSED_PARAMETER(arg);
	pthread_mutex_lock(&r->mutex);
	const uint64_t now = os_gettime_ns();
	if (r->post_ms > 0)
		r->pending_at = now + (uint64_t)r->post_ms * 1000000ULL;
	else
		do_snapshot(r, now);
	pthread_mutex_unlock(&r->mutex);
}

static void act_play_pause(struct mc_replay *r, int arg)
{
	UNUSED_PARAMETER(arg);
	pthread_mutex_lock(&r->mutex);
	if (r->nclips) {
		if (r->playing) {
			r->playing = false;
		} else {
			const int64_t dur = clip_duration(&r->clips[r->cur]);
			if (!r->backward && r->pos >= dur)
				r->pos = 0;
			if (r->backward && r->pos <= 0)
				r->pos = dur;
			r->playing = true;
			r->hidden = false;
			r->need_output = true;
		}
	}
	pthread_mutex_unlock(&r->mutex);
}

static void act_restart(struct mc_replay *r, int arg)
{
	UNUSED_PARAMETER(arg);
	pthread_mutex_lock(&r->mutex);
	start_play(r);
	pthread_mutex_unlock(&r->mutex);
}

static void act_reverse(struct mc_replay *r, int arg)
{
	UNUSED_PARAMETER(arg);
	pthread_mutex_lock(&r->mutex);
	r->backward = !r->backward;
	if (r->nclips && !r->playing) {
		const int64_t dur = clip_duration(&r->clips[r->cur]);
		if ((!r->backward && r->pos >= dur) || (r->backward && r->pos <= 0))
			r->pos = r->backward ? dur : 0;
		r->playing = true;
		r->hidden = false;
		r->need_output = true;
	}
	pthread_mutex_unlock(&r->mutex);
}

static void act_camera(struct mc_replay *r, int cam)
{
	pthread_mutex_lock(&r->mutex);
	if (cam >= 0 && cam < MC_MAX_CAMS) {
		r->cam = cam;
		r->need_output = true;
		r->last_out = NULL;
		set_setting_int(r, S_CAMERA, cam + 1);
	}
	pthread_mutex_unlock(&r->mutex);
}

static void act_speed_mul(struct mc_replay *r, int dir)
{
	pthread_mutex_lock(&r->mutex);
	double v = dir > 0 ? r->speed * 1.5 : r->speed / 1.5;
	if (v < 5.0)
		v = 5.0;
	if (v > 400.0)
		v = 400.0;
	r->speed = v;
	set_setting_double(r, S_SPEED, v);
	pthread_mutex_unlock(&r->mutex);
}

static void act_speed_set(struct mc_replay *r, int percent)
{
	pthread_mutex_lock(&r->mutex);
	r->speed = percent;
	set_setting_double(r, S_SPEED, percent);
	pthread_mutex_unlock(&r->mutex);
}

static void act_step(struct mc_replay *r, int dir)
{
	pthread_mutex_lock(&r->mutex);
	if (r->nclips) {
		struct mc_clip *c = &r->clips[r->cur];
		const size_t n = c->count[r->cam];
		if (n) {
			int64_t p = r->pos;
			if (p < 0)
				p = 0;
			if (p > clip_duration(c))
				p = clip_duration(c);
			size_t idx = find_idx(c->frames[r->cam], n, c->t0 + (uint64_t)p);
			if (dir > 0 && idx + 1 < n)
				idx++;
			else if (dir < 0 && idx > 0)
				idx--;
			r->pos = (int64_t)(c->frames[r->cam][idx]->ts - c->t0);
			r->playing = false;
			r->hidden = false;
			r->need_output = true;
		}
	}
	pthread_mutex_unlock(&r->mutex);
}

/* arg: 1 следующий, -1 предыдущий, 2 последний */
static void act_clip_nav(struct mc_replay *r, int arg)
{
	pthread_mutex_lock(&r->mutex);
	if (r->nclips) {
		if (arg == 2)
			r->cur = r->nclips - 1;
		else if (arg > 0 && r->cur + 1 < r->nclips)
			r->cur++;
		else if (arg < 0 && r->cur > 0)
			r->cur--;
		r->last_out = NULL;
		start_play(r);
	}
	pthread_mutex_unlock(&r->mutex);
}

static void act_hide(struct mc_replay *r, int arg)
{
	UNUSED_PARAMETER(arg);
	pthread_mutex_lock(&r->mutex);
	r->playing = false;
	r->hidden = true;
	r->need_output = true;
	pthread_mutex_unlock(&r->mutex);
}

static void act_clear(struct mc_replay *r, int arg)
{
	UNUSED_PARAMETER(arg);
	pthread_mutex_lock(&r->mutex);
	while (r->nclips)
		drop_oldest(r);
	r->cur = 0;
	r->playing = false;
	r->pending_at = 0;
	r->need_output = true;
	pthread_mutex_unlock(&r->mutex);
}

/* ---------- хоткеи ---------- */

struct mc_hk_def {
	const char *name;
	const char *text;
	mc_action_fn fn;
	int arg;
};

static const struct mc_hk_def hk_defs[] = {
	{"mc_save", "HotkeySave", act_save, 0},
	{"mc_play_pause", "HotkeyPlayPause", act_play_pause, 0},
	{"mc_restart", "HotkeyRestart", act_restart, 0},
	{"mc_reverse", "HotkeyReverse", act_reverse, 0},
	{"mc_cam1", "HotkeyCam1", act_camera, 0},
	{"mc_cam2", "HotkeyCam2", act_camera, 1},
	{"mc_cam3", "HotkeyCam3", act_camera, 2},
	{"mc_cam4", "HotkeyCam4", act_camera, 3},
	{"mc_faster", "HotkeyFaster", act_speed_mul, 1},
	{"mc_slower", "HotkeySlower", act_speed_mul, -1},
	{"mc_speed_100", "HotkeySpeed100", act_speed_set, 100},
	{"mc_speed_50", "HotkeySpeed50", act_speed_set, 50},
	{"mc_speed_25", "HotkeySpeed25", act_speed_set, 25},
	{"mc_step_fwd", "HotkeyStepFwd", act_step, 1},
	{"mc_step_back", "HotkeyStepBack", act_step, -1},
	{"mc_prev_clip", "HotkeyPrevClip", act_clip_nav, -1},
	{"mc_next_clip", "HotkeyNextClip", act_clip_nav, 1},
	{"mc_last_clip", "HotkeyLastClip", act_clip_nav, 2},
	{"mc_hide", "HotkeyHide", act_hide, 0},
	{"mc_clear", "HotkeyClear", act_clear, 0},
};
#define HK_COUNT (sizeof(hk_defs) / sizeof(hk_defs[0]))

static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct mc_hk_ctx *ctx = data;
	if (pressed)
		ctx->fn(ctx->r, ctx->arg);
}

/* ---------- источник ---------- */

static const char *mc_replay_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T("ReplaySource");
}

static void mc_replay_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, S_GROUP, "main");
	obs_data_set_default_int(s, S_CLIP_SEC, 8);
	obs_data_set_default_int(s, S_POST_MS, 0);
	obs_data_set_default_double(s, S_SPEED, 100.0);
	obs_data_set_default_int(s, S_CAMERA, 1);
	obs_data_set_default_int(s, S_END, END_HOLD);
	obs_data_set_default_int(s, S_MAX_CLIPS, 4);
	obs_data_set_default_int(s, S_MEM_MB, 8192);
	obs_data_set_default_bool(s, S_AUTOPLAY, true);
}

static void mc_replay_update(void *data, obs_data_t *s)
{
	struct mc_replay *r = data;

	const char *g = obs_data_get_string(s, S_GROUP);
	int cam = (int)obs_data_get_int(s, S_CAMERA) - 1;
	int clip = (int)obs_data_get_int(s, S_CLIP_SEC);
	int maxc = (int)obs_data_get_int(s, S_MAX_CLIPS);
	int mem = (int)obs_data_get_int(s, S_MEM_MB);
	double speed = obs_data_get_double(s, S_SPEED);

	if (cam < 0)
		cam = 0;
	if (cam >= MC_MAX_CAMS)
		cam = MC_MAX_CAMS - 1;
	if (clip < 1)
		clip = 1;
	if (maxc < 1)
		maxc = 1;
	if (mem < 256)
		mem = 256;
	if (speed < 5.0)
		speed = 5.0;
	if (speed > 400.0)
		speed = 400.0;

	pthread_mutex_lock(&r->mutex);
	bfree(r->group);
	r->group = bstrdup(g && *g ? g : "main");
	r->clip_seconds = clip;
	r->post_ms = (int)obs_data_get_int(s, S_POST_MS);
	r->speed = speed;
	if (r->cam != cam) {
		r->cam = cam;
		r->need_output = true;
		r->last_out = NULL;
	}
	r->end_action = (int)obs_data_get_int(s, S_END);
	r->max_clips = maxc;
	r->mem_limit = (size_t)mem * 1024 * 1024;
	r->autoplay = obs_data_get_bool(s, S_AUTOPLAY);
	while ((int)r->nclips > r->max_clips && r->nclips > 1)
		drop_oldest(r);
	pthread_mutex_unlock(&r->mutex);
}

static void *mc_replay_create(obs_data_t *settings, obs_source_t *source)
{
	struct mc_replay *r = bzalloc(sizeof(*r));
	r->source = source;
	pthread_mutex_init(&r->mutex, NULL);
	mc_replay_update(r, settings);

	/* Кадры отдаём сразу, без буферизации по таймстемпам. */
	obs_source_set_async_unbuffered(source, true);

	for (size_t i = 0; i < HK_COUNT && i < HK_MAX; i++) {
		r->hk[i].r = r;
		r->hk[i].fn = hk_defs[i].fn;
		r->hk[i].arg = hk_defs[i].arg;
		r->hk_ids[i] = obs_hotkey_register_source(source, hk_defs[i].name, T(hk_defs[i].text), hotkey_cb, &r->hk[i]);
		r->hk_count++;
	}

	pthread_mutex_lock(&g_reg_mutex);
	r->next_reg = g_reg;
	g_reg = r;
	pthread_mutex_unlock(&g_reg_mutex);
	return r;
}

static void mc_replay_destroy(void *data)
{
	struct mc_replay *r = data;

	pthread_mutex_lock(&g_reg_mutex);
	for (struct mc_replay **pp = &g_reg; *pp; pp = &(*pp)->next_reg) {
		if (*pp == r) {
			*pp = r->next_reg;
			break;
		}
	}
	pthread_mutex_unlock(&g_reg_mutex);

	for (size_t i = 0; i < r->hk_count; i++)
		obs_hotkey_unregister(r->hk_ids[i]);

	pthread_mutex_lock(&r->mutex);
	while (r->nclips)
		drop_oldest(r);
	pthread_mutex_unlock(&r->mutex);

	bfree(r->clips);
	bfree(r->group);
	pthread_mutex_destroy(&r->mutex);
	bfree(r);
}

static void mc_replay_tick(void *data, float seconds)
{
	struct mc_replay *r = data;

	pthread_mutex_lock(&r->mutex);

	if (r->pending_at && os_gettime_ns() >= r->pending_at) {
		const uint64_t t = r->pending_at;
		r->pending_at = 0;
		do_snapshot(r, t);
	}

	if (!r->nclips) {
		if (r->shown) {
			obs_source_output_video(r->source, NULL);
			r->shown = false;
		}
		pthread_mutex_unlock(&r->mutex);
		return;
	}

	struct mc_clip *c = &r->clips[r->cur];
	const int64_t dur = clip_duration(c);

	if (r->playing && seconds > 0.0f) {
		const double step = (double)seconds * 1e9 * (r->speed / 100.0);
		r->pos += (int64_t)(r->backward ? -step : step);
		if (!r->backward && r->pos >= dur)
			end_reached(r, dur, false);
		else if (r->backward && r->pos <= 0)
			end_reached(r, dur, true);
	}
	if (r->pos < 0)
		r->pos = 0;
	if (r->pos > dur)
		r->pos = dur;

	const size_t n = c->count[r->cam];
	if (r->hidden || !n) {
		if (r->shown) {
			obs_source_output_video(r->source, NULL);
			r->shown = false;
			r->last_out = NULL;
		}
		pthread_mutex_unlock(&r->mutex);
		return;
	}

	const size_t idx = find_idx(c->frames[r->cam], n, c->t0 + (uint64_t)r->pos);
	struct mc_frame *f = c->frames[r->cam][idx];

	if (r->need_output || f != r->last_out || !r->shown) {
		struct obs_source_frame out = f->meta;
		out.timestamp = os_gettime_ns();
		obs_source_output_video(r->source, &out);
		r->last_out = f;
		r->shown = true;
		r->need_output = false;
	}

	pthread_mutex_unlock(&r->mutex);
}

/* ---------- кнопки в свойствах (для проверки без хоткеев) ---------- */

#define BTN(name, fn, arg)                                                                     \
	static bool name(obs_properties_t *p, obs_property_t *prop, void *data)                \
	{                                                                                      \
		UNUSED_PARAMETER(p);                                                           \
		UNUSED_PARAMETER(prop);                                                        \
		fn((struct mc_replay *)data, arg);                                             \
		return false;                                                                  \
	}

BTN(btn_save, act_save, 0)
BTN(btn_play, act_play_pause, 0)
BTN(btn_cam1, act_camera, 0)
BTN(btn_cam2, act_camera, 1)
BTN(btn_cam3, act_camera, 2)
BTN(btn_cam4, act_camera, 3)
BTN(btn_prev, act_clip_nav, -1)
BTN(btn_next, act_clip_nav, 1)
BTN(btn_clear, act_clear, 0)

static obs_properties_t *mc_replay_properties(void *data)
{
	obs_properties_t *p = obs_properties_create();

	obs_properties_add_text(p, S_GROUP, T("Group"), OBS_TEXT_DEFAULT);
	obs_property_t *clip = obs_properties_add_int_slider(p, S_CLIP_SEC, T("ClipLength"), 1, 120, 1);
	obs_property_int_set_suffix(clip, " s");
	obs_property_t *post = obs_properties_add_int(p, S_POST_MS, T("PostRoll"), 0, 30000, 100);
	obs_property_int_set_suffix(post, " ms");
	obs_property_t *sp = obs_properties_add_float_slider(p, S_SPEED, T("Speed"), 5.0, 400.0, 1.0);
	obs_property_float_set_suffix(sp, " %");

	obs_property_t *cam = obs_properties_add_list(p, S_CAMERA, T("Camera"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 1; i <= MC_MAX_CAMS; i++) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%s %d", T("CameraN"), i);
		obs_property_list_add_int(cam, buf, i);
	}

	obs_property_t *end = obs_properties_add_list(p, S_END, T("EndAction"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(end, T("EndHold"), END_HOLD);
	obs_property_list_add_int(end, T("EndLoop"), END_LOOP);
	obs_property_list_add_int(end, T("EndHide"), END_HIDE);

	obs_properties_add_int(p, S_MAX_CLIPS, T("MaxClips"), 1, 50, 1);
	obs_property_t *mem = obs_properties_add_int(p, S_MEM_MB, T("MemLimit"), 256, 262144, 256);
	obs_property_int_set_suffix(mem, " MB");
	obs_properties_add_bool(p, S_AUTOPLAY, T("AutoPlay"));

	obs_properties_add_button2(p, "btn_save", T("BtnSave"), btn_save, data);
	obs_properties_add_button2(p, "btn_play", T("BtnPlayPause"), btn_play, data);
	obs_properties_add_button2(p, "btn_cam1", T("BtnCam1"), btn_cam1, data);
	obs_properties_add_button2(p, "btn_cam2", T("BtnCam2"), btn_cam2, data);
	obs_properties_add_button2(p, "btn_cam3", T("BtnCam3"), btn_cam3, data);
	obs_properties_add_button2(p, "btn_cam4", T("BtnCam4"), btn_cam4, data);
	obs_properties_add_button2(p, "btn_prev", T("BtnPrev"), btn_prev, data);
	obs_properties_add_button2(p, "btn_next", T("BtnNext"), btn_next, data);
	obs_properties_add_button2(p, "btn_clear", T("BtnClear"), btn_clear, data);
	return p;
}

struct obs_source_info mc_replay_source_info = {
	.id = "multicam_replay_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name = mc_replay_name,
	.create = mc_replay_create,
	.destroy = mc_replay_destroy,
	.update = mc_replay_update,
	.get_defaults = mc_replay_defaults,
	.get_properties = mc_replay_properties,
	.video_tick = mc_replay_tick,
};

/* ---------- API для окна управления (mc-control.cpp) ---------- */

void mc_replay_registry_init(void)
{
	pthread_mutex_init(&g_reg_mutex, NULL);
	g_reg = NULL;
}

void mc_replay_registry_shutdown(void)
{
	pthread_mutex_destroy(&g_reg_mutex);
}

static struct mc_replay *reg_find(const char *name)
{
	for (struct mc_replay *r = g_reg; r; r = r->next_reg) {
		const char *n = obs_source_get_name(r->source);
		if (n && name && strcmp(n, name) == 0)
			return r;
	}
	return NULL;
}

void mc_replay_enum_names(mc_name_cb cb, void *param)
{
	pthread_mutex_lock(&g_reg_mutex);
	for (struct mc_replay *r = g_reg; r; r = r->next_reg) {
		const char *n = obs_source_get_name(r->source);
		if (n)
			cb(n, param);
	}
	pthread_mutex_unlock(&g_reg_mutex);
}

bool mc_replay_trigger(const char *source_name, const char *action)
{
	bool ok = false;
	pthread_mutex_lock(&g_reg_mutex);
	struct mc_replay *r = reg_find(source_name);
	if (r) {
		for (size_t i = 0; i < HK_COUNT; i++) {
			if (strcmp(hk_defs[i].name, action) == 0) {
				hk_defs[i].fn(r, hk_defs[i].arg);
				ok = true;
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_reg_mutex);
	return ok;
}

bool mc_replay_set_speed(const char *source_name, double percent)
{
	bool ok = false;
	pthread_mutex_lock(&g_reg_mutex);
	struct mc_replay *r = reg_find(source_name);
	if (r) {
		pthread_mutex_lock(&r->mutex);
		if (percent < 5.0)
			percent = 5.0;
		if (percent > 400.0)
			percent = 400.0;
		r->speed = percent;
		set_setting_double(r, S_SPEED, percent);
		pthread_mutex_unlock(&r->mutex);
		ok = true;
	}
	pthread_mutex_unlock(&g_reg_mutex);
	return ok;
}

bool mc_replay_get_status(const char *source_name, struct mc_replay_status *out)
{
	bool ok = false;
	pthread_mutex_lock(&g_reg_mutex);
	struct mc_replay *r = reg_find(source_name);
	if (r) {
		pthread_mutex_lock(&r->mutex);
		out->clips = (int)r->nclips;
		out->cur = r->nclips ? (int)r->cur + 1 : 0;
		out->cam = r->cam + 1;
		out->speed = r->speed;
		out->playing = r->playing;
		out->backward = r->backward;
		out->hidden = r->hidden;
		out->post_pending = r->pending_at != 0;
		out->pos_sec = r->nclips ? (double)r->pos / 1e9 : 0.0;
		out->dur_sec = r->nclips ? (double)clip_duration(&r->clips[r->cur]) / 1e9 : 0.0;
		out->mem_mb = (double)mc_mem_in_use() / (1024.0 * 1024.0);
		pthread_mutex_unlock(&r->mutex);
		ok = true;
	}
	pthread_mutex_unlock(&g_reg_mutex);
	return ok;
}
