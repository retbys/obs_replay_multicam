/*
 * Фильтр «Multicam Replay Capture»: вешается на камеру (асинхронный источник),
 * пишет сырые кадры в кольцевой буфер в RAM. Никакого GPU, конвертации или
 * кодирования: на кадр приходится один memcpy из буфера камеры.
 */
#include "mc-common.h"
#include <string.h>
#include <stdio.h>

#define T(s) obs_module_text(s)

#define S_GROUP "group"
#define S_CAMERA "camera"
#define S_BUFFER "buffer_seconds"
#define S_MAXMB "max_mb"
#define S_ENABLED "enabled"

/* ---------- кольцевой буфер указателей на кадры ---------- */

struct mc_ring {
	struct mc_frame **items;
	size_t cap, head, count;
	size_t bytes;
};

static inline struct mc_frame *ring_at(const struct mc_ring *r, size_t i)
{
	return r->items[(r->head + i) % r->cap];
}

static void ring_push(struct mc_ring *r, struct mc_frame *f)
{
	if (r->count == r->cap) {
		const size_t ncap = r->cap ? r->cap * 2 : 128;
		struct mc_frame **n = bmalloc(sizeof(*n) * ncap);
		for (size_t i = 0; i < r->count; i++)
			n[i] = r->items[(r->head + i) % r->cap];
		bfree(r->items);
		r->items = n;
		r->cap = ncap;
		r->head = 0;
	}
	r->items[(r->head + r->count) % r->cap] = f;
	r->count++;
	r->bytes += f->alloc_size;
}

static struct mc_frame *ring_pop(struct mc_ring *r)
{
	if (!r->count)
		return NULL;
	struct mc_frame *f = r->items[r->head];
	r->head = (r->head + 1) % r->cap;
	r->count--;
	r->bytes -= f->alloc_size;
	return f;
}

static void ring_clear(struct mc_ring *r)
{
	struct mc_frame *f;
	while ((f = ring_pop(r)) != NULL)
		mc_frame_release(f);
}

/* первый логический индекс с ts >= t */
static size_t ring_lower_bound(const struct mc_ring *r, uint64_t t)
{
	size_t lo = 0, hi = r->count;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (ring_at(r, mid)->ts < t)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* ---------- фильтр ---------- */

struct mc_capture {
	obs_source_t *source;
	pthread_mutex_t mutex;
	char *group;
	int cam; /* 0..3 */
	uint64_t buffer_ns;
	size_t max_bytes;
	bool enabled;
	bool warned_format;
	bool logged_first;
	struct mc_ring ring;
	struct mc_capture *next;
};

static pthread_mutex_t g_caps_mutex;
static struct mc_capture *g_caps;

void mc_registry_init(void)
{
	pthread_mutex_init(&g_caps_mutex, NULL);
	g_caps = NULL;
}

void mc_registry_shutdown(void)
{
	pthread_mutex_destroy(&g_caps_mutex);
}

static void registry_add(struct mc_capture *c)
{
	pthread_mutex_lock(&g_caps_mutex);
	c->next = g_caps;
	g_caps = c;
	pthread_mutex_unlock(&g_caps_mutex);
}

static void registry_remove(struct mc_capture *c)
{
	pthread_mutex_lock(&g_caps_mutex);
	struct mc_capture **pp = &g_caps;
	while (*pp) {
		if (*pp == c) {
			*pp = c->next;
			break;
		}
		pp = &(*pp)->next;
	}
	pthread_mutex_unlock(&g_caps_mutex);
}

static const char *mc_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T("CaptureFilter");
}

static void mc_capture_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, S_GROUP, "main");
	obs_data_set_default_int(s, S_CAMERA, 1);
	obs_data_set_default_int(s, S_BUFFER, 10);
	obs_data_set_default_int(s, S_MAXMB, 2048);
	obs_data_set_default_bool(s, S_ENABLED, true);
}

static void mc_capture_update(void *data, obs_data_t *s)
{
	struct mc_capture *c = data;

	const char *g = obs_data_get_string(s, S_GROUP);
	int cam = (int)obs_data_get_int(s, S_CAMERA) - 1;
	int sec = (int)obs_data_get_int(s, S_BUFFER);
	int mb = (int)obs_data_get_int(s, S_MAXMB);
	const bool enabled = obs_data_get_bool(s, S_ENABLED);

	if (cam < 0)
		cam = 0;
	if (cam >= MC_MAX_CAMS)
		cam = MC_MAX_CAMS - 1;
	if (sec < 1)
		sec = 1;
	if (mb < 64)
		mb = 64;

	pthread_mutex_lock(&c->mutex);
	bfree(c->group);
	c->group = bstrdup(g && *g ? g : "main");
	c->cam = cam;
	c->buffer_ns = (uint64_t)sec * 1000000000ULL;
	c->max_bytes = (size_t)mb * 1024 * 1024;
	c->enabled = enabled;
	if (!enabled)
		ring_clear(&c->ring);
	pthread_mutex_unlock(&c->mutex);
}

static void *mc_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct mc_capture *c = bzalloc(sizeof(*c));
	c->source = source;
	pthread_mutex_init(&c->mutex, NULL);
	mc_capture_update(c, settings);
	registry_add(c);
	return c;
}

