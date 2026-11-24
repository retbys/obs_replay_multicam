/*
 * Multicam Replay for OBS Studio
 * Общие типы: кадр в пуле памяти, клип, реестр камер.
 */
#pragma once

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#define MC_MAX_CAMS 4
#define MC_MAX_PLANES 4

/* Один сохранённый кадр. Лежит в одном блоке памяти: [заголовок][плоскости].
 * Копия приходит «как есть» из асинхронного источника (NV12/YUY2/...), поэтому
 * ни GPU, ни конвертация цвета не используются. */
struct mc_frame {
	volatile long refs;
	struct mc_frame *next_free; /* для пула */
	size_t alloc_size;
	uint64_t ts; /* os_gettime_ns() в момент прихода кадра - общая шкала для всех камер */
	struct obs_source_frame meta; /* формат, размеры, цветовая матрица; data[] указывают в наш блок */
};

/* Клип: снимок последних N секунд со всех камер группы. Кадры не копируются, а
 * берутся по ссылке (refcount) из кольцевых буферов. */
struct mc_clip {
	struct mc_frame **frames[MC_MAX_CAMS];
	size_t count[MC_MAX_CAMS];
	uint64_t t0, t1; /* абсолютные метки времени начала и конца клипа */
};

/* --- mc-frame.c: пул памяти --- */
void mc_pool_init(void);
void mc_pool_shutdown(void);
struct mc_frame *mc_frame_copy(const struct obs_source_frame *src, uint64_t ts);
void mc_frame_addref(struct mc_frame *f);
void mc_frame_release(struct mc_frame *f);
size_t mc_mem_in_use(void);

/* --- mc-capture.c: реестр камер и снимки --- */
void mc_registry_init(void);
void mc_registry_shutdown(void);
bool mc_snapshot_group(const char *group, uint64_t t_end, uint64_t length_ns, struct mc_clip *out);
void mc_clip_free(struct mc_clip *clip);

extern struct obs_source_info mc_capture_filter_info;
extern struct obs_source_info mc_replay_source_info;
