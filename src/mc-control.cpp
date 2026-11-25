/*
 * Окно управления Multicam Replay (Qt). Открывается из меню OBS:
 * Инструменты -> Multicam Replay: управление.
 * Окно ничего не хранит само: все команды идут в ядро плагина через mc-api.h,
 * а состояние читается по таймеру 5 раз в секунду.
 */
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "mc-api.h"

static inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static void collect_names(const char *name, void *param)
{
	static_cast<QStringList *>(param)->append(QString::fromUtf8(name));
}

class MCControl : public QWidget {
public:
	explicit MCControl(QWidget *parent) : QWidget(parent, Qt::Window)
	{
		setWindowTitle(T("ControlTitle"));
		resize(540, 760);

		QVBoxLayout *root = new QVBoxLayout(this);

		/* источник повтора */
		QHBoxLayout *srcRow = new QHBoxLayout();
		srcRow->addWidget(new QLabel(T("ControlSource"), this));
		src = new QComboBox(this);
		src->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		srcRow->addWidget(src);
		root->addLayout(srcRow);

		/* сохранить */
		QPushButton *save = new QPushButton(T("ControlSave"), this);
		save->setMinimumHeight(64);
		save->setStyleSheet("QPushButton{background:#2e7d32;color:white;font-weight:bold;font-size:18px;}"
				    "QPushButton:pressed{background:#1b5e20;}");
		connect(save, &QPushButton::clicked, this, [this]() { trig("mc_save"); });
		root->addWidget(save);

		/* камеры */
		QGroupBox *camBox = new QGroupBox(T("ControlCameras"), this);
		QHBoxLayout *camRow = new QHBoxLayout(camBox);
		static const char *cam_actions[4] = {"mc_cam1", "mc_cam2", "mc_cam3", "mc_cam4"};
		for (int i = 0; i < 4; i++) {
			cam[i] = new QPushButton(QString::number(i + 1), this);
			cam[i]->setCheckable(true);
			cam[i]->setMinimumHeight(52);
			cam[i]->setStyleSheet("QPushButton{font-size:20px;font-weight:bold;}"
					      "QPushButton:checked{background:#c62828;color:white;}");
			const char *action = cam_actions[i];
			connect(cam[i], &QPushButton::clicked, this, [this, action]() { trig(action); });
			camRow->addWidget(cam[i]);
		}
		root->addWidget(camBox);

		/* воспроизведение */
		QGroupBox *playBox = new QGroupBox(T("ControlPlayback"), this);
		QGridLayout *pg = new QGridLayout(playBox);
		addBtn(pg, 0, 0, T("BtnPrev"), "mc_prev_clip");
		addBtn(pg, 0, 1, T("BtnStepBack"), "mc_step_back");
		addBtn(pg, 0, 2, T("BtnPlayPause"), "mc_play_pause");
		addBtn(pg, 0, 3, T("BtnStepFwd"), "mc_step_fwd");
		addBtn(pg, 0, 4, T("BtnNext"), "mc_next_clip");
		addBtn(pg, 1, 0, T("BtnLast"), "mc_last_clip");
		addBtn(pg, 1, 1, T("BtnRestart"), "mc_restart");
		addBtn(pg, 1, 2, T("BtnReverse"), "mc_reverse");
		addBtn(pg, 1, 3, T("BtnHide"), "mc_hide");
		addBtn(pg, 1, 4, T("BtnClear"), "mc_clear");
		root->addWidget(playBox);

		/* скорость */
		QGroupBox *spBox = new QGroupBox(T("ControlSpeed"), this);
		QHBoxLayout *sp = new QHBoxLayout(spBox);
		QPushButton *b25 = new QPushButton("25%", this);
		QPushButton *b50 = new QPushButton("50%", this);
		QPushButton *b100 = new QPushButton("100%", this);
		connect(b25, &QPushButton::clicked, this, [this]() { setSpeed(25); });
		connect(b50, &QPushButton::clicked, this, [this]() { setSpeed(50); });
		connect(b100, &QPushButton::clicked, this, [this]() { setSpeed(100); });
		speed = new QSlider(Qt::Horizontal, this);
		speed->setRange(5, 400);
		speed->setValue(100);
		speedLbl = new QLabel("100%", this);
		speedLbl->setMinimumWidth(48);
		connect(speed, &QSlider::valueChanged, this, [this](int v) {
			speedLbl->setText(QString::number(v) + "%");
			if (!updating)
				setSpeed(v);
		});
		sp->addWidget(b25);
		sp->addWidget(b50);
		sp->addWidget(b100);
		sp->addWidget(speed, 1);
		sp->addWidget(speedLbl);
		root->addWidget(spBox);

		/* камеры подряд */
		QGroupBox *seqBox = new QGroupBox(T("ControlSequence"), this);
		QVBoxLayout *seqCol = new QVBoxLayout(seqBox);
		QHBoxLayout *seqRow = new QHBoxLayout();
		for (int i = 0; i < 4; i++) {
			seqCam[i] = new QCheckBox(T("ControlCamShort") + " " + QString::number(i + 1), this);
			seqCam[i]->setChecked(true);
			const QString key = QString("seq_cam%1").arg(i + 1);
			connect(seqCam[i], &QCheckBox::toggled, this,
				[this, key](bool on) { applyBool(key.toUtf8().constData(), on); });
			seqRow->addWidget(seqCam[i]);
		}
		QPushButton *seqPlay = new QPushButton(T("ControlSeqPlay"), this);
		seqPlay->setMinimumHeight(40);
		seqPlay->setStyleSheet("QPushButton{background:#1565c0;color:white;font-weight:bold;font-size:15px;}"
				       "QPushButton:pressed{background:#0d47a1;}");
		connect(seqPlay, &QPushButton::clicked, this, [this]() { trig("mc_seq_play"); });
		autoSeq = new QCheckBox(T("ControlAutoSeq"), this);
		connect(autoSeq, &QCheckBox::toggled, this, [this](bool on) { applyBool("autoplay_seq", on); });
		seqCol->addLayout(seqRow);
		seqCol->addWidget(seqPlay);
		seqCol->addWidget(autoSeq);
		root->addWidget(seqBox);

		/* возврат на сцену */
		QGroupBox *retBox = new QGroupBox(T("ControlReturn"), this);
		QGridLayout *rg = new QGridLayout(retBox);
		retEnable = new QCheckBox(T("ControlReturnEnable"), this);
		connect(retEnable, &QCheckBox::toggled, this, [this](bool on) { applyBool("return_enabled", on); });
		retScene = new QComboBox(this);
		retScene->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		connect(retScene, &QComboBox::currentTextChanged, this, [this](const QString &t) {
			if (!updating)
				applyString("return_scene", t.toUtf8().constData());
		});
		retDelay = new QSpinBox(this);
		retDelay->setRange(0, 60000);
		retDelay->setSingleStep(100);
		retDelay->setSuffix(" " + T("ControlMsShort"));
		connect(retDelay, QOverload<int>::of(&QSpinBox::valueChanged), this,
			[this](int v) { applySetting("return_delay_ms", v); });
		rg->addWidget(retEnable, 0, 0, 1, 2);
		rg->addWidget(new QLabel(T("ControlReturnScene"), this), 1, 0);
		rg->addWidget(retScene, 1, 1);
		rg->addWidget(new QLabel(T("ControlReturnDelay"), this), 2, 0);
		rg->addWidget(retDelay, 2, 1);
		root->addWidget(retBox);

		/* параметры клипа */
		QHBoxLayout *prm = new QHBoxLayout();
		prm->addWidget(new QLabel(T("ControlClipLen"), this));
		clipSec = new QSpinBox(this);
		clipSec->setRange(1, 120);
		clipSec->setSuffix(" " + T("ControlSecShort"));
		prm->addWidget(clipSec);
		prm->addSpacing(12);
		prm->addWidget(new QLabel(T("ControlPostRoll"), this));
		postMs = new QSpinBox(this);
		postMs->setRange(0, 30000);
		postMs->setSingleStep(100);
		postMs->setSuffix(" " + T("ControlMsShort"));
		prm->addWidget(postMs);
		prm->addStretch(1);
		root->addLayout(prm);
		connect(clipSec, QOverload<int>::of(&QSpinBox::valueChanged), this,
			[this](int v) { applySetting("clip_seconds", v); });
		connect(postMs, QOverload<int>::of(&QSpinBox::valueChanged), this,
			[this](int v) { applySetting("post_ms", v); });

		/* статус и «поверх всех окон» */
		onTop = new QCheckBox(T("ControlOnTop"), this);
		connect(onTop, &QCheckBox::toggled, this, [this](bool on) {
			const bool wasVisible = isVisible();
			setWindowFlag(Qt::WindowStaysOnTopHint, on);
			if (wasVisible)
				show();
		});
		root->addWidget(onTop);

		status = new QLabel(this);
		status->setWordWrap(true);
		status->setStyleSheet("font-weight:bold;");
		root->addWidget(status);

		connect(src, &QComboBox::currentTextChanged, this, [this](const QString &) {
			if (!updating)
				loadSettings();
		});

		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start(200);
		refresh();
	}

private:
	QComboBox *src = nullptr;
	QLabel *status = nullptr;
	QLabel *speedLbl = nullptr;
	QPushButton *cam[4] = {nullptr, nullptr, nullptr, nullptr};
	QSlider *speed = nullptr;
	QSpinBox *clipSec = nullptr;
	QSpinBox *postMs = nullptr;
	QCheckBox *onTop = nullptr;
	QCheckBox *seqCam[4] = {nullptr, nullptr, nullptr, nullptr};
	QCheckBox *autoSeq = nullptr;
	QCheckBox *retEnable = nullptr;
	QComboBox *retScene = nullptr;
	QSpinBox *retDelay = nullptr;
	QStringList sceneNames;
	QString savedScene;
	int sceneTick = 0;
	QTimer *timer = nullptr;
	QStringList names;
	bool updating = false;

