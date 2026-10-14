// joa-launcher: Qt6 Widgets launcher. Links joa_install only (no engine code). Contract: docs/LAUNCHER.md.
#include "install/install.hpp"
#include "install/settings.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <QThread>
#include <algorithm>
#include <csignal>
#include <utility>

namespace {
namespace ji = joa::install;

QString qs(const std::filesystem::path& p) { return QString::fromStdString(p.string()); }
std::filesystem::path fp(const QString& s) { return std::filesystem::path(s.toStdString()); }

// Sibling of this executable first (bundle layout), then PATH.
QString findTool(const QString& name) {
    const QString sibling = QCoreApplication::applicationDirPath() + "/" + name;
    return QFileInfo(sibling).isExecutable() ? sibling : QStandardPaths::findExecutable(name);
}

QComboBox* combo(const QList<QPair<QString, QString>>& items) { // (label, settings value)
    auto* c = new QComboBox;
    for (const auto& [label, value] : items) c->addItem(label, value);
    return c;
}
void select(QComboBox* c, const QVariant& value) {
    const int i = c->findData(value);
    c->setCurrentIndex(i < 0 ? 0 : i);
}

class Launcher : public QMainWindow {
public:
    Launcher() : paths_(ji::Paths::from_env()), settings_(ji::Settings::load(paths_.settings())) {
        setWindowTitle("Wars and Warriors: Joan of Arc — Native Launcher");
        auto* tabs = new QTabWidget;
        tabs->addTab(gameTab(), "Game");
        tabs->addTab(graphicsTab(), "Graphics");
        tabs->addTab(controllerTab(), "Controller");
        tabs->addTab(updatesTab(), "Updates");
        tabs_ = tabs;

        status_ = new QLabel;
        status_->setWordWrap(true);
        progress_ = new QProgressBar;
        progress_->setRange(0, 100);
        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setMaximumBlockCount(2000);
        log_->setMaximumHeight(150);
        auto* central = new QWidget;
        auto* layout = new QVBoxLayout(central);
        layout->addWidget(status_);
        layout->addWidget(tabs, 1);
        layout->addWidget(progress_);
        layout->addWidget(log_);
        setCentralWidget(central);
        resize(820, 560);

        loadSettingsIntoWidgets();
        connectSettings(); // after loading, so populating widgets never writes the file
        setBusy(false);    // applies enable/disable rules for the initial state
        updater_.setProcessChannelMode(QProcess::SeparateChannels);
        connect(&updater_, &QProcess::readyReadStandardError, this, [this] { consumeUpdaterStderr(); });
        connect(&updater_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) { updaterFinished(code, status); });
        connect(&updater_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) { append("joa-updater failed to start: " + updater_.errorString()); setBusy(false); }
        });
    }
    QTabWidget* tabs() const { return tabs_; }
    // Play button / `--play [-- ENGINE_ARGS]`: start joan-native detached; its pid, or 0 if it did not start.
    qint64 play(const QStringList& extra = {}) {
        const QString exe = findTool("joan-native");
        if (exe.isEmpty()) {
            QMessageBox::warning(this, "Game not found", "joan-native was not found next to the launcher or on PATH.");
            return 0;
        }
        saveSettings();
        QDir().mkpath(qs(paths_.logs()));
        const QString log = qs(paths_.logs()) + "/game-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'") + ".log";
        QProcess p;
        p.setProgram(exe);
        p.setArguments(QStringList{"--game-dir", qs(gameDir()), "--settings", qs(paths_.settings())} + extra);
        p.setWorkingDirectory(qs(gameDir()));
        p.setStandardOutputFile(log, QIODevice::Append);
        p.setStandardErrorFile(log, QIODevice::Append);
        qint64 pid = 0;
        const bool started = p.startDetached(&pid);
        append(started ? "Started joan-native; log: " + log : "Cannot start joan-native: " + p.errorString());
        return started ? pid : 0;
    }

