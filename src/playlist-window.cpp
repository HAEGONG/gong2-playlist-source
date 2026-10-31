/*
gong2 Playlist Source for OBS
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "playlist-window.hpp"
#include "playlist-source.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPair>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <climits>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr const char *kDockId = "gong2_playlist_source_window";
constexpr const char *kConfigSection = "Gong2PlaylistSource";
constexpr const char *kConfigKey = "WindowSource";

constexpr int kRoleEntry = Qt::UserRole;     // int: index into Snapshot::entries
constexpr int kRoleFile = Qt::UserRole + 1;  // int: index into entries[].files, -1 if none
constexpr int kRolePath = Qt::UserRole + 2;  // QString: entry path (top-level rows only)

QString last_dir;

QString tr_key(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Dialog titles: the menu strings end in "...". */
QString plain_key(const char *key)
{
	QString s = tr_key(key);
	while (s.endsWith('.'))
		s.chop(1);
	return s;
}

QString display_name(const std::string &path, bool is_url)
{
	const QString full = QString::fromUtf8(path.c_str());
	if (is_url)
		return full;
	const QString name = QFileInfo(QDir::cleanPath(full)).fileName();
	return name.isEmpty() ? full : name;
}

QString format_time(int64_t ms, bool with_hours)
{
	const int64_t total = std::max<int64_t>(ms, 0) / 1000;
	const int64_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
	if (with_hours)
		return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
	return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}

QString state_text(obs_media_state state)
{
	switch (state) {
	case OBS_MEDIA_STATE_PLAYING:
		return tr_key("Window.State.Playing");
	case OBS_MEDIA_STATE_PAUSED:
		return tr_key("Window.State.Paused");
	case OBS_MEDIA_STATE_STOPPED:
		return tr_key("Window.State.Stopped");
	case OBS_MEDIA_STATE_ENDED:
		return tr_key("Window.State.Ended");
	case OBS_MEDIA_STATE_OPENING:
		return tr_key("Window.State.Opening");
	case OBS_MEDIA_STATE_BUFFERING:
		return tr_key("Window.State.Buffering");
	case OBS_MEDIA_STATE_ERROR:
		return tr_key("Window.State.Error");
	default:
		return tr_key("Window.State.None");
	}
}

/* Tree whose top-level rows can be reordered by drag & drop and which accepts
 * files dropped from the file manager. The files inside a folder can be
 * reordered too: when every selected row is a child of the same folder, the
 * drag is a "child drag" that may only be dropped between that folder's
 * children. It never lets Qt move items itself: it only reports the drop row
 * and the window rewrites the playlist. */
class PlaylistTree : public QTreeWidget {
public:
	std::function<void(int row)> on_move;                              // internal reorder
	std::function<void(int folder, int row)> on_move_children;         // child drag: top-level index, child row
	std::function<void(const QStringList &paths, int row)> on_files;   // external drop
	std::function<void()> on_delete;

	explicit PlaylistTree(QWidget *parent = nullptr) : QTreeWidget(parent)
	{
		setHeaderHidden(true);
		setColumnCount(1);
		setSelectionMode(ExtendedSelection);
		setAlternatingRowColors(false);
		setUniformRowHeights(true);
		setEditTriggers(NoEditTriggers);
		setDragDropMode(DragDrop);
		setDefaultDropAction(Qt::MoveAction);
		setDropIndicatorShown(false); // we paint our own, between top-level rows
		setContextMenuPolicy(Qt::CustomContextMenu);
	}

protected:
	void keyPressEvent(QKeyEvent *e) override
	{
		if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && on_delete) {
			on_delete();
			e->accept();
			return;
		}
		QTreeWidget::keyPressEvent(e);
	}

	void dragEnterEvent(QDragEnterEvent *e) override
	{
		if (!acceptable(e->source(), e->mimeData())) {
			e->ignore();
			return;
		}
		QTreeWidget::dragEnterEvent(e);
		e->acceptProposedAction();
	}

	void startDrag(Qt::DropActions actions) override
	{
		// Children can only be dragged together within their folder.
		const auto items = selectedItems();
		const bool only_children =
			!items.isEmpty() && std::all_of(items.begin(), items.end(), [](const QTreeWidgetItem *it) {
				return it->parent() != nullptr;
			});
		if (only_children && child_drag_folder() < 0)
			return;
		QTreeWidget::startDrag(actions);
	}

	void dragMoveEvent(QDragMoveEvent *e) override
	{
		if (!acceptable(e->source(), e->mimeData())) {
			e->ignore();
			return;
		}
		QTreeWidget::dragMoveEvent(e); // keeps auto-scroll working
		const QPoint pos = e->position().toPoint();
		drop_folder_ = is_internal(e->source()) ? child_drag_folder() : -1;
		if (drop_folder_ >= 0) {
			drop_row_ = child_drop_row_at(pos, drop_folder_);
			if (drop_row_ < 0)
				e->ignore(); // not between the children of the dragged folder
			else
				e->acceptProposedAction();
		} else {
			drop_row_ = drop_row_at(pos);
			e->acceptProposedAction();
		}
		viewport()->update();
	}

	void dragLeaveEvent(QDragLeaveEvent *e) override
	{
		drop_row_ = -1;
		drop_folder_ = -1;
		viewport()->update();
		QTreeWidget::dragLeaveEvent(e);
	}

	void dropEvent(QDropEvent *e) override
	{
		const QPoint pos = e->position().toPoint();
		const bool internal = is_internal(e->source());
		const int folder = internal ? child_drag_folder() : -1;
		const int row = folder >= 0 ? child_drop_row_at(pos, folder) : drop_row_at(pos);
		drop_row_ = -1;
		drop_folder_ = -1;
		stopAutoScroll();
		setState(NoState);
		viewport()->update();

		if (!acceptable(e->source(), e->mimeData())) {
			e->ignore();
			return;
		}
		if (internal) {
			if (folder >= 0 && row < 0) {
				e->ignore();
				return;
			}
			// IgnoreAction: Qt must not remove the dragged rows, the playlist
			// signal rebuilds the tree.
			e->setDropAction(Qt::IgnoreAction);
			e->accept();
			if (folder >= 0) {
				if (on_move_children)
					on_move_children(folder, row);
			} else if (on_move) {
				on_move(row);
			}
			return;
		}
		e->setDropAction(Qt::CopyAction);
		e->accept();
		const QStringList paths = local_paths(e->mimeData());
		if (on_files && !paths.isEmpty())
			on_files(paths, row);
	}

	void paintEvent(QPaintEvent *e) override
	{
		QTreeWidget::paintEvent(e);
		if (drop_row_ < 0)
			return;
		QPainter p(viewport());
		const int y = drop_folder_ >= 0 ? child_drop_line_y(drop_folder_, drop_row_) : drop_line_y(drop_row_);
		p.fillRect(QRect(0, y - 1, viewport()->width(), 2), palette().color(QPalette::Highlight));
	}