	QByteArray srcName() const { return src->currentText().toUtf8(); }

	void addBtn(QGridLayout *g, int row, int col, const QString &text, const char *action)
	{
		QPushButton *b = new QPushButton(text, this);
		b->setMinimumHeight(36);
		connect(b, &QPushButton::clicked, this, [this, action]() { trig(action); });
		g->addWidget(b, row, col);
	}

	void trig(const char *action)
	{
		const QByteArray n = srcName();
		if (!n.isEmpty())
			mc_replay_trigger(n.constData(), action);
		refresh();
	}

	void setSpeed(int percent)
	{
		const QByteArray n = srcName();
		if (!n.isEmpty())
			mc_replay_set_speed(n.constData(), (double)percent);
		refresh();
	}

	/* записать настройку в источник: она сохранится в сцене и обновит ядро */
	void applySetting(const char *key, int value)
	{
		if (updating)
			return;
		const QByteArray n = srcName();
		if (n.isEmpty())
			return;
		obs_source_t *s = obs_get_source_by_name(n.constData());
		if (!s)
			return;
		obs_data_t *d = obs_source_get_settings(s);
		obs_data_set_int(d, key, value);
		obs_source_update(s, d);
		obs_data_release(d);
		obs_source_release(s);
	}

	void applyBool(const char *key, bool value)
	{
		if (updating)
			return;
		const QByteArray n = srcName();
		if (n.isEmpty())
			return;
		obs_source_t *s = obs_get_source_by_name(n.constData());
		if (!s)
			return;
		obs_data_t *d = obs_source_get_settings(s);
		obs_data_set_bool(d, key, value);
		obs_source_update(s, d);
		obs_data_release(d);
		obs_source_release(s);
	}

