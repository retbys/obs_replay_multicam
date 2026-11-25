/* Тест логики плагина на заглушках API: 4 камеры, сохранение, воспроизведение,
 * смена камеры, скорость, шаг, реверс, пул памяти и отсутствие утечек. */
#include <obs-module.h>
#include <stdarg.h>
#include <assert.h>
#include "mc-common.h"
#include "mc-api.h"

/* ---------- заглушки ---------- */
static uint64_t g_now;
static long g_bmalloc_calls;

void *bmalloc(size_t n) { g_bmalloc_calls++; return malloc(n); }
void bfree(void *p) { free(p); }
void *bzalloc(size_t n) { return calloc(1, n); }
char *bstrdup(const char *s) { return strdup(s); }
void blog(int l, const char *fmt, ...) { (void)l; va_list a; va_start(a, fmt); printf("   [log] "); vprintf(fmt, a); printf("\n"); va_end(a); }
uint64_t os_gettime_ns(void) { return g_now; }
long os_atomic_inc_long(volatile long *v) { return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST); }
long os_atomic_dec_long(volatile long *v) { return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST); }
const char *obs_module_text(const char *s) { return s; }

struct kv { char key[32]; double num; char str[64]; };
struct obs_data { struct kv kv[32]; int n; };
static struct kv *find(obs_data_t *d, const char *k, bool create)
{
	for (int i = 0; i < d->n; i++) if (!strcmp(d->kv[i].key, k)) return &d->kv[i];
	if (!create) return NULL;
	struct kv *e = &d->kv[d->n++]; memset(e, 0, sizeof(*e)); snprintf(e->key, 32, "%s", k); return e;
}
void obs_data_set_default_string(obs_data_t *d, const char *k, const char *v) { if (!find(d, k, false)) snprintf(find(d, k, true)->str, 64, "%s", v); }
void obs_data_set_default_int(obs_data_t *d, const char *k, long long v) { if (!find(d, k, false)) find(d, k, true)->num = (double)v; }
void obs_data_set_default_double(obs_data_t *d, const char *k, double v) { if (!find(d, k, false)) find(d, k, true)->num = v; }
void obs_data_set_default_bool(obs_data_t *d, const char *k, bool v) { if (!find(d, k, false)) find(d, k, true)->num = v; }
const char *obs_data_get_string(obs_data_t *d, const char *k) { struct kv *e = find(d, k, false); return e ? e->str : ""; }
long long obs_data_get_int(obs_data_t *d, const char *k) { struct kv *e = find(d, k, false); return e ? (long long)e->num : 0; }
double obs_data_get_double(obs_data_t *d, const char *k) { struct kv *e = find(d, k, false); return e ? e->num : 0; }
bool obs_data_get_bool(obs_data_t *d, const char *k) { struct kv *e = find(d, k, false); return e && e->num != 0; }
void obs_data_set_int(obs_data_t *d, const char *k, long long v) { find(d, k, true)->num = (double)v; }
void obs_data_set_double(obs_data_t *d, const char *k, double v) { find(d, k, true)->num = v; }
void obs_data_set_bool(obs_data_t *d, const char *k, bool v) { find(d, k, true)->num = v; }
void obs_data_set_string(obs_data_t *d, const char *k, const char *v) { snprintf(find(d, k, true)->str, 64, "%s", v); }
void obs_data_release(obs_data_t *d) { (void)d; }

struct obs_source { obs_data_t *settings; const char *name; };
const char *obs_source_get_name(const obs_source_t *s) { return s->name; }