static void mc_capture_destroy(void *data)
{
	struct mc_capture *c = data;
	registry_remove(c);

	pthread_mutex_lock(&c->mutex);
	ring_clear(&c->ring);
	pthread_mutex_unlock(&c->mutex);

	bfree(c->ring.items);
	bfree(c->group);
	pthread_mutex_destroy(&c->mutex);
	bfree(c);
}

/* Вызывается потоком камеры для каждого кадра. Кадр возвращаем без изменений. */
static struct obs_source_frame *mc_capture_video(void *data, struct obs_source_frame *frame)
{
	struct mc_capture *c = data;

	if (!frame || !c->enabled)
		return frame;

	const uint64_t ts = os_gettime_ns();
	struct mc_frame *f = mc_frame_copy(frame, ts);
	if (!f) {
		if (!c->warned_format) {
			c->warned_format = true;
			blog(LOG_WARNING, "[multicam-replay] cam %d: unsupported frame format %d, frames are skipped",
			     c->cam + 1, (int)frame->format);
		}
		return frame;
	}

	pthread_mutex_lock(&c->mutex);
	ring_push(&c->ring, f);
	while (c->ring.count > 1 &&
	       (ts - ring_at(&c->ring, 0)->ts > c->buffer_ns || c->ring.bytes > c->max_bytes)) {
		mc_frame_release(ring_pop(&c->ring));
	}
	pthread_mutex_unlock(&c->mutex);

	if (!c->logged_first) {
		c->logged_first = true;
		blog(LOG_INFO, "[multicam-replay] cam %d: %ux%u, %.2f MB per frame, buffer %.0f s", c->cam + 1,
		     (unsigned)frame->width, (unsigned)frame->height, (double)f->alloc_size / (1024.0 * 1024.0),
		     (double)c->buffer_ns / 1e9);
	}
	return frame;
}

static obs_properties_t *mc_capture_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *p = obs_properties_create();

	obs_properties_add_text(p, S_GROUP, T("Group"), OBS_TEXT_DEFAULT);

	obs_property_t *cam = obs_properties_add_list(p, S_CAMERA, T("Camera"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 1; i <= MC_MAX_CAMS; i++) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%s %d", T("CameraN"), i);
		obs_property_list_add_int(cam, buf, i);
	}

	obs_property_t *sec = obs_properties_add_int_slider(p, S_BUFFER, T("BufferSeconds"), 2, 120, 1);
	obs_property_int_set_suffix(sec, " s");
	obs_property_t *mb = obs_properties_add_int(p, S_MAXMB, T("MaxMemory"), 128, 32768, 64);
	obs_property_int_set_suffix(mb, " MB");
	obs_properties_add_bool(p, S_ENABLED, T("Enabled"));
	obs_properties_add_text(p, "info", T("CaptureInfo"), OBS_TEXT_INFO);
	return p;
}

struct obs_source_info mc_capture_filter_info = {
	.id = "multicam_replay_capture",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.get_name = mc_capture_name,
	.create = mc_capture_create,
	.destroy = mc_capture_destroy,
	.update = mc_capture_update,
	.get_defaults = mc_capture_defaults,
	.get_properties = mc_capture_properties,
	.filter_video = mc_capture_video,
};

/* ---------- снимок группы камер ---------- */

void mc_clip_free(struct mc_clip *clip)
{
	for (int cam = 0; cam < MC_MAX_CAMS; cam++) {
		for (size_t i = 0; i < clip->count[cam]; i++)
			mc_frame_release(clip->frames[cam][i]);
		bfree(clip->frames[cam]);
	}
	memset(clip, 0, sizeof(*clip));
}

/* Берёт по ссылке кадры [t_end - length, t_end] со всех камер группы.
 * Данные не копируются, поэтому «Save replay» срабатывает мгновенно. */
bool mc_snapshot_group(const char *group, uint64_t t_end, uint64_t length_ns, struct mc_clip *out)
{
	memset(out, 0, sizeof(*out));
	const uint64_t t_start = t_end > length_ns ? t_end - length_ns : 0;
	uint64_t t0 = UINT64_MAX, t1 = 0;
	bool any = false;

	pthread_mutex_lock(&g_caps_mutex);
	for (struct mc_capture *c = g_caps; c; c = c->next) {
		pthread_mutex_lock(&c->mutex);
		if (c->cam >= 0 && c->cam < MC_MAX_CAMS && !out->frames[c->cam] && strcmp(c->group, group) == 0) {
			const size_t i0 = ring_lower_bound(&c->ring, t_start);
			const size_t i1 = ring_lower_bound(&c->ring, t_end + 1);
			if (i1 > i0) {
				const size_t n = i1 - i0;
				struct mc_frame **arr = bmalloc(sizeof(*arr) * n);
				for (size_t i = 0; i < n; i++) {
					arr[i] = ring_at(&c->ring, i0 + i);
					mc_frame_addref(arr[i]);
				}
				out->frames[c->cam] = arr;
				out->count[c->cam] = n;
				if (arr[0]->ts < t0)
					t0 = arr[0]->ts;
				if (arr[n - 1]->ts > t1)
					t1 = arr[n - 1]->ts;
				any = true;
			}
		}
		pthread_mutex_unlock(&c->mutex);
	}
	pthread_mutex_unlock(&g_caps_mutex);

	if (!any)
		return false;
	out->t0 = t0;
	out->t1 = t1;
	return true;
}