	void applyString(const char *key, const char *value)
	{
		if (updating)
			return;
		const QByteArray n = srcName();
		if (n.isEmpty())
			return;
		obs_source_t *s = obs_get_source_by_name(n.constData());
		if (!s)
			return;
		obs_data_t *d = obs_source_get_settings(s);
		obs_data_set_string(d, key, value);
		obs_source_update(s, d);
		obs_data_release(d);
		obs_source_release(s);
		savedScene = QString::fromUtf8(value);
	}

	/* список сцен OBS; выбранная сцена берётся из настроек источника */
	void fillScenes(bool force)
	{
		QStringList now;
		char **list = obs_frontend_get_scene_names();
		for (char **p = list; p && *p; p++)
			now.append(QString::fromUtf8(*p));
		bfree(list);

		if (!force && now == sceneNames)
			return;
		sceneNames = now;
		updating = true;
		retScene->clear();
		retScene->addItem(QString());
		retScene->addItems(sceneNames);
		int i = retScene->findText(savedScene);
		if (i < 0 && !savedScene.isEmpty()) {
			retScene->addItem(savedScene); /* сцены уже нет, но выбор не теряем */
			i = retScene->count() - 1;
		}
		retScene->setCurrentIndex(i < 0 ? 0 : i);
		updating = false;
	}