/* заглушки возврата на сцену */
static char g_scene_switched[64];
static int g_scene_switches;
char **obs_frontend_get_scene_names(void) { return NULL; }
obs_source_t *obs_get_source_by_name(const char *n) { return (obs_source_t *)(void *)strdup(n); }
void obs_source_release(obs_source_t *s) { free(s); }
void obs_frontend_set_current_scene(obs_source_t *s) { snprintf(g_scene_switched, sizeof(g_scene_switched), "%s", (const char *)(void *)s); g_scene_switches++; }
void obs_queue_task(enum obs_task_type t, obs_task_t fn, void *p, bool wait) { (void)t; (void)wait; fn(p); }
size_t obs_property_list_add_string(obs_property_t *p, const char *a, const char *b) { (void)p;(void)a;(void)b; return 0; }
obs_data_t *obs_source_get_settings(const obs_source_t *s) { return s->settings; }

static struct { bool blank; int cam; uint32_t frameno; long count; } g_out;
void obs_source_output_video(obs_source_t *s, const struct obs_source_frame *f)
{
	(void)s;
	g_out.count++;
	if (!f) { g_out.blank = true; return; }
	g_out.blank = false;
	g_out.cam = f->data[0][0];
	memcpy(&g_out.frameno, f->data[0] + 4, 4);
	assert(f->width == 640 && f->height == 360 && f->format == VIDEO_FORMAT_NV12);
	assert(f->data[1] && f->linesize[1] == 640);
}
void obs_source_set_async_unbuffered(obs_source_t *s, bool b) { (void)s; (void)b; }
void obs_register_source(struct obs_source_info *i) { (void)i; }

obs_properties_t *obs_properties_create(void) { return calloc(1, 8); }
obs_property_t *obs_properties_add_text(obs_properties_t *p, const char *a, const char *b, int c) { (void)p;(void)a;(void)b;(void)c; return NULL; }
obs_property_t *obs_properties_add_list(obs_properties_t *p, const char *a, const char *b, int c, int d) { (void)p;(void)a;(void)b;(void)c;(void)d; return NULL; }
size_t obs_property_list_add_int(obs_property_t *p, const char *a, long long b) { (void)p;(void)a;(void)b; return 0; }
obs_property_t *obs_properties_add_int_slider(obs_properties_t *p, const char *a, const char *b, int c, int d, int e) { (void)p;(void)a;(void)b;(void)c;(void)d;(void)e; return NULL; }
obs_property_t *obs_properties_add_int(obs_properties_t *p, const char *a, const char *b, int c, int d, int e) { (void)p;(void)a;(void)b;(void)c;(void)d;(void)e; return NULL; }
obs_property_t *obs_properties_add_float_slider(obs_properties_t *p, const char *a, const char *b, double c, double d, double e) { (void)p;(void)a;(void)b;(void)c;(void)d;(void)e; return NULL; }
obs_property_t *obs_properties_add_bool(obs_properties_t *p, const char *a, const char *b) { (void)p;(void)a;(void)b; return NULL; }
obs_property_t *obs_properties_add_button2(obs_properties_t *p, const char *a, const char *b, obs_property_clicked_t c, void *d) { (void)p;(void)a;(void)b;(void)c;(void)d; return NULL; }
void obs_property_int_set_suffix(obs_property_t *p, const char *s) { (void)p;(void)s; }
void obs_property_float_set_suffix(obs_property_t *p, const char *s) { (void)p;(void)s; }

struct hk { char name[32]; void (*cb)(void *, obs_hotkey_id, obs_hotkey_t *, bool); void *data; bool live; };
static struct hk g_hk[64];
static int g_nhk;
obs_hotkey_id obs_hotkey_register_source(obs_source_t *s, const char *name, const char *desc,
					 void (*cb)(void *, obs_hotkey_id, obs_hotkey_t *, bool), void *data)
{
	(void)s; (void)desc;
	snprintf(g_hk[g_nhk].name, 32, "%s", name); g_hk[g_nhk].cb = cb; g_hk[g_nhk].data = data; g_hk[g_nhk].live = true;
	return (obs_hotkey_id)g_nhk++;
}
void obs_hotkey_unregister(obs_hotkey_id id) { g_hk[id].live = false; }
static void press(const char *name)
{
	for (int i = 0; i < g_nhk; i++)
		if (g_hk[i].live && !strcmp(g_hk[i].name, name)) { g_hk[i].cb(g_hk[i].data, (obs_hotkey_id)i, NULL, true); return; }
	printf("FAIL: hotkey %s not found\n", name); exit(1);
}