private:
	int drop_row_ = -1;
	int drop_folder_ = -1; // top-level index of the folder during a child drag, else -1

	bool is_internal(const QObject *src) const { return src == this || src == viewport(); }

	static QStringList local_paths(const QMimeData *mime)
	{
		QStringList paths;
		if (!mime || !mime->hasUrls())
			return paths;
		for (const QUrl &url : mime->urls())
			if (url.isLocalFile())
				paths << url.toLocalFile();
		return paths;
	}

	bool acceptable(const QObject *src, const QMimeData *mime) const
	{
		return is_internal(src) || !local_paths(mime).isEmpty();
	}

	/* Top-level index of the folder when every selected row is a child of that
	 * same folder (a child drag), else -1. */
	int child_drag_folder() const
	{
		const auto items = selectedItems();
		if (items.isEmpty())
			return -1;
		QTreeWidgetItem *parent = items.first()->parent();
		if (!parent)
			return -1;
		for (const QTreeWidgetItem *it : items)
			if (it->parent() != parent)
				return -1;
		return indexOfTopLevelItem(parent);
	}

	/* Insertion index among the children of `folder` (0..count), or -1 when
	 * the position is not on one of its children. */
	int child_drop_row_at(const QPoint &pos, int folder) const
	{
		QTreeWidgetItem *item = itemAt(pos);
		if (!item || !item->parent() || indexOfTopLevelItem(item->parent()) != folder)
			return -1;
		const int idx = item->parent()->indexOfChild(item);
		return pos.y() < visualItemRect(item).center().y() ? idx : idx + 1;
	}

	int child_drop_line_y(int folder, int row) const
	{
		QTreeWidgetItem *top = topLevelItem(folder);
		if (!top || top->childCount() == 0)
			return 0;
		if (row < top->childCount())
			return visualItemRect(top->child(std::max(row, 0))).top();
		return visualItemRect(top->child(top->childCount() - 1)).bottom() + 1;
	}

	/* Insertion index among the top-level rows (0..count). Hovering a folder's
	 * children means "after the folder". */
	int drop_row_at(const QPoint &pos) const
	{
		QTreeWidgetItem *item = itemAt(pos);
		if (!item)
			return topLevelItemCount();
		QTreeWidgetItem *top = item->parent() ? item->parent() : item;
		const int idx = indexOfTopLevelItem(top);
		if (item != top)
			return idx + 1;
		return pos.y() < visualItemRect(item).center().y() ? idx : idx + 1;
	}

	int drop_line_y(int row) const
	{
		if (topLevelItemCount() == 0)
			return 0;
		if (row <= 0)
			return visualItemRect(topLevelItem(0)).top();
		QTreeWidgetItem *last = topLevelItem(std::min(row, topLevelItemCount()) - 1);
		while (last->isExpanded() && last->childCount() > 0)
			last = last->child(last->childCount() - 1);
		return visualItemRect(last).bottom() + 1;
	}
};

class PlaylistWindow : public QWidget {
public:
	explicit PlaylistWindow(QWidget *parent) : QWidget(parent)
	{
		setObjectName("Gong2PlaylistSourceWindow");
		setWindowTitle(tr_key("PlaylistWindow"));

		const QStyle *st = style();
		icon_play_ = st->standardIcon(QStyle::SP_MediaPlay);
		icon_pause_ = st->standardIcon(QStyle::SP_MediaPause);
		QPixmap blank(16, 16);
		blank.fill(Qt::transparent);
		icon_blank_ = QIcon(blank);

		build_ui();
		wire_ui();

		rebuild_timer_.setSingleShot(true);
		rebuild_timer_.setInterval(0);
		connect(&rebuild_timer_, &QTimer::timeout, this, [this] { rebuild_sources(); });

		poll_timer_.setInterval(250);
		connect(&poll_timer_, &QTimer::timeout, this, [this] { poll_status(); });

		if (config_t *cfg = obs_frontend_get_user_config()) {
			const char *saved = config_get_string(cfg, kConfigSection, kConfigKey);
			if (saved)
				selected_uuid_ = QString::fromUtf8(saved);
		}

		connect_global_signals();
		rebuild_sources();
	}

	~PlaylistWindow() override
	{
		disconnect_global_signals();
		detach_source();
	}

	/* Shows the dock (floating if it was hidden) and brings it to the front. */
	void present()
	{
		QDockWidget *dock = dock_widget();
		QWidget *top = dock ? static_cast<QWidget *>(dock) : this;
		if (!top->isVisible()) {
			if (dock)
				dock->setFloating(true);
			top->show();
			top->resize(std::max(top->width(), 420), std::max(top->height(), 560));
		}
		top->raise();
		top->activateWindow();
	}

	void open_for(obs_source_t *source)
	{
		if (const char *uuid = source ? obs_source_get_uuid(source) : nullptr) {
			selected_uuid_ = QString::fromUtf8(uuid);
			save_selected_uuid();
		}
		rebuild_sources();
		present();
	}

	/* Stops talking to libobs entirely (scene collection cleanup / exit). */
	void detach_all()
	{
		detach_source();
		clear_view();
		QSignalBlocker block(combo_);
		set_no_sources();
	}

	void shutdown()
	{
		shutting_down_ = true;
		rebuild_timer_.stop();
		disconnect_global_signals();
		detach_all();
	}

	void rebuild_sources_soon() { rebuild_timer_.start(); }

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		update_timer();
		poll_status();
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		update_timer();
	}