	void loadSettings()
	{
		const QByteArray n = srcName();
		if (n.isEmpty())
			return;
		obs_source_t *s = obs_get_source_by_name(n.constData());
		if (!s)
			return;
		obs_data_t *d = obs_source_get_settings(s);
		updating = true;
		clipSec->setValue((int)obs_data_get_int(d, "clip_seconds"));
		postMs->setValue((int)obs_data_get_int(d, "post_ms"));
		for (int i = 0; i < 4; i++)
			seqCam[i]->setChecked(obs_data_get_bool(d, QString("seq_cam%1").arg(i + 1).toUtf8().constData()));
		autoSeq->setChecked(obs_data_get_bool(d, "autoplay_seq"));
		retEnable->setChecked(obs_data_get_bool(d, "return_enabled"));
		retDelay->setValue((int)obs_data_get_int(d, "return_delay_ms"));
		savedScene = QString::fromUtf8(obs_data_get_string(d, "return_scene"));
		updating = false;
		obs_data_release(d);
		obs_source_release(s);
		fillScenes(true);
	}

	void refresh()
	{
		QStringList now;
		mc_replay_enum_names(collect_names, &now);
		now.sort();
		if (now != names) {
			names = now;
			const QString keep = src->currentText();
			updating = true;
			src->clear();
			src->addItems(names);
			const int i = src->findText(keep);
			if (i >= 0)
				src->setCurrentIndex(i);
			updating = false;
			loadSettings();
		}

		if (++sceneTick >= 10) {
			sceneTick = 0;
			fillScenes(false);
		}

		const QByteArray n = srcName();
		struct mc_replay_status st;
		if (n.isEmpty() || !mc_replay_get_status(n.constData(), &st)) {
			status->setText(T("StatusNoSource"));
			for (int i = 0; i < 4; i++)
				cam[i]->setChecked(false);
			return;
		}

		for (int i = 0; i < 4; i++)
			cam[i]->setChecked(st.cam == i + 1);

		if (!speed->isSliderDown() && speed->value() != (int)(st.speed + 0.5)) {
			updating = true;
			speed->setValue((int)(st.speed + 0.5));
			updating = false;
		}

		QString state;
		if (st.post_pending)
			state = T("StatePost");
		else if (st.hidden)
			state = T("StateHidden");
		else if (st.seq)
			state = T("StateSeqFmt").arg(st.seq_pos).arg(st.seq_total);
		else if (st.playing)
			state = st.backward ? T("StateBackward") : T("StatePlaying");
		else
			state = T("StatePaused");
		if (st.return_pending)
			state += " " + T("StateReturn");

		status->setText(T("StatusFmt")
					.arg(st.cur)
					.arg(st.clips)
					.arg(st.cam)
					.arg(state)
					.arg(st.pos_sec, 0, 'f', 1)
					.arg(st.dur_sec, 0, 'f', 1)
					.arg(st.mem_mb, 0, 'f', 0));
	}
};

static QPointer<MCControl> g_window;

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && g_window) {
		delete g_window.data();
		g_window = nullptr;
	}
}

extern "C" void mc_control_init(void)
{
	QWidget *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main_window) {
		obs_log(LOG_WARNING, "main window not found, control window is unavailable");
		return;
	}

	g_window = new MCControl(main_window);

	QAction *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("ControlMenu")));
	if (action) {
		QObject::connect(action, &QAction::triggered, [] {
			if (g_window) {
				g_window->show();
				g_window->raise();
				g_window->activateWindow();
			}
		});
	}
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
}

extern "C" void mc_control_shutdown(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (g_window) {
		delete g_window.data();
		g_window = nullptr;
	}
}