/* ---------- тест ---------- */
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); exit(1); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W 640
#define H 360
#define FRAME_NS 33333333ULL

static int g_names_seen;
static void name_cb(const char *n, void *p) { if (!strcmp(n, "Replay")) (*(int *)p)++; }

static void feed(void *filter, int cam, uint32_t no)
{
	static uint8_t y[W * H], uv[W * H / 2];
	y[0] = (uint8_t)cam; memcpy(y + 4, &no, 4);
	struct obs_source_frame f = {0};
	f.data[0] = y; f.data[1] = uv; f.linesize[0] = W; f.linesize[1] = W;
	f.width = W; f.height = H; f.format = VIDEO_FORMAT_NV12;
	struct obs_source_frame *r = mc_capture_filter_info.filter_video(filter, &f);
	assert(r == &f);
}

static void tick_n(void *replay, int n, float dt)
{
	for (int i = 0; i < n; i++) { g_now += (uint64_t)(dt * 1e9); mc_replay_source_info.video_tick(replay, dt); }
}

int main(void)
{
	mc_pool_init();
	mc_registry_init();
	mc_replay_registry_init();

	struct obs_source fsrc[4]; void *filt[4];
	for (int c = 0; c < 4; c++) {
		fsrc[c].settings = calloc(1, sizeof(struct obs_data));
		mc_capture_filter_info.get_defaults(fsrc[c].settings);
		obs_data_set_int(fsrc[c].settings, "camera", c + 1);
		obs_data_set_int(fsrc[c].settings, "buffer_seconds", 5);
		filt[c] = mc_capture_filter_info.create(fsrc[c].settings, &fsrc[c]);
	}

	/* 12 с при 30 fps на 4 камеры */
	for (uint32_t i = 0; i < 360; i++) {
		g_now = 1000000000ULL + i * FRAME_NS;
		for (int c = 0; c < 4; c++) feed(filt[c], c, i);
	}
	CHECK(g_bmalloc_calls < 4 * 360 / 2, "pool reuses blocks: %ld mallocs for %d frames", g_bmalloc_calls, 4 * 360);
	printf("     in use: %.1f MB (4 cams x 5 s ring)\n", mc_mem_in_use() / 1048576.0);
	CHECK(mc_mem_in_use() < 4 * 160 * (size_t)(W * H * 3 / 2 + 4096), "ring trimmed to ~5 s per camera");

	struct obs_source rsrc; rsrc.name = "Replay"; rsrc.settings = calloc(1, sizeof(struct obs_data));
	mc_replay_source_info.get_defaults(rsrc.settings);
	obs_data_set_int(rsrc.settings, "clip_seconds", 3);
	void *rep = mc_replay_source_info.create(rsrc.settings, &rsrc);

	/* --- сохранение и воспроизведение камеры 1 в реальном времени --- */
	g_now = 1000000000ULL + 359 * FRAME_NS;
	press("mc_save");
	tick_n(rep, 1, 1.0f / 30);
	CHECK(!g_out.blank && g_out.cam == 0, "first output is camera 1");
	CHECK(g_out.frameno >= 268 && g_out.frameno <= 272, "clip starts ~3 s before save (frame %u, expected ~270)", g_out.frameno);
	uint32_t prev = g_out.frameno; int monotonic = 1; long n0 = g_out.count;
	tick_n(rep, 120, 1.0f / 30);
	CHECK(g_out.frameno >= 357, "reaches the end of the clip, last frame %u", g_out.frameno);
	CHECK(g_out.count - n0 < 100, "hold at end: outputs stop being sent (%ld sends)", g_out.count - n0);
	(void)prev; (void)monotonic;

	/* --- 50% скорость + переключение камеры на лету --- */
	press("mc_speed_50");
	press("mc_restart");
	tick_n(rep, 1, 1.0f / 30);
	long n1 = g_out.count;
	tick_n(rep, 60, 1.0f / 30); /* 2 с реального = 1 с клипа */
	uint32_t at_mid = g_out.frameno;
	CHECK(at_mid >= 296 && at_mid <= 304, "50%% speed: 2 s real -> ~1 s clip (frame %u, expected ~300)", at_mid);
	CHECK(g_out.count - n1 <= 32, "slow-mo sends each frame once, not every tick (%ld sends in 60 ticks)", g_out.count - n1);
	press("mc_cam3");
	tick_n(rep, 1, 1.0f / 30);
	CHECK(g_out.cam == 2, "camera switch -> camera 3");
	CHECK(g_out.frameno >= at_mid - 1 && g_out.frameno <= at_mid + 2, "same time position after camera switch (frame %u vs %u)", g_out.frameno, at_mid);

	/* --- шаг по кадрам --- */
	press("mc_play_pause");
	tick_n(rep, 1, 1.0f / 30);
	uint32_t paused_at = g_out.frameno;
	tick_n(rep, 10, 1.0f / 30);
	CHECK(g_out.frameno == paused_at, "pause freezes the frame");
	press("mc_step_back"); tick_n(rep, 1, 1.0f / 30);
	CHECK(g_out.frameno == paused_at - 1, "step back = exactly one frame (%u -> %u)", paused_at, g_out.frameno);
	press("mc_step_fwd"); press("mc_step_fwd"); tick_n(rep, 1, 1.0f / 30);
	CHECK(g_out.frameno == paused_at + 1, "step forward x2 (%u)", g_out.frameno);

	/* --- реверс --- */
	press("mc_speed_100");
	press("mc_reverse");
	tick_n(rep, 15, 1.0f / 30);
	CHECK(g_out.frameno < paused_at, "reverse plays backwards (frame %u)", g_out.frameno);

	/* --- второй клип, навигация, лимит клипов --- */
	g_now += 2000000000ULL;
	for (uint32_t i = 360; i < 420; i++) { g_now = 1000000000ULL + i * FRAME_NS; for (int c = 0; c < 4; c++) feed(filt[c], c, i); }
	press("mc_save");
	tick_n(rep, 1, 1.0f / 30);
	CHECK(g_out.frameno >= 328, "second clip is newer (frame %u)", g_out.frameno);
	press("mc_prev_clip");
	tick_n(rep, 1, 1.0f / 30);
	CHECK(g_out.frameno >= 268 && g_out.frameno <= 358, "previous clip selectable (frame %u)", g_out.frameno);
	for (int i = 0; i < 10; i++) { g_now += 100000000ULL; press("mc_save"); }
	size_t used = mc_mem_in_use();
	printf("     in use with 4-clip limit: %.1f MB\n", used / 1048576.0);

	/* --- пост-ролл --- */
	obs_data_set_int(rsrc.settings, "post_ms", 1000);
	mc_replay_source_info.update(rep, rsrc.settings);
	press("mc_clear");
	press("mc_save");
	long before = g_out.count;
	tick_n(rep, 5, 1.0f / 30);
	CHECK(g_out.count == before || g_out.blank, "post-roll: nothing played before the delay expires");
	for (uint32_t i = 420; i < 460; i++) { g_now += FRAME_NS; for (int c = 0; c < 4; c++) feed(filt[c], c, i); }
	tick_n(rep, 3, 1.0f / 30);
	CHECK(!g_out.blank, "post-roll: clip appears after the delay");

	/* --- API окна управления --- */
	{
		struct mc_replay_status st;
		mc_replay_enum_names(name_cb, &g_names_seen);
		CHECK(g_names_seen == 1, "enum names finds the registered source");
		CHECK(mc_replay_trigger("Replay", "mc_clear") && mc_replay_get_status("Replay", &st) && st.clips == 0, "API trigger: clear -> 0 clips");
		obs_data_set_int(rsrc.settings, "post_ms", 0);
		mc_replay_source_info.update(rep, rsrc.settings);
		g_now += 100000000ULL;
		CHECK(mc_replay_trigger("Replay", "mc_save"), "API trigger: save accepted");
		tick_n(rep, 1, 1.0f / 30);
		CHECK(mc_replay_get_status("Replay", &st) && st.clips == 1 && st.cur == 1 && st.playing, "status: 1 clip, playing");
		CHECK(st.dur_sec > 2.5 && st.dur_sec < 3.2, "status: clip duration %.2f s", st.dur_sec);
		CHECK(mc_replay_trigger("Replay", "mc_cam2") && mc_replay_get_status("Replay", &st) && st.cam == 2, "status: camera 2");
		CHECK(mc_replay_set_speed("Replay", 50.0) && mc_replay_get_status("Replay", &st) && st.speed == 50.0, "status: speed 50");
		CHECK(mc_replay_set_speed("Replay", 9999.0) && mc_replay_get_status("Replay", &st) && st.speed == 400.0, "speed is clamped to 400");
		CHECK(!mc_replay_trigger("Replay", "no_such_action"), "unknown action rejected");
		CHECK(!mc_replay_trigger("Nope", "mc_save"), "unknown source rejected");
		CHECK(!mc_replay_get_status("Nope", &st), "status of unknown source fails");
	}
	/* --- камеры подряд --- */
	{
		struct mc_replay_status st;
		mc_replay_trigger("Replay", "mc_clear");
		mc_replay_trigger("Replay", "mc_speed_100");
		mc_replay_set_speed("Replay", 400.0); /* быстро, чтобы тест был коротким */
		obs_data_set_int(rsrc.settings, "post_ms", 0);
		obs_data_set_int(rsrc.settings, "clip_seconds", 2);
		obs_data_set_bool(rsrc.settings, "seq_cam3", false); /* камера 3 исключена */
		mc_replay_source_info.update(rep, rsrc.settings);
		for (uint32_t i = 460; i < 560; i++) { g_now += FRAME_NS; for (int c = 0; c < 4; c++) feed(filt[c], c, i); }
		mc_replay_trigger("Replay", "mc_reverse"); /* убедимся, что направление не ломает порядок */
		mc_replay_trigger("Replay", "mc_reverse");
		mc_replay_trigger("Replay", "mc_save");
		tick_n(rep, 1, 1.0f / 30);
		CHECK(mc_replay_trigger("Replay", "mc_seq_play"), "seq play accepted");
		tick_n(rep, 1, 1.0f / 30);
		CHECK(mc_replay_get_status("Replay", &st) && st.seq && st.seq_total == 3 && st.seq_pos == 1 && g_out.cam == 0,
		      "sequence starts on camera 1 (%d of %d)", st.seq_pos, st.seq_total);
		int order[8], norder = 0, last_cam = -1;
		for (int i = 0; i < 400 && norder < 8; i++) {
			tick_n(rep, 1, 1.0f / 30);
			if (!g_out.blank && g_out.cam != last_cam) { last_cam = g_out.cam; order[norder++] = g_out.cam; }
			mc_replay_get_status("Replay", &st);
			if (!st.seq && !st.playing) break;
		}
		CHECK(norder == 3 && order[0] == 0 && order[1] == 1 && order[2] == 3, "camera order 1,2,4 (camera 3 excluded): got %d cams", norder);
		CHECK(!st.seq && !st.playing && st.cam == 4, "sequence finished and holds last camera");
		obs_data_set_bool(rsrc.settings, "seq_cam3", true);
		mc_replay_source_info.update(rep, rsrc.settings);
		mc_replay_trigger("Replay", "mc_seq_play");
		mc_replay_trigger("Replay", "mc_cam2"); /* ручной выбор камеры отменяет последовательность */
		CHECK(mc_replay_get_status("Replay", &st) && !st.seq, "manual camera switch cancels the sequence");
	}

	/* --- возврат на сцену --- */
	{
		struct mc_replay_status st;
		obs_data_set_bool(rsrc.settings, "return_enabled", true);
		obs_data_set_string(rsrc.settings, "return_scene", "Game");
		obs_data_set_int(rsrc.settings, "return_delay_ms", 500);
		mc_replay_source_info.update(rep, rsrc.settings);
		g_scene_switches = 0; g_scene_switched[0] = 0;
		mc_replay_trigger("Replay", "mc_restart");
		for (int i = 0; i < 200; i++) { tick_n(rep, 1, 1.0f / 30); mc_replay_get_status("Replay", &st); if (!st.playing) break; }
		CHECK(!st.playing && st.return_pending && g_scene_switches == 0, "clip ended: return is scheduled, not yet executed");
		tick_n(rep, 10, 1.0f / 30); /* 0.33 s < 0.5 s */
		CHECK(g_scene_switches == 0, "no switch before the delay expires");
		tick_n(rep, 10, 1.0f / 30);
		CHECK(g_scene_switches == 1 && !strcmp(g_scene_switched, "Game"), "switched to scene 'Game' (%d switches)", g_scene_switches);
		tick_n(rep, 30, 1.0f / 30);
		CHECK(g_scene_switches == 1, "switch happens only once");

		/* возврат после полной последовательности камер, а не после каждой камеры */
		g_scene_switches = 0;
		mc_replay_trigger("Replay", "mc_seq_play");
		int ticks = 0;
		for (; ticks < 600; ticks++) { tick_n(rep, 1, 1.0f / 30); mc_replay_get_status("Replay", &st); if (!st.playing && !st.seq) break; }
		CHECK(g_scene_switches == 0 && st.return_pending, "no scene switch between cameras of a sequence");
		tick_n(rep, 30, 1.0f / 30);
		CHECK(g_scene_switches == 1, "one switch after the whole sequence");

		/* ручная остановка отменяет возврат */
		g_scene_switches = 0;
		mc_replay_trigger("Replay", "mc_restart");
		for (int i = 0; i < 200; i++) { tick_n(rep, 1, 1.0f / 30); mc_replay_get_status("Replay", &st); if (!st.playing) break; }
		mc_replay_trigger("Replay", "mc_hide");
		tick_n(rep, 60, 1.0f / 30);
		CHECK(g_scene_switches == 0, "Hide cancels the pending return");

		/* пустое имя сцены: ничего не делаем */
		obs_data_set_string(rsrc.settings, "return_scene", "");
		mc_replay_source_info.update(rep, rsrc.settings);
		mc_replay_trigger("Replay", "mc_restart");
		for (int i = 0; i < 200; i++) { tick_n(rep, 1, 1.0f / 30); mc_replay_get_status("Replay", &st); if (!st.playing) break; }
		tick_n(rep, 60, 1.0f / 30);
		CHECK(g_scene_switches == 0, "empty scene name = no return");
	}

	/* --- очистка, отсутствие утечек --- */
	press("mc_hide");
	tick_n(rep, 1, 1.0f / 30);
	CHECK(g_out.blank, "hide outputs blank");
	mc_replay_source_info.destroy(rep);
	for (int c = 0; c < 4; c++) mc_capture_filter_info.destroy(filt[c]);
	CHECK(mc_mem_in_use() == 0, "no leaked frames after teardown (%zu bytes in use)", mc_mem_in_use());
	mc_registry_shutdown();
	mc_pool_shutdown();
	printf("ALL TESTS PASSED\n");
	return 0;
}