private:
	/* ---- widgets ---- */
	QComboBox *combo_ = nullptr;
	QWidget *body_ = nullptr;
	PlaylistTree *tree_ = nullptr;
	QSlider *slider_ = nullptr;
	QLabel *status_ = nullptr;
	QToolButton *btn_prev_ = nullptr, *btn_play_ = nullptr, *btn_stop_ = nullptr, *btn_next_ = nullptr;
	QPushButton *btn_add_files_ = nullptr, *btn_add_folder_ = nullptr, *btn_add_url_ = nullptr;
	QPushButton *btn_remove_ = nullptr, *btn_up_ = nullptr, *btn_down_ = nullptr;
	QIcon icon_play_, icon_pause_, icon_blank_;

	/* ---- state ---- */
	QTimer rebuild_timer_;
	QTimer poll_timer_;
	QString selected_uuid_; // what the user wants; survives scene collection changes
	QString attached_uuid_;
	OBSWeakSourceAutoRelease weak_;
	playlist::Snapshot snap_;
	QStringList tree_signature_;
	QString last_current_key_;
	QSet<QPair<int, int>> pending_select_; // (entry, file); file -1 = the top-level row
	bool has_pending_select_ = false;
	bool rebuilding_ = false;
	bool slider_dragging_ = false;
	bool global_connected_ = false;
	bool shutting_down_ = false;
	std::atomic<bool> playlist_pending_{false};
	std::atomic<bool> current_pending_{false};

	/* ---- construction ---- */
	void build_ui()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(6, 6, 6, 6);

		auto *top_row = new QHBoxLayout;
		top_row->addWidget(new QLabel(tr_key("Window.Source"), this));
		combo_ = new QComboBox(this);
		combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo_->setMinimumContentsLength(12);
		top_row->addWidget(combo_, 1);
		root->addLayout(top_row);

		body_ = new QWidget(this);
		auto *body = new QVBoxLayout(body_);
		body->setContentsMargins(0, 0, 0, 0);
		root->addWidget(body_, 1);

		tree_ = new PlaylistTree(body_);
		tree_->setIconSize(QSize(16, 16));
		body->addWidget(tree_, 1);

		slider_ = new QSlider(Qt::Horizontal, body_);
		slider_->setRange(0, 0);
		body->addWidget(slider_);

		status_ = new QLabel(body_);
		status_->setAlignment(Qt::AlignCenter);
		body->addWidget(status_);

		auto *transport = new QHBoxLayout;
		transport->addStretch(1);
		btn_prev_ = make_tool_button(QStyle::SP_MediaSkipBackward, "Window.Previous");
		btn_play_ = make_tool_button(QStyle::SP_MediaPlay, "Window.PlayPause");
		btn_stop_ = make_tool_button(QStyle::SP_MediaStop, "Window.Stop");
		btn_next_ = make_tool_button(QStyle::SP_MediaSkipForward, "Window.Next");
		for (QToolButton *b : {btn_prev_, btn_play_, btn_stop_, btn_next_})
			transport->addWidget(b);
		transport->addStretch(1);
		body->addLayout(transport);

		auto *edit = new QGridLayout;
		btn_add_files_ = new QPushButton(tr_key("Window.AddFiles"), body_);
		btn_add_folder_ = new QPushButton(tr_key("Window.AddFolder"), body_);
		btn_add_url_ = new QPushButton(tr_key("Window.AddURL"), body_);
		btn_remove_ = new QPushButton(tr_key("Window.Remove"), body_);
		btn_up_ = new QPushButton(tr_key("Window.MoveUp"), body_);
		btn_down_ = new QPushButton(tr_key("Window.MoveDown"), body_);
		edit->addWidget(btn_add_files_, 0, 0);
		edit->addWidget(btn_add_folder_, 0, 1);
		edit->addWidget(btn_add_url_, 0, 2);
		edit->addWidget(btn_remove_, 1, 0);
		edit->addWidget(btn_up_, 1, 1);
		edit->addWidget(btn_down_, 1, 2);
		body->addLayout(edit);

		set_no_sources();
		clear_view();
	}

	QToolButton *make_tool_button(QStyle::StandardPixmap icon, const char *tip_key)
	{
		auto *b = new QToolButton(body_);
		b->setIcon(style()->standardIcon(icon));
		b->setToolTip(tr_key(tip_key));
		b->setAutoRaise(false);
		return b;
	}

	void wire_ui()
	{
		connect(combo_, &QComboBox::activated, this, [this](int index) {
			selected_uuid_ = combo_->itemData(index).toString();
			save_selected_uuid();
			attach(selected_uuid_);
		});

		connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) { play_item(item); });
		connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
			if (!rebuilding_)
				has_pending_select_ = false;
			update_buttons();
		});
		connect(tree_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) { show_menu(pos); });
		tree_->on_move = [this](int row) { move_to_row(row); };
		tree_->on_move_children = [this](int folder, int row) { move_children_to_row(folder, row); };
		tree_->on_files = [this](const QStringList &paths, int row) { add_paths(paths, row); };
		tree_->on_delete = [this] { remove_selected(); };

		connect(slider_, &QSlider::sliderPressed, this, [this] { slider_dragging_ = true; });
		connect(slider_, &QSlider::sliderReleased, this, [this] {
			slider_dragging_ = false;
			OBSSourceAutoRelease src = source();
			if (src)
				obs_source_media_set_time(src, slider_->value());
		});

		connect(btn_prev_, &QToolButton::clicked, this, [this] { media_action(obs_source_media_previous); });
		connect(btn_next_, &QToolButton::clicked, this, [this] { media_action(obs_source_media_next); });
		connect(btn_stop_, &QToolButton::clicked, this, [this] { media_action(obs_source_media_stop); });
		connect(btn_play_, &QToolButton::clicked, this, [this] { on_play_pause(); });

		connect(btn_add_files_, &QPushButton::clicked, this, [this] { add_files(); });
		connect(btn_add_folder_, &QPushButton::clicked, this, [this] { add_folder(); });
		connect(btn_add_url_, &QPushButton::clicked, this, [this] { add_url(); });
		connect(btn_remove_, &QPushButton::clicked, this, [this] { remove_selected(); });
		connect(btn_up_, &QPushButton::clicked, this, [this] { move_selected(-1); });
		connect(btn_down_, &QPushButton::clicked, this, [this] { move_selected(+1); });
	}

	QDockWidget *dock_widget() const { return qobject_cast<QDockWidget *>(parentWidget()); }

	/* ---- libobs plumbing ---- */
	OBSSourceAutoRelease source() const { return OBSSourceAutoRelease(obs_weak_source_get_source(weak_)); }

	void media_action(void (*fn)(obs_source_t *))
	{
		OBSSourceAutoRelease src = source();
		if (src)
			fn(src);
	}

	/* The signal callbacks run on the graphics thread (source signals) or an
	 * arbitrary thread (global signals): they only queue work for the UI thread. */
	static void on_global_source_signal(void *param, calldata_t *)
	{
		auto *w = static_cast<PlaylistWindow *>(param);
		QMetaObject::invokeMethod(w, [w] { w->rebuild_sources_soon(); }, Qt::QueuedConnection);
	}

	static void on_playlist_changed(void *param, calldata_t *)
	{
		auto *w = static_cast<PlaylistWindow *>(param);
		if (w->playlist_pending_.exchange(true))
			return;
		QMetaObject::invokeMethod(
			w,
			[w] {
				w->playlist_pending_ = false;
				w->refresh_playlist();
			},
			Qt::QueuedConnection);
	}

	static void on_current_changed(void *param, calldata_t *)
	{
		auto *w = static_cast<PlaylistWindow *>(param);
		if (w->current_pending_.exchange(true))
			return;
		QMetaObject::invokeMethod(
			w,
			[w] {
				w->current_pending_ = false;
				w->refresh_current();
			},
			Qt::QueuedConnection);
	}

	void connect_global_signals()
	{
		signal_handler_t *sh = obs_get_signal_handler();
		if (!sh || global_connected_)
			return;
		for (const char *sig : {"source_create", "source_destroy", "source_rename"})
			signal_handler_connect(sh, sig, on_global_source_signal, this);
		global_connected_ = true;
	}

	void disconnect_global_signals()
	{
		if (!global_connected_)
			return;
		global_connected_ = false;
		signal_handler_t *sh = obs_get_signal_handler();
		if (!sh)
			return;
		for (const char *sig : {"source_create", "source_destroy", "source_rename"})
			signal_handler_disconnect(sh, sig, on_global_source_signal, this);
	}

	void detach_source()
	{
		poll_timer_.stop();
		// If the source is already gone, its signal handler (and our
		// connections) went with it, so there is nothing to disconnect.
		OBSSourceAutoRelease src = source();
		if (src) {
			signal_handler_t *sh = obs_source_get_signal_handler(src);
			signal_handler_disconnect(sh, playlist::SIGNAL_PLAYLIST_CHANGED, on_playlist_changed, this);
			signal_handler_disconnect(sh, playlist::SIGNAL_CURRENT_CHANGED, on_current_changed, this);
		}
		weak_ = nullptr;
		attached_uuid_.clear();
	}

	void save_selected_uuid()
	{
		config_t *cfg = obs_frontend_get_user_config();
		if (!cfg)
			return;
		config_set_string(cfg, kConfigSection, kConfigKey, selected_uuid_.toUtf8().constData());
		config_save_safe(cfg, "tmp", nullptr);
	}

	/* ---- source list ---- */
	void set_no_sources()
	{
		combo_->clear();
		combo_->addItem(tr_key("Window.NoSources"));
		if (auto *model = qobject_cast<QStandardItemModel *>(combo_->model()))
			if (QStandardItem *item = model->item(0))
				item->setEnabled(false);
		combo_->setEnabled(false);
		body_->setEnabled(false);
	}

	void rebuild_sources()
	{
		if (shutting_down_)
			return;

		struct Found {
			QString name, uuid;
		};
		std::vector<Found> found;
		obs_enum_sources(
			[](void *param, obs_source_t *s) {
				if (playlist::is_playlist_source(s))
					static_cast<std::vector<Found> *>(param)->push_back(
						{QString::fromUtf8(obs_source_get_name(s)),
						 QString::fromUtf8(obs_source_get_uuid(s))});
				return true;
			},
			&found);
		std::sort(found.begin(), found.end(),
			  [](const Found &a, const Found &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });

		if (found.empty()) {
			detach_all();
			return;
		}

		{
			QSignalBlocker block(combo_);
			combo_->clear();
			for (const Found &f : found)
				combo_->addItem(f.name, f.uuid);
			int index = combo_->findData(selected_uuid_);
			if (index < 0)
				index = 0; // gone: fall back to the first without overwriting the saved choice
			combo_->setCurrentIndex(index);
			combo_->setEnabled(true);
		}

		const QString uuid = combo_->currentData().toString();
		OBSSourceAutoRelease current = source();
		if (uuid != attached_uuid_ || !current)
			attach(uuid);
	}

	void attach(const QString &uuid)
	{
		detach_source();
		clear_view();

		OBSSourceAutoRelease src = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (!src || !playlist::is_playlist_source(src)) {
			body_->setEnabled(false);
			return;
		}

		weak_ = obs_source_get_weak_source(src);
		attached_uuid_ = uuid;
		signal_handler_t *sh = obs_source_get_signal_handler(src);
		signal_handler_connect(sh, playlist::SIGNAL_PLAYLIST_CHANGED, on_playlist_changed, this);
		signal_handler_connect(sh, playlist::SIGNAL_CURRENT_CHANGED, on_current_changed, this);

		body_->setEnabled(true);
		refresh_playlist();
		update_timer();
		poll_status();
	}

	void update_timer()
	{
		if (weak_ != nullptr && isVisible())
			poll_timer_.start();
		else
			poll_timer_.stop();
	}

	/* ---- view ---- */
	void clear_view()
	{
		rebuilding_ = true;
		tree_->clear();
		rebuilding_ = false;
		snap_ = playlist::Snapshot();
		tree_signature_.clear();
		last_current_key_.clear();
		has_pending_select_ = false;
		slider_dragging_ = false;
		slider_->setRange(0, 0);
		slider_->setValue(0);
		slider_->setEnabled(false);
		status_->setText(state_text(OBS_MEDIA_STATE_NONE) + "  " + format_time(0, false) + " / " +
				 format_time(0, false));
		btn_play_->setIcon(icon_play_);
		update_buttons();
	}

	static QStringList signature_of(const playlist::Snapshot &s)
	{
		QStringList sig;
		for (const auto &e : s.entries) {
			QString item = QString("%1|%2|%3|%4|%5")
					       .arg(QString::fromUtf8(e.path.c_str()))
					       .arg(e.files.size())
					       .arg(e.missing)
					       .arg(e.custom_order)
					       .arg(e.excluded_count);
			if (e.is_folder) // a new play order must rebuild the rows
				for (const std::string &f : e.files)
					item += '|' + QString::fromUtf8(f.c_str());
			sig << item;
		}
		return sig;
	}

	void refresh_snapshot()
	{
		snap_ = playlist::Snapshot();
		OBSSourceAutoRelease src = source();
		if (!src || !playlist::get_snapshot(src, snap_))
			snap_ = playlist::Snapshot();
	}

	void refresh_playlist()
	{
		if (weak_ == nullptr)
			return;
		refresh_snapshot();
		rebuild_tree();
	}

	void refresh_current()
	{
		if (weak_ == nullptr)
			return;
		refresh_snapshot();
		if (signature_of(snap_) != tree_signature_) {
			rebuild_tree(); // the playlist signal has not been handled yet
			return;
		}
		apply_marks();
		reveal_current_if_changed();
		update_play_icon();
	}

	void rebuild_tree()
	{
		rebuilding_ = true;

		// Remember what the user sees so the rebuild is not disruptive.
		QSet<QPair<int, int>> selected;
		if (has_pending_select_) {
			selected = pending_select_;
			has_pending_select_ = false;
		} else {
			for (QTreeWidgetItem *it : tree_->selectedItems())
				if (!it->parent())
					selected.insert({it->data(0, kRoleEntry).toInt(), -1});
		}
		QSet<QString> expanded;
		for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
			QTreeWidgetItem *it = tree_->topLevelItem(i);
			if (it->isExpanded())
				expanded.insert(it->data(0, kRolePath).toString());
		}
		const int scroll = tree_->verticalScrollBar()->value();

		QSignalBlocker block(tree_);
		tree_->clear();

		const QBrush grey = tree_->palette().brush(QPalette::Disabled, QPalette::Text);
		const QString missing_suffix = " " + tr_key("Window.Missing");
		constexpr Qt::ItemFlags top_flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
		constexpr Qt::ItemFlags child_flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;

		for (int i = 0; i < (int)snap_.entries.size(); ++i) {
			const playlist::EntryInfo &e = snap_.entries[i];
			const QString path = QString::fromUtf8(e.path.c_str());
			QString text = display_name(e.path, e.is_url);
			if (e.is_folder)
				text += QString(" (%1)").arg(e.files.size());
			if (e.is_folder && e.custom_order)
				text += QString(" ") + QChar(0x2195); // custom play order marker
			if (e.missing)
				text += missing_suffix;

			auto *item = new QTreeWidgetItem(tree_);
			item->setText(0, text);
			item->setToolTip(0, path);
			item->setFlags(top_flags);
			item->setData(0, kRoleEntry, i);
			item->setData(0, kRoleFile, e.is_folder ? -1 : (e.files.empty() ? -1 : 0));
			item->setData(0, kRolePath, path);
			if (e.missing)
				item->setForeground(0, grey);

			if (e.is_folder && e.files.empty()) {
				auto *child = new QTreeWidgetItem(item);
				child->setText(0, tr_key("Window.EmptyFolder"));
				child->setFlags(Qt::NoItemFlags);
				child->setForeground(0, grey);
				child->setData(0, kRoleEntry, i);
				child->setData(0, kRoleFile, -1);
			}
			if (e.is_folder) {
				bool child_selected = false;
				for (int f = 0; f < (int)e.files.size(); ++f) {
					auto *child = new QTreeWidgetItem(item);
					child->setText(0, display_name(e.files[f], false));
					child->setToolTip(0, QString::fromUtf8(e.files[f].c_str()));
					child->setFlags(child_flags);
					child->setData(0, kRoleEntry, i);
					child->setData(0, kRoleFile, f);
					if (selected.contains({i, f})) {
						child->setSelected(true);
						child_selected = true;
					}
				}
				item->setExpanded(expanded.contains(path) || child_selected);
			}
			if (selected.contains({i, -1}))
				item->setSelected(true);
		}

		tree_signature_ = signature_of(snap_);
		apply_marks();

		tree_->doItemsLayout();
		tree_->verticalScrollBar()->setValue(scroll);
		reveal_current_if_changed();

		rebuilding_ = false;
		update_buttons();
		update_play_icon();
	}

	QTreeWidgetItem *current_item() const
	{
		if (snap_.current_entry < 0 || snap_.current_entry >= tree_->topLevelItemCount())
			return nullptr;
		QTreeWidgetItem *top = tree_->topLevelItem(snap_.current_entry);
		const auto &e = snap_.entries[snap_.current_entry];
		if (e.is_folder && snap_.current_file >= 0 && snap_.current_file < top->childCount())
			return top->child(snap_.current_file);
		return top;
	}

	QString current_key() const
	{
		if (snap_.current_entry < 0 || snap_.current_entry >= (int)snap_.entries.size())
			return QString();
		const auto &files = snap_.entries[snap_.current_entry].files;
		if (snap_.current_file < 0 || snap_.current_file >= (int)files.size())
			return QString();
		return QString::fromUtf8(files[snap_.current_file].c_str());
	}

	/* Bold + play/pause icon on the current row (and bold on its folder). */
	void apply_marks()
	{
		const QIcon &icon = snap_.state == OBS_MEDIA_STATE_PAUSED ? icon_pause_ : icon_play_;
		auto mark = [&](QTreeWidgetItem *it, bool bold, bool playing) {
			QFont f = it->font(0);
			f.setBold(bold);
			it->setFont(0, f);
			it->setIcon(0, playing ? icon : icon_blank_);
		};
		for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
			QTreeWidgetItem *top = tree_->topLevelItem(i);
			const bool is_cur = i == snap_.current_entry;
			const bool folder = i < (int)snap_.entries.size() && snap_.entries[i].is_folder;
			mark(top, is_cur, is_cur && !folder);
			for (int c = 0; c < top->childCount(); ++c) {
				QTreeWidgetItem *child = top->child(c);
				const bool playing = is_cur && child->data(0, kRoleFile).toInt() == snap_.current_file;
				mark(child, playing, playing);
			}
		}
	}

	void reveal_current_if_changed()
	{
		const QString key = current_key();
		if (key == last_current_key_)
			return;
		last_current_key_ = key;
		if (QTreeWidgetItem *item = current_item()) {
			if (item->parent())
				item->parent()->setExpanded(true);
			tree_->scrollToItem(item);
		}
	}

	void update_play_icon()
	{
		btn_play_->setIcon(snap_.state == OBS_MEDIA_STATE_PLAYING ? icon_pause_ : icon_play_);
	}

	void poll_status()
	{
		OBSSourceAutoRelease src = source();
		if (!src)
			return;

		const obs_media_state state = obs_source_media_get_state(src);
		const int64_t time = obs_source_media_get_time(src);
		const int64_t duration = obs_source_media_get_duration(src);

		if (state != snap_.state) {
			snap_.state = state;
			apply_marks();
			update_play_icon();
		}

		const bool seekable = duration > 0;
		slider_->setEnabled(seekable);
		if (!slider_dragging_) {
			slider_->setRange(0, seekable ? (int)std::min<int64_t>(duration, INT_MAX) : 0);
			slider_->setValue(seekable ? (int)std::clamp<int64_t>(time, 0, duration) : 0);
		}
		const bool hours = duration >= 3600 * 1000;
		status_->setText(state_text(state) + "  " + format_time(time, hours) + " / " +
				 format_time(std::max<int64_t>(duration, 0), hours));
	}

	/* ---- selection helpers ---- */
	std::vector<int> selected_entries() const
	{
		std::vector<int> out;
		for (QTreeWidgetItem *it : tree_->selectedItems())
			if (!it->parent())
				out.push_back(it->data(0, kRoleEntry).toInt());
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	/* The selected files of one folder entry. */
	struct ChildSelection {
		int entry = -1;
		std::string folder;             // the folder entry's path
		std::vector<std::string> files; // selected file paths
	};

	/* True when every selected row is a file of the same folder entry (and at
	 * least one is selected). The paths come from the snapshot the tree shows. */
	bool selected_children(ChildSelection &out) const
	{
		const auto items = tree_->selectedItems();
		if (items.isEmpty())
			return false;
		const QTreeWidgetItem *parent = items.first()->parent();
		if (!parent)
			return false;
		const int entry = parent->data(0, kRoleEntry).toInt();
		if (entry < 0 || entry >= (int)snap_.entries.size() || !snap_.entries[entry].is_folder)
			return false;
		const auto &files = snap_.entries[entry].files;

		ChildSelection cs;
		cs.entry = entry;
		cs.folder = snap_.entries[entry].path;
		for (const QTreeWidgetItem *it : items) {
			const int file = it->data(0, kRoleFile).toInt();
			if (it->parent() != parent || file < 0 || file >= (int)files.size())
				return false;
			cs.files.push_back(files[file]);
		}
		out = std::move(cs);
		return true;
	}

	/* Top-level rows or files of folders are selected. */
	bool can_remove() const { return !tree_->selectedItems().isEmpty(); }

	void update_buttons()
	{
		ChildSelection cs;
		const bool has_sel = !selected_entries().empty();
		const bool can_move = has_sel || selected_children(cs);
		btn_remove_->setEnabled(can_remove());
		btn_up_->setEnabled(can_move);
		btn_down_->setEnabled(can_move);
	}

	/* Resolves a row to (entry, file) the same way for double-click, menu and Play. */
	bool play_target(QTreeWidgetItem *item, int &entry, int &file) const
	{
		if (!item || !(item->flags() & Qt::ItemIsEnabled))
			return false;
		entry = item->data(0, kRoleEntry).toInt();
		file = item->data(0, kRoleFile).toInt();
		if (entry < 0 || entry >= (int)snap_.entries.size())
			return false;
		const auto &e = snap_.entries[entry];
		if (!item->parent() && e.is_folder)
			file = e.files.empty() ? -1 : 0;
		return file >= 0 && file < (int)e.files.size();
	}

	bool play_item(QTreeWidgetItem *item)
	{
		int entry, file;
		OBSSourceAutoRelease src = source();
		if (!src || !play_target(item, entry, file))
			return false;
		playlist::play_item(src, entry, file);
		return true;
	}

	QTreeWidgetItem *first_selected() const
	{
		const auto items = tree_->selectedItems();
		return items.isEmpty() ? nullptr : items.first();
	}

	void on_play_pause()
	{
		OBSSourceAutoRelease src = source();
		if (!src)
			return;
		switch (obs_source_media_get_state(src)) {
		case OBS_MEDIA_STATE_PLAYING:
			obs_source_media_play_pause(src, true);
			break;
		case OBS_MEDIA_STATE_PAUSED:
			obs_source_media_play_pause(src, false);
			break;
		default:
			if (!play_item(first_selected()))
				obs_source_media_restart(src);
			break;
		}
	}

	/* ---- editing ---- */

	/* Runs fn on the latest entry paths. fn returns false for "no change".
	 * `select` lists the entry indices to keep selected after the rebuild. */
	void edit(const std::function<bool(std::vector<std::string> &, std::vector<int> &)> &fn)
	{
		OBSSourceAutoRelease src = source();
		playlist::Snapshot latest;
		if (!src || !playlist::get_snapshot(src, latest))
			return;

		std::vector<std::string> paths;
		for (const auto &e : latest.entries)
			paths.push_back(e.path);
		std::vector<int> select;
		if (!fn(paths, select))
			return;

		pending_select_.clear();
		for (int i : select)
			pending_select_.insert({i, -1});
		has_pending_select_ = true;
		playlist::set_entries(src, paths);
	}

	/* Like edit(), for the files of the folder entry `cs`. fn gets the folder's
	 * file paths in play order and which of them are selected; it returns
	 * false for "no change" and lists the indices to keep selected. */
	void edit_folder(const ChildSelection &cs,
			 const std::function<bool(std::vector<std::string> &, const std::vector<char> &,
						  std::vector<int> &)> &fn)
	{
		OBSSourceAutoRelease src = source();
		playlist::Snapshot latest;
		if (!src || !playlist::get_snapshot(src, latest))
			return;

		int entry = cs.entry;
		if (entry < 0 || entry >= (int)latest.entries.size() || latest.entries[entry].path != cs.folder) {
			entry = -1;
			for (int i = 0; i < (int)latest.entries.size(); ++i)
				if (latest.entries[i].is_folder && latest.entries[i].path == cs.folder) {
					entry = i;
					break;
				}
		}
		if (entry < 0)
			return;

		std::vector<std::string> files = latest.entries[entry].files;
		std::vector<char> sel(files.size(), 0);
		for (size_t i = 0; i < files.size(); ++i)
			sel[i] = std::find(cs.files.begin(), cs.files.end(), files[i]) != cs.files.end();
		std::vector<int> select;
		if (!fn(files, sel, select))
			return;

		pending_select_.clear();
		for (int i : select)
			pending_select_.insert({entry, i});
		has_pending_select_ = true;
		playlist::set_folder_order(src, cs.folder, files);
	}

	/* Inserts at `row` (clamped), or after the last selected entry when row < 0. */
	void insert_entries(const std::vector<std::string> &added, int row)
	{
		edit([&](std::vector<std::string> &paths, std::vector<int> &select) {
			if (added.empty())
				return false;
			if (row < 0) {
				const auto sel = selected_entries();
				row = sel.empty() ? (int)paths.size() : sel.back() + 1;
			}
			row = std::clamp(row, 0, (int)paths.size());
			paths.insert(paths.begin() + row, added.begin(), added.end());
			for (int i = 0; i < (int)added.size(); ++i)
				select.push_back(row + i);
			return true;
		});
	}

	void add_paths(const QStringList &list, int row)
	{
		std::vector<std::string> added;
		for (const QString &p : list)
			added.push_back(p.toUtf8().constData());
		insert_entries(added, row);
	}

	void add_files()
	{
		const QString filter =
			QString("%1 (*.mp4 *.mpg *.mpeg *.m4v *.ts *.mts *.m2ts *.mov *.mxf *.flv *.mkv *.avi *.gif "
				"*.webm *.wmv *.3gp *.mp3 *.m4a *.mka *.aac *.ogg *.oga *.wav *.opus *.flac *.wma);;%2 (*)")
				.arg(tr_key("MediaFilter.AllMedia"), tr_key("MediaFilter.All"));
		const QStringList files =
			QFileDialog::getOpenFileNames(this, tr_key("Window.SelectFiles"), last_dir, filter);
		if (files.isEmpty())
			return;
		last_dir = QFileInfo(files.first()).absolutePath();
		add_paths(files, -1);
	}

	void add_folder()
	{
		const QString dir = QFileDialog::getExistingDirectory(this, tr_key("Window.SelectFolder"), last_dir);
		if (dir.isEmpty())
			return;
		last_dir = dir;
		add_paths({dir}, -1);
	}

	void add_url()
	{
		bool ok = false;
		const QString url = QInputDialog::getText(this, plain_key("Window.AddURL"), tr_key("Window.AddURL.Prompt"),
							  QLineEdit::Normal, QString(), &ok)
					    .trimmed();
		if (ok && !url.isEmpty())
			add_paths({url}, -1);
	}

	/* Removes the selected rows from the playlist (never from the disk). Selected
	 * folder entries and other top-level rows leave the list; selected files of a
	 * folder are excluded from that folder, except when the folder itself is
	 * selected too. */
	void remove_selected()
	{
		OBSSourceAutoRelease src = source();
		if (!src)
			return;

		const std::vector<int> top = selected_entries();

		struct Group {
			int entry = -1;
			std::string folder;
			std::vector<std::string> files; // selected file paths
			int first = INT_MAX;            // smallest selected file index
			int removed = 0;
		};
		std::vector<Group> groups;
		for (const QTreeWidgetItem *it : tree_->selectedItems()) {
			const QTreeWidgetItem *parent = it->parent();
			if (!parent)
				continue;
			const int entry = parent->data(0, kRoleEntry).toInt();
			const int file = it->data(0, kRoleFile).toInt();
			if (entry < 0 || entry >= (int)snap_.entries.size() || !snap_.entries[entry].is_folder)
				continue;
			const auto &files = snap_.entries[entry].files;
			if (file < 0 || file >= (int)files.size())
				continue;
			if (std::binary_search(top.begin(), top.end(), entry))
				continue; // the whole folder goes
			auto g = std::find_if(groups.begin(), groups.end(), [&](const Group &x) { return x.entry == entry; });
			if (g == groups.end()) {
				groups.emplace_back();
				g = groups.end() - 1;
				g->entry = entry;
				g->folder = snap_.entries[entry].path;
			}
			g->files.push_back(files[file]);
			g->first = std::min(g->first, file);
			g->removed++;
		}
		if (top.empty() && groups.empty())
			return;

		// Exclusions first: set_entries() below reads the folder settings.
		for (const Group &g : groups)
			playlist::exclude_folder_files(src, g.folder, g.files);

		if (!top.empty()) {
			edit([&](std::vector<std::string> &paths, std::vector<int> &) {
				for (auto it = top.rbegin(); it != top.rend(); ++it)
					if (*it < (int)paths.size())
						paths.erase(paths.begin() + *it);
				return true;
			});
		} else {
			pending_select_.clear();
			has_pending_select_ = true;
		}

		// Select the row that takes the place of the first removed file.
		for (const Group &g : groups) {
			const int left = (int)snap_.entries[g.entry].files.size() - g.removed;
			if (left <= 0)
				continue;
			const int entry = g.entry - (int)(std::lower_bound(top.begin(), top.end(), g.entry) - top.begin());
			pending_select_.insert({entry, std::min(g.first, left - 1)});
		}
	}

	/* Moves the flagged items one step up (dir < 0) or down; a flagged item
	 * next to another flagged one (or at the end) stays. `sel` follows the
	 * items. Returns false if nothing moved. */
	static bool shift_selected(std::vector<std::string> &items, std::vector<char> &sel, int dir)
	{
		const int n = (int)items.size();
		bool changed = false;
		auto step = [&](int i, int j) { // move item i onto j when only i is selected
			if (sel[i] && !sel[j]) {
				std::swap(items[i], items[j]);
				std::swap(sel[i], sel[j]);
				changed = true;
			}
		};
		if (dir < 0)
			for (int i = 1; i < n; ++i)
				step(i, i - 1);
		else
			for (int i = n - 2; i >= 0; --i)
				step(i, i + 1);
		return changed;
	}

	/* Moves the flagged items so they start at `row` (an index into the list
	 * as it is now), keeping their order. `select` gets their new indices.
	 * Returns false if the order does not change. */
	static bool move_block(std::vector<std::string> &items, const std::vector<char> &sel, int row,
			       std::vector<int> &select)
	{
		row = std::clamp(row, 0, (int)items.size());

		std::vector<std::string> moved, rest;
		int before = 0;
		for (int i = 0; i < (int)items.size(); ++i) {
			if (i < (int)sel.size() && sel[i]) {
				moved.push_back(items[i]);
				if (i < row)
					++before;
			} else {
				rest.push_back(items[i]);
			}
		}
		const int at = row - before;
		rest.insert(rest.begin() + at, moved.begin(), moved.end());
		if (rest == items)
			return false;
		for (int i = 0; i < (int)moved.size(); ++i)
			select.push_back(at + i);
		items = std::move(rest);
		return true;
	}

	void move_selected(int dir)
	{
		ChildSelection cs;
		if (selected_children(cs)) {
			// Files of one folder: reorder them inside the folder.
			edit_folder(cs, [&](std::vector<std::string> &files, const std::vector<char> &flags,
					    std::vector<int> &select) {
				std::vector<char> sel = flags;
				if (!shift_selected(files, sel, dir))
					return false;
				for (int i = 0; i < (int)sel.size(); ++i)
					if (sel[i])
						select.push_back(i);
				return true;
			});
			return;
		}

		edit([&](std::vector<std::string> &paths, std::vector<int> &select) {
			std::vector<char> sel(paths.size(), 0);
			for (int i : selected_entries())
				if (i < (int)sel.size())
					sel[i] = 1;

			const bool changed = shift_selected(paths, sel, dir);
			for (int i = 0; i < (int)sel.size(); ++i)
				if (sel[i])
					select.push_back(i);
			return changed;
		});
	}

	/* Internal drag & drop: move all selected entries so they start at `row`. */
	void move_to_row(int row)
	{
		edit([&](std::vector<std::string> &paths, std::vector<int> &select) {
			const auto selected = selected_entries();
			if (selected.empty())
				return false;
			std::vector<char> sel(paths.size(), 0);
			for (int i : selected)
				if (i < (int)sel.size())
					sel[i] = 1;
			return move_block(paths, sel, row, select);
		});
	}

	/* Child drag & drop: move the selected files of the folder at top-level
	 * index `folder` so they start at child row `row`. */
	void move_children_to_row(int folder, int row)
	{
		ChildSelection cs;
		if (!selected_children(cs) || cs.entry != folder)
			return;
		edit_folder(cs, [&](std::vector<std::string> &files, const std::vector<char> &sel,
				    std::vector<int> &select) { return move_block(files, sel, row, select); });
	}

	/* Back to name order for the folder entry `entry`. */
	void reset_folder_order(int entry)
	{
		OBSSourceAutoRelease src = source();
		if (!src || entry < 0 || entry >= (int)snap_.entries.size() || !snap_.entries[entry].is_folder)
			return;
		playlist::set_folder_order(src, snap_.entries[entry].path, {});
	}

	/* Brings back the files removed from the folder entry `entry`. */
	void restore_folder_files(int entry)
	{
		OBSSourceAutoRelease src = source();
		if (!src || entry < 0 || entry >= (int)snap_.entries.size() || !snap_.entries[entry].is_folder)
			return;
		playlist::restore_folder_files(src, snap_.entries[entry].path);
	}

	/* Re-reads the contents of the folder entry `entry`. */
	void rescan_folder(int entry)
	{
		OBSSourceAutoRelease src = source();
		if (!src || entry < 0 || entry >= (int)snap_.entries.size() || !snap_.entries[entry].is_folder)
			return;
		playlist::rescan_folder(src, snap_.entries[entry].path);
	}

	void show_menu(const QPoint &pos)
	{
		if (weak_ == nullptr)
			return;
		QMenu menu(this);
		QTreeWidgetItem *clicked = tree_->itemAt(pos);
		int entry, file;
		QAction *play = menu.addAction(tr_key("Window.Play"), [this, clicked] { play_item(clicked); });
		play->setEnabled(play_target(clicked, entry, file));
		ChildSelection cs;
		const bool can_move = !selected_entries().empty() || selected_children(cs);
		menu.addAction(tr_key("Window.Remove"), [this] { remove_selected(); })->setEnabled(can_remove());
		menu.addAction(tr_key("Window.MoveUp"), [this] { move_selected(-1); })->setEnabled(can_move);
		menu.addAction(tr_key("Window.MoveDown"), [this] { move_selected(+1); })->setEnabled(can_move);
		// The folder of the right-clicked row (a folder row or one of its files).
		int folder = -1;
		if (clicked) {
			const int entry = clicked->data(0, kRoleEntry).toInt();
			if (entry >= 0 && entry < (int)snap_.entries.size() && snap_.entries[entry].is_folder)
				folder = entry;
		}
		if (folder >= 0) {
			menu.addAction(tr_key("Window.ResetFolderOrder"), [this, folder] { reset_folder_order(folder); })
				->setEnabled(snap_.entries[folder].custom_order);
			menu.addAction(tr_key("Window.RestoreFolderFiles"), [this, folder] { restore_folder_files(folder); })
				->setEnabled(snap_.entries[folder].excluded_count > 0);
		}
		menu.addSeparator();
		menu.addAction(tr_key("Window.AddFiles"), [this] { add_files(); });
		menu.addAction(tr_key("Window.AddFolder"), [this] { add_folder(); });
		menu.addAction(tr_key("Window.AddURL"), [this] { add_url(); });
		if (folder >= 0)
			menu.addAction(tr_key("Window.Rescan"), [this, folder] { rescan_folder(folder); });
		menu.exec(tree_->viewport()->mapToGlobal(pos));
	}
};

