/* Мост между ядром плагина (C) и окном управления (C++/Qt). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mc_replay_status {
	int clips;       /* сколько клипов хранится */
	int cur;         /* номер текущего клипа (с 1), 0 если клипов нет */
	int cam;         /* активная камера 1..4 */
	double speed;    /* проценты */
	bool playing;
	bool backward;
	bool hidden;
	bool post_pending; /* идёт пост-ролл, клип ещё не сохранён */
	bool seq;          /* идёт проигрывание камер подряд */
	int seq_pos;       /* какая по счёту камера в последовательности */
	int seq_total;     /* сколько камер в последовательности */
	bool return_pending; /* ждём возврата на сцену */
	double pos_sec;
	double dur_sec;
	double mem_mb; /* память, занятая кадрами всех камер */
};

typedef void (*mc_name_cb)(const char *name, void *param);

void mc_replay_registry_init(void);
void mc_replay_registry_shutdown(void);
void mc_replay_enum_names(mc_name_cb cb, void *param);

/* action - имя хоткея: "mc_save", "mc_cam1", "mc_play_pause", ... */
bool mc_replay_trigger(const char *source_name, const char *action);
bool mc_replay_set_speed(const char *source_name, double percent);
bool mc_replay_get_status(const char *source_name, struct mc_replay_status *out);

/* mc-control.cpp */
void mc_control_init(void);
void mc_control_shutdown(void);

#ifdef __cplusplus
}
#endif
