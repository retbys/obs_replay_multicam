#include "mc-common.h"
#include <string.h>

#define HEADER_SIZE ((sizeof(struct mc_frame) + 63) & ~(size_t)63)
#define ALIGN64(x) (((x) + 63) & ~(size_t)63)
#define POOL_MAX_FREE_BYTES ((size_t)512 * 1024 * 1024)

static pthread_mutex_t g_pool_mutex;
static struct mc_frame *g_free_list;
static size_t g_free_bytes;
static size_t g_in_use_bytes;

void mc_pool_init(void)
{
	pthread_mutex_init(&g_pool_mutex, NULL);
	g_free_list = NULL;
	g_free_bytes = 0;
	g_in_use_bytes = 0;
}

void mc_pool_shutdown(void)
{
	pthread_mutex_lock(&g_pool_mutex);
	struct mc_frame *f = g_free_list;
	while (f) {
		struct mc_frame *next = f->next_free;
		bfree(f);
		f = next;
	}
	g_free_list = NULL;
	g_free_bytes = 0;
	pthread_mutex_unlock(&g_pool_mutex);
	pthread_mutex_destroy(&g_pool_mutex);
}

size_t mc_mem_in_use(void)
{
	pthread_mutex_lock(&g_pool_mutex);
	size_t v = g_in_use_bytes;
	pthread_mutex_unlock(&g_pool_mutex);
	return v;
}

/* Высота каждой плоскости для поддерживаемых форматов. Размер плоскости =
 * linesize[i] * высота, поэтому bpp считать не нужно. */
static bool plane_heights(enum video_format fmt, uint32_t h, int *np, uint32_t out[MC_MAX_PLANES])
{
	const uint32_t h2 = (h + 1) / 2;

	switch (fmt) {
	case VIDEO_FORMAT_NV12:
		*np = 2;
		out[0] = h;
		out[1] = h2;
		return true;
	case VIDEO_FORMAT_I420:
		*np = 3;
		out[0] = h;
		out[1] = h2;
		out[2] = h2;
		return true;
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I444:
		*np = 3;
		out[0] = out[1] = out[2] = h;
		return true;
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_Y800:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGR3:
		*np = 1;
		out[0] = h;
		return true;
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 0, 0)
	case VIDEO_FORMAT_P010:
		*np = 2;
		out[0] = h;
		out[1] = h2;
		return true;
	case VIDEO_FORMAT_I010:
		*np = 3;
		out[0] = h;
		out[1] = h2;
		out[2] = h2;
		return true;
	case VIDEO_FORMAT_I210:
		*np = 3;
		out[0] = out[1] = out[2] = h;
		return true;
#endif
	default:
		return false;
	}
}

static struct mc_frame *pool_get(size_t total)
{
	struct mc_frame *f = NULL;

	pthread_mutex_lock(&g_pool_mutex);
	struct mc_frame **pp = &g_free_list;
	while (*pp) {
		if ((*pp)->alloc_size == total) {
			f = *pp;
			*pp = f->next_free;
			g_free_bytes -= total;
			break;
		}
		pp = &(*pp)->next_free;
	}
	g_in_use_bytes += total;
	pthread_mutex_unlock(&g_pool_mutex);

	if (f)
		return f;

	f = bmalloc(total);
	if (!f) {
		pthread_mutex_lock(&g_pool_mutex);
		g_in_use_bytes -= total;
		pthread_mutex_unlock(&g_pool_mutex);
		return NULL;
	}
	f->alloc_size = total;
	return f;
}

static void pool_put(struct mc_frame *f)
{
	struct mc_frame *evict = NULL;
	bool keep = false;

	pthread_mutex_lock(&g_pool_mutex);
	g_in_use_bytes -= f->alloc_size;

	if (g_free_bytes + f->alloc_size > POOL_MAX_FREE_BYTES) {
		/* Пул полон: выбрасываем блоки другого размера (остались от старого разрешения). */
		struct mc_frame **pp = &g_free_list;
		while (*pp) {
			struct mc_frame *cur = *pp;
			if (cur->alloc_size != f->alloc_size) {
				*pp = cur->next_free;
				g_free_bytes -= cur->alloc_size;
				cur->next_free = evict;
				evict = cur;
			} else {
				pp = &cur->next_free;
			}
		}
	}
	if (g_free_bytes + f->alloc_size <= POOL_MAX_FREE_BYTES) {
		f->next_free = g_free_list;
		g_free_list = f;
		g_free_bytes += f->alloc_size;
		keep = true;
	}
	pthread_mutex_unlock(&g_pool_mutex);

	while (evict) {
		struct mc_frame *next = evict->next_free;
		bfree(evict);
		evict = next;
	}
	if (!keep)
		bfree(f);
}

struct mc_frame *mc_frame_copy(const struct obs_source_frame *src, uint64_t ts)
{
	uint32_t ph[MC_MAX_PLANES] = {0};
	size_t offs[MC_MAX_PLANES] = {0};
	size_t sizes[MC_MAX_PLANES] = {0};
	int np = 0;

	if (!src || !src->width || !src->height)
		return NULL;
	if (!plane_heights(src->format, src->height, &np, ph))
		return NULL;

	size_t total = HEADER_SIZE;
	for (int i = 0; i < np; i++) {
		if (!src->data[i] || !src->linesize[i])
			return NULL;
		sizes[i] = (size_t)src->linesize[i] * ph[i];
		offs[i] = total;
		total += ALIGN64(sizes[i]);
	}

	struct mc_frame *f = pool_get(total);
	if (!f)
		return NULL;

	f->refs = 1;
	f->next_free = NULL;
	f->ts = ts;
	memcpy(&f->meta, src, sizeof(*src));
	for (size_t i = 0; i < sizeof(f->meta.data) / sizeof(f->meta.data[0]); i++)
		f->meta.data[i] = NULL;
	for (int i = 0; i < np; i++) {
		uint8_t *dst = (uint8_t *)f + offs[i];
		memcpy(dst, src->data[i], sizes[i]);
		f->meta.data[i] = dst;
	}
	return f;
}

void mc_frame_addref(struct mc_frame *f)
{
	if (f)
		os_atomic_inc_long(&f->refs);
}

void mc_frame_release(struct mc_frame *f)
{
	if (!f)
		return;
	if (os_atomic_dec_long(&f->refs) > 0)
		return;
	pool_put(f);
}