QPointer<PlaylistWindow> g_window;

void on_frontend_event(enum obs_frontend_event event, void *)
{
	PlaylistWindow *w = g_window;
	if (!w)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		w->rebuild_sources_soon();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		w->detach_all();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		w->shutdown();
		break;
	default:
		break;
	}
}

void show_from_menu(void *)
{
	if (g_window)
		g_window->present();
}

void open_window_for(obs_source_t *source)
{
	if (g_window)
		g_window->open_for(source);
}

} // namespace

void playlist_window_init()
{
	auto *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main_window)
		return; // loaded without the OBS frontend: the source works, there is just no window
	auto *window = new PlaylistWindow(main_window);
	g_window = window;

	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("PlaylistWindow"), window)) {
		// No dock available: fall back to a plain top-level window.
		window->setWindowFlag(Qt::Window);
	}

	obs_frontend_add_tools_menu_item(obs_module_text("PlaylistWindow"), show_from_menu, nullptr);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	playlist::set_open_window_callback(open_window_for);
}

void playlist_window_shutdown()
{
	playlist::set_open_window_callback(nullptr);
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	// The main window normally deleted the dock (and our widget) already.
	if (g_window) {
		g_window->shutdown();
		obs_frontend_remove_dock(kDockId);
	}
	g_window = nullptr;
}