private:
    ji::Paths paths_;
    ji::Settings settings_;
    QTabWidget* tabs_ = nullptr;
    QLabel *status_ = nullptr, *updateStatus_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QList<QWidget*> actions_; // disabled while a job runs
    QPushButton *play_ = nullptr, *verify_ = nullptr, *repair_ = nullptr, *applyGame_ = nullptr, *applyLauncher_ = nullptr;
    QCheckBox* wine_ = nullptr;
    QComboBox *backend_ = nullptr, *preset_ = nullptr, *resolution_ = nullptr, *aspect_ = nullptr, *mode_ = nullptr, *channel_ = nullptr;
    QCheckBox *rtReflections_ = nullptr, *rtShadows_ = nullptr, *rtGi_ = nullptr;
    QDoubleSpinBox *deadzone_ = nullptr, *sensitivity_ = nullptr;
    QTextBrowser* notes_ = nullptr;
    QProcess updater_;
    QString updaterCommand_;
    QByteArray updaterStderr_;

    QPushButton* button(const QString& text, void (Launcher::*slot)()) {
        auto* b = new QPushButton(text);
        connect(b, &QPushButton::clicked, this, slot);
        actions_.push_back(b);
        return b;
    }

    QWidget* gameTab() {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        play_ = button("Play", &Launcher::playClicked);
        play_->setMinimumHeight(56);
        QFont f = play_->font();
        f.setPointSizeF(f.pointSizeF() * 1.5);
        f.setBold(true);
        play_->setFont(f);
        v->addWidget(play_);

        auto* install = new QGroupBox("Install / Import Original Game");
        auto* iv = new QVBoxLayout(install);
        iv->addWidget(new QLabel("Your own Windows copy is required. Files are copied into the native data folder; "
                                 "the original installation is never modified."));
        auto* row = new QHBoxLayout;
        row->addWidget(button("Import Game Folder…", &Launcher::importFolder));
        row->addWidget(button("Install from GOG Installer…", &Launcher::importInstaller));
        iv->addLayout(row);
        wine_ = new QCheckBox("Allow Wine-assisted install if innoextract is unavailable (opt-in; runs the original installer in a private prefix)");
        actions_.push_back(wine_);
        iv->addWidget(wine_);
        v->addWidget(install);

        auto* maint = new QGroupBox("Maintenance");
        auto* mv = new QHBoxLayout(maint);
        verify_ = button("Verify Files", &Launcher::verify);
        repair_ = button("Repair…", &Launcher::repair);
        mv->addWidget(verify_);
        mv->addWidget(repair_);
        auto* logs = new QPushButton("Open Logs");
        connect(logs, &QPushButton::clicked, this, [this] {
            QDir().mkpath(qs(paths_.logs()));
            QDesktopServices::openUrl(QUrl::fromLocalFile(qs(paths_.logs())));
        });
        mv->addWidget(logs);
        v->addWidget(maint);
        v->addStretch();
        return w;
    }

    QWidget* graphicsTab() {
        auto* w = new QWidget;
        auto* form = new QFormLayout(w);
        backend_ = combo({{"OpenGL", "opengl"}, {"Vulkan", "vulkan"}});
        preset_ = combo({{"Original", "original"}, {"Enhanced", "enhanced"}, {"Ultra", "ultra"}, {"Custom", "custom"}});
        form->addRow("Renderer", backend_);
        form->addRow("Graphics preset", preset_);
        auto* rt = new QGroupBox("Ray tracing (Vulkan only)");
        auto* rv = new QVBoxLayout(rt);
        rtReflections_ = new QCheckBox("Reflections");
        rtShadows_ = new QCheckBox("Shadows");
        rtGi_ = new QCheckBox("Global illumination");
        for (auto* c : {rtReflections_, rtShadows_, rtGi_}) rv->addWidget(c);
        form->addRow(rt);
        QList<QPair<QString, QString>> res = {{"Desktop", "0x0"}};
        QSize screen = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->size() : QSize();
        for (const QSize s : {QSize(1280, 720), QSize(1600, 900), QSize(1920, 1080), QSize(2560, 1440), QSize(3440, 1440), QSize(3840, 2160), screen})
            if (s.isValid() && !s.isEmpty()) {
                const QString key = QString("%1x%2").arg(s.width()).arg(s.height());
                if (std::none_of(res.begin(), res.end(), [&](const auto& p) { return p.second == key; })) res.push_back({key, key});
            }
        resolution_ = combo(res);
        aspect_ = combo({{"Automatic", "auto"}, {"4:3", "4:3"}, {"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"}});
        mode_ = combo({{"Fullscreen", "fullscreen"}, {"Borderless window", "borderless"}, {"Windowed", "windowed"}});
        form->addRow("Resolution", resolution_);
        form->addRow("Aspect ratio", aspect_);
        form->addRow("Display mode", mode_);
        return w;
    }

    QWidget* controllerTab() {
        auto* w = new QWidget;
        auto* form = new QFormLayout(w);
        deadzone_ = new QDoubleSpinBox;
        deadzone_->setRange(0.0, 0.9);
        deadzone_->setSingleStep(0.05);
        sensitivity_ = new QDoubleSpinBox;
        sensitivity_->setRange(0.1, 5.0);
        sensitivity_->setSingleStep(0.1);
        form->addRow("Stick deadzone", deadzone_);
        form->addRow("Stick sensitivity", sensitivity_);
        return w;
    }

    QWidget* updatesTab() {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        auto* form = new QFormLayout;
        channel_ = combo({{"Stable", "stable"}, {"Experimental", "experimental"}});
        form->addRow("Update channel", channel_);
        v->addLayout(form);
        updateStatus_ = new QLabel("Press “Check for Updates”.");
        v->addWidget(updateStatus_);
        auto* row = new QHBoxLayout;
        row->addWidget(button("Check for Updates", &Launcher::checkUpdates));
        applyGame_ = button("Update Game", &Launcher::updateGame);
        applyLauncher_ = button("Update Launcher", &Launcher::updateLauncher);
        row->addWidget(applyGame_);
        row->addWidget(applyLauncher_);
        row->addWidget(button("Roll Back Update", &Launcher::rollback));
        v->addLayout(row);
        v->addWidget(new QLabel("Release notes"));
        notes_ = new QTextBrowser;
        notes_->setOpenExternalLinks(true);
        v->addWidget(notes_, 1);
        return w;
    }

    void loadSettingsIntoWidgets() {
        const auto& s = settings_;
        select(backend_, QString::fromStdString(s.backend));
        select(preset_, QString::fromStdString(s.preset));
        rtReflections_->setChecked(s.rt_reflections);
        rtShadows_->setChecked(s.rt_shadows);
        rtGi_->setChecked(s.rt_gi);
        const QString res = QString("%1x%2").arg(s.width).arg(s.height);
        if (resolution_->findData(res) < 0) resolution_->addItem(res, res);
        select(resolution_, res);
        select(aspect_, QString::fromStdString(s.aspect));
        select(mode_, QString::fromStdString(s.mode));
        deadzone_->setValue(s.deadzone);
        sensitivity_->setValue(s.sensitivity);
        select(channel_, QString::fromStdString(s.channel));
        syncRayTracing();
    }

    void syncRayTracing() {
        const bool vulkan = backend_->currentData().toString() == "vulkan";
        for (auto* c : {rtReflections_, rtShadows_, rtGi_}) c->setEnabled(vulkan);
    }

    void connectSettings() {
        for (auto* c : {backend_, preset_, resolution_, aspect_, mode_, channel_})
            connect(c, &QComboBox::currentIndexChanged, this, [this] { saveSettings(); });
        for (auto* c : {rtReflections_, rtShadows_, rtGi_}) connect(c, &QCheckBox::toggled, this, [this] { saveSettings(); });
        for (auto* d : {deadzone_, sensitivity_}) connect(d, &QDoubleSpinBox::valueChanged, this, [this] { saveSettings(); });
    }

    void saveSettings() {
        auto& s = settings_;
        s.backend = backend_->currentData().toString().toStdString();
        s.preset = preset_->currentData().toString().toStdString();
        s.rt_reflections = rtReflections_->isChecked();
        s.rt_shadows = rtShadows_->isChecked();
        s.rt_gi = rtGi_->isChecked();
        const auto wh = resolution_->currentData().toString().split('x');
        s.width = wh.value(0).toInt();
        s.height = wh.value(1).toInt();
        s.aspect = aspect_->currentData().toString().toStdString();
        s.mode = mode_->currentData().toString().toStdString();
        s.deadzone = deadzone_->value();
        s.sensitivity = sensitivity_->value();
        s.channel = channel_->currentData().toString().toStdString();
        syncRayTracing();
        try { s.save(paths_.settings()); } catch (const std::exception& e) { append(QString("Cannot save settings: ") + e.what()); }
    }

    QJsonObject marker() const {
        QFile f(qs(paths_.current() / ".joa-install.json"));
        return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    }

    std::filesystem::path gameDir() const {
        return settings_.game_dir.empty() ? paths_.current() : std::filesystem::path(settings_.game_dir);
    }

    void refreshStatus() {
        const auto m = marker();
        const bool installed = !m.isEmpty();
        status_->setText(installed ? QString("<b>Installed:</b> %1<br><small>%2</small>").arg(m["label"].toString(), qs(paths_.current()))
                                   : QString("<b>Original game not imported yet.</b> Import your game folder or GOG installer below."));
        play_->setEnabled(installed || !settings_.game_dir.empty());
        verify_->setEnabled(installed);
        repair_->setEnabled(installed);
    }

    void append(const QString& line) { log_->appendPlainText(line); }

    void setBusy(bool busy) {
        for (auto* w : actions_) w->setEnabled(!busy);
        if (!busy) { refreshStatus(); applyGame_->setEnabled(false); applyLauncher_->setEnabled(false); }
    }

    ji::Options jobOptions() {
        ji::Options opt;
        opt.use_wine = wine_->isChecked();
        opt.launcher_exec = fp(QCoreApplication::applicationFilePath());
        if (qEnvironmentVariableIsSet("APPIMAGE")) opt.launcher_exec = fp(qEnvironmentVariable("APPIMAGE"));
        opt.progress = [this](int percent, const std::string& message) {
            QMetaObject::invokeMethod(this, [this, percent, msg = QString::fromStdString(message)] {
                progress_->setValue(percent);
                progress_->setFormat(QString("%p% — ") + msg);
            }, Qt::QueuedConnection);
        };
        return opt;
    }

    // Runs an install-library job off the GUI thread; result text lands in the log.
    void runJob(const QString& title, std::function<QString()> job) {
        setBusy(true);
        append(title + "…");
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
            append(watcher->result());
            watcher->deleteLater();
            setBusy(false);
        });
        watcher->setFuture(QtConcurrent::run([job] {
            try { return job(); } catch (const std::exception& e) { return QString("Failed: ") + e.what(); }
        }));
    }

    static QString describe(const ji::VerifyReport& r) {
        if (r.ok()) return QString("All %1 files verified.").arg(r.checked);
        QString out = QString("%1 missing, %2 damaged of %3 files.").arg(r.missing.size()).arg(r.mismatched.size()).arg(r.checked);
        for (const auto& f : r.missing) out += "\n  missing: " + QString::fromStdString(f);
        for (const auto& f : r.mismatched) out += "\n  damaged: " + QString::fromStdString(f);
        return out;
    }

    void startImport(const QString& source) {
        if (source.isEmpty()) return;
        const auto d = ji::detect(fp(source));
        for (const auto& n : d.notes) append(QString::fromStdString(n));
        if (d.kind == ji::SourceKind::InstalledDir && !d.version) {
            QMessageBox::warning(this, "Unknown version", "This joan.exe (sha256 " + QString::fromStdString(d.exe_sha256) +
                                 ") is not a known release. Please report it so it can be added.");
            return;
        }
        auto opt = jobOptions();
        const auto paths = paths_;
        runJob("Importing " + source, [opt, paths, source] {
            return "Installed into " + qs(ji::import_game(fp(source), paths, opt));
        });
    }

    void importFolder() { startImport(QFileDialog::getExistingDirectory(this, "Select the original game folder (contains joan.exe)")); }
    void importInstaller() {
        startImport(QFileDialog::getOpenFileName(this, "Select the GOG offline installer", QDir::homePath(),
                                                 "GOG installer (setup_wars_and_warriors_joan_of_arc*.exe);;Windows executables (*.exe)"));
    }

    void verify() {
        auto opt = jobOptions();
        const auto paths = paths_;
        runJob("Verifying files", [opt, paths] { return describe(ji::verify_installed(paths, opt)); });
    }

    void repair() {
        // Default to the folder the game was imported from; ask if it is gone.
        QString source = marker()["source"].toString();
        if (!QFileInfo::exists(source))
            source = QFileDialog::getExistingDirectory(this, "Select an original game folder to repair from");
        if (source.isEmpty()) return;
        auto opt = jobOptions();
        const auto paths = paths_;
        runJob("Repairing from " + source, [opt, paths, source] { return describe(ji::repair(fp(source), paths, opt)); });
    }

    void playClicked() { play(); }

    // ---- joa-updater (contract in docs/LAUNCHER.md) ----
    void runUpdater(const QString& command) {
        const QString exe = findTool("joa-updater");
        if (exe.isEmpty()) { append("joa-updater was not found next to the launcher or on PATH."); return; }
        QStringList args{command};
        if (command == "check" || command == "apply") args << "--channel" << channel_->currentData().toString();
        args << "--json";
        updaterCommand_ = command;
        updaterStderr_.clear();
        setBusy(true);
        append("joa-updater " + args.join(' '));
        updater_.start(exe, args);
    }
    void checkUpdates() { runUpdater("check"); }
    void updateGame() { runUpdater("apply"); }
    void updateLauncher() { relaunchAfterApply_ = true; runUpdater("apply"); }
    void rollback() { runUpdater("rollback"); }
    bool relaunchAfterApply_ = false;

    void consumeUpdaterStderr() {
        updaterStderr_ += updater_.readAllStandardError();
        qsizetype nl;
        while ((nl = updaterStderr_.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(updaterStderr_.left(nl)).trimmed();
            updaterStderr_.remove(0, nl + 1);
            if (line.startsWith("progress ")) {
                const QString rest = line.mid(9);
                progress_->setValue(rest.section(' ', 0, 0).toInt());
                progress_->setFormat(QString("%p% — ") + rest.section(' ', 1));
            } else if (!line.isEmpty()) {
                append(line);
            }
        }
    }

    void updaterFinished(int code, QProcess::ExitStatus status) {
        consumeUpdaterStderr();
        const auto obj = QJsonDocument::fromJson(updater_.readAllStandardOutput()).object();
        const bool relaunch = std::exchange(relaunchAfterApply_, false);
        setBusy(false);
        if (status != QProcess::NormalExit || code != 0) {
            append("joa-updater failed: " + (obj["error"].toString().isEmpty() ? QString("exit code %1").arg(code) : obj["error"].toString()));
            return;
        }
        if (updaterCommand_ == "check") {
            const bool available = obj["available"].toBool();
            updateStatus_->setText(QString("Installed %1 — latest %2 %3").arg(obj["current"].toString(), obj["latest"].toString(),
                                                                            available ? "(update available)" : "(up to date)"));
            notes_->setMarkdown(obj["notes"].toString());
            applyGame_->setEnabled(available);
            applyLauncher_->setEnabled(available);
        } else {
            append("joa-updater " + updaterCommand_ + ": done.");
            if (relaunch) {
                // joa-updater swaps $XDG_DATA_HOME/joan-native/app/current; prefer the new launcher there.
                QString self = qs(paths_.game_root.parent_path() / "app/current/bin/joa-launcher");
                if (!QFileInfo(self).isExecutable())
                    self = qEnvironmentVariableIsSet("APPIMAGE") ? qEnvironmentVariable("APPIMAGE") : findTool("joa-launcher");
                if (QProcess::startDetached(self, {})) QCoreApplication::quit();
                else append("Restart the launcher to use the new version.");
            }
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("joa-launcher");
    QGuiApplication::setDesktopFileName("joan-native"); // Wayland app-id = desktop entry id
    const QStringList args = app.arguments();
    Launcher window;
    // --play [-- ENGINE_ARGS]: start the game exactly like the Play button (desktop action), then stay
    // until it exits: inside an AppImage the game's files vanish when the launcher process ends.
    if (const int play = args.indexOf("--play"); play > 0) {
        const int sep = args.indexOf("--");
        const qint64 pid = window.play(sep > play ? args.mid(sep + 1) : QStringList{});
        while (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0) QThread::msleep(200);
        return pid > 0 ? 0 : 1;
    }
    window.show();
    // --screenshot FILE [--tab N]: render the window offscreen (QWidget::grab), save, exit.
    const int shot = args.indexOf("--screenshot");
    if (shot > 0) {
        const QString file = args.value(shot + 1);
        const int tab = args.indexOf("--tab");
        if (tab > 0) window.tabs()->setCurrentIndex(args.value(tab + 1).toInt());
        QTimer::singleShot(200, &window, [&window, file] {
            QCoreApplication::exit(!file.isEmpty() && window.grab().save(file) ? 0 : 1);
        });
    }
    return app.exec();
}
