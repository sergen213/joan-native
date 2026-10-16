// joa-launcher: Qt6 Widgets launcher. Links joa_install and joa_update (no engine code). Contract: docs/LAUNCHER.md.
#include "install/install.hpp"
#include "install/settings.hpp"
#include "update.hpp"

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
#include <QScrollArea>
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
#include <unistd.h>
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

// [graphics] keys other than the image-quality ones live in Settings::graphics_extra; "" = key unset.
std::string extraValue(const ji::Settings& s, const std::string& key) {
    std::string value;
    for (const auto& [k, v] : s.graphics_extra) if (k == key) value = v; // last one wins, like the engine
    return value;
}
void setExtra(ji::Settings& s, const std::string& key, const std::string& value) {
    std::erase_if(s.graphics_extra, [&](const auto& kv) { return kv.first == key; });
    if (!value.empty()) s.graphics_extra.emplace_back(key, value);
}

class Launcher : public QMainWindow {
public:
    Launcher() : paths_(ji::Paths::from_env()), settings_(ji::Settings::load(paths_.settings())) {
        setWindowTitle("Wars and Warriors: Joan of Arc — Native Launcher");
        auto* tabs = new QTabWidget;
        tabs->addTab(gameTab(), "Game");
        auto* graphics = new QScrollArea;  // the Custom group makes this tab taller than a 768 px screen
        graphics->setWidget(graphicsTab());
        graphics->setWidgetResizable(true);
        graphics->setFrameShape(QFrame::NoFrame);
        tabs->addTab(graphics, "Graphics");
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
    QPushButton *play_ = nullptr, *verify_ = nullptr, *repair_ = nullptr, *apply_ = nullptr;
    QCheckBox* wine_ = nullptr;
    QComboBox *backend_ = nullptr, *preset_ = nullptr, *resolution_ = nullptr, *aspect_ = nullptr, *mode_ = nullptr, *channel_ = nullptr;
    QCheckBox *rtReflections_ = nullptr, *rtShadows_ = nullptr, *rtGi_ = nullptr;
    QComboBox *aa_ = nullptr, *msaa_ = nullptr, *anisotropy_ = nullptr, *renderScale_ = nullptr, *sharpness_ = nullptr;
    QGroupBox* custom_ = nullptr;
    std::vector<std::pair<QComboBox*, std::string>> customKeys_; // Custom preset combos -> [graphics] key
    std::vector<std::pair<QComboBox*, std::string>> qualityKeys_; // image-quality combos kept in graphics_extra (any preset)
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
        // Custom preset look keys (read by the engine only with preset=custom); "Default (Enhanced)" = key unset.
        custom_ = new QGroupBox("Custom preset options");
        auto* cf = new QFormLayout(custom_);
        const QList<QPair<QString, QString>> onOff = {{"Default (Enhanced)", ""}, {"On", "true"}, {"Off", "false"}};
        for (const auto& [label, key] : {std::pair{"PBR materials", "pbr"}, {"Normal maps (HD packs)", "normal_maps"},
                                         {"Image-based lighting", "ibl"}, {"HDR + tone mapping", "hdr"},
                                         {"Ambient occlusion (SSAO)", "ssao"}, {"Volumetric fog", "volumetric_fog"},
                                         {"Lit particles", "particle_lighting"}}) {
            customKeys_.emplace_back(combo(onOff), key);
            cf->addRow(label, customKeys_.back().first);
        }
        customKeys_.emplace_back(combo({{"Default (Enhanced)", ""}, {"Original stencil shadows", "0"}, {"Shadow map 1024", "1024"},
                                        {"2048", "2048"}, {"4096", "4096"}, {"8192", "8192"}}), "shadow_map_size");
        cf->addRow("Shadows", customKeys_.back().first);
        form->addRow(custom_);
        auto* rt = new QGroupBox("Ray tracing (Vulkan only)");
        auto* rv = new QVBoxLayout(rt);
        rtReflections_ = new QCheckBox("Reflections");
        rtShadows_ = new QCheckBox("Shadows");
        rtGi_ = new QCheckBox("Global illumination");
        for (auto* c : {rtReflections_, rtShadows_, rtGi_}) rv->addWidget(c);
        form->addRow(rt);
        // [graphics] image-quality keys: work with every preset (Original keeps its look); "Preset default" = key unset.
        auto* quality = new QGroupBox("Anti-aliasing and image quality (all presets)");
        auto* qf = new QFormLayout(quality);
        aa_ = combo({{"Preset default", ""}, {"Off", "none"}, {"FXAA", "fxaa"}, {"SMAA", "smaa"}, {"TAA", "taa"}});
        aa_->setToolTip("Post-process anti-aliasing. TAA needs Enhanced/Ultra/Custom (motion vectors); with Original it runs as SMAA.");
        msaa_ = combo({{"Preset default", ""}, {"Off", "1"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}});
        msaa_->setToolTip("Multisampled geometry edges, alpha-to-coverage for foliage, grass and hair.");
        anisotropy_ = combo({{"Preset default", ""}, {"Off", "1"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}, {"16x", "16"}});
        renderScale_ = combo({{"Preset default", ""}, {"50%", "0.5"}, {"75%", "0.75"}, {"100% (native)", "1"},
                              {"150% (supersampling)", "1.5"}, {"200% (supersampling)", "2"}});
        sharpness_ = combo({{"Preset default", ""}, {"Off", "0"}, {"Low", "0.25"}, {"Medium", "0.5"}, {"High", "0.8"}});
        qf->addRow("Anti-aliasing", aa_);
        qf->addRow("MSAA", msaa_);
        qf->addRow("Anisotropic filtering", anisotropy_);
        qf->addRow("Render scale", renderScale_);
        // AMD FSR (upscale / fsr_quality / frame_generation keys, kept in graphics_extra): FSR 3 replaces TAA and the render
        // scale on Vulkan; elsewhere it runs as FSR 1 at the same scale.
        auto* upscale = combo({{"Preset default", ""}, {"Off", "none"}, {"AMD FSR 1", "fsr1"}, {"AMD FSR 3", "fsr3"}});
        upscale->setToolTip("FSR 1: spatial upscale from the render scale. FSR 3: temporal upscaling + anti-aliasing (Vulkan, "
                            "Enhanced/Ultra/Custom; Ultra uses it by default). OpenGL runs FSR 3 as FSR 1.");
        auto* fsrQuality = combo({{"Preset default", ""}, {"Native AA (100%)", "native"}, {"Quality (67%)", "quality"},
                                  {"Balanced (59%)", "balanced"}, {"Performance (50%)", "performance"},
                                  {"Ultra performance (33%)", "ultra_performance"}});
        fsrQuality->setToolTip("FSR 3 render resolution per axis; it sets the render scale.");
        auto* frameGeneration = combo({{"Preset default (off)", ""}, {"Off", "false"}, {"On", "true"}});
        frameGeneration->setToolTip("FSR 3 frame generation (Vulkan, needs FSR 3): an interpolated frame between rendered frames; "
                                    "with vsync the game renders at half the refresh rate and shows every refresh.");
        for (auto [c, key] : {std::pair{upscale, "upscale"}, {fsrQuality, "fsr_quality"}, {frameGeneration, "frame_generation"}})
            qualityKeys_.emplace_back(c, key);
        qf->addRow("Upscaler", upscale);
        qf->addRow("FSR 3 quality", fsrQuality);
        qf->addRow("Frame generation", frameGeneration);
        qf->addRow("Sharpening", sharpness_);
        form->addRow(quality);
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
        apply_ = button("Update", &Launcher::update);
        row->addWidget(apply_);
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
        for (auto [c, v] : {std::pair{aa_, &s.aa}, {msaa_, &s.msaa}, {anisotropy_, &s.anisotropy}, {renderScale_, &s.render_scale},
                            {sharpness_, &s.sharpness}}) {
            const QString value = QString::fromStdString(*v);
            if (c->findData(value) < 0) c->addItem(value, value);  // a hand-edited value stays selectable
            select(c, value);
        }
        for (const auto* keys : {&customKeys_, &qualityKeys_})
            for (const auto& [c, key] : *keys) {
                const QString value = QString::fromStdString(extraValue(s, key));
                if (c->findData(value) < 0) c->addItem(value, value);
                select(c, value);
            }
        const QString res = QString("%1x%2").arg(s.width).arg(s.height);
        if (resolution_->findData(res) < 0) resolution_->addItem(res, res);
        select(resolution_, res);
        select(aspect_, QString::fromStdString(s.aspect));
        select(mode_, QString::fromStdString(s.mode));
        deadzone_->setValue(s.deadzone);
        sensitivity_->setValue(s.sensitivity);
        select(channel_, QString::fromStdString(s.channel));
        syncGraphics();
    }

    void syncGraphics() {
        const bool vulkan = backend_->currentData().toString() == "vulkan";
        for (auto* c : {rtReflections_, rtShadows_, rtGi_}) c->setEnabled(vulkan);
        qualityKeys_[2].first->setEnabled(vulkan);  // frame generation is Vulkan-only
        custom_->setVisible(preset_->currentData().toString() == "custom");
    }

    void connectSettings() {
        for (auto* c : {backend_, preset_, resolution_, aspect_, mode_, channel_, aa_, msaa_, anisotropy_, renderScale_, sharpness_})
            connect(c, &QComboBox::currentIndexChanged, this, [this] { saveSettings(); });
        for (auto* c : {rtReflections_, rtShadows_, rtGi_}) connect(c, &QCheckBox::toggled, this, [this] { saveSettings(); });
        for (const auto* keys : {&customKeys_, &qualityKeys_})
            for (const auto& [c, key] : *keys) connect(c, &QComboBox::currentIndexChanged, this, [this] { saveSettings(); });
        for (auto* d : {deadzone_, sensitivity_}) connect(d, &QDoubleSpinBox::valueChanged, this, [this] { saveSettings(); });
    }

    void saveSettings() {
        auto& s = settings_;
        s.backend = backend_->currentData().toString().toStdString();
        s.preset = preset_->currentData().toString().toStdString();
        s.rt_reflections = rtReflections_->isChecked();
        s.rt_shadows = rtShadows_->isChecked();
        s.rt_gi = rtGi_->isChecked();
        s.aa = aa_->currentData().toString().toStdString();
        s.msaa = msaa_->currentData().toString().toStdString();
        s.anisotropy = anisotropy_->currentData().toString().toStdString();
        s.render_scale = renderScale_->currentData().toString().toStdString();
        s.sharpness = sharpness_->currentData().toString().toStdString();
        // Custom keys are kept with every preset (the engine reads them only with custom), so switching presets loses
        // nothing; the FSR keys apply to every preset.
        for (const auto* keys : {&customKeys_, &qualityKeys_})
            for (const auto& [c, key] : *keys) setExtra(s, key, c->currentData().toString().toStdString());
        const auto wh = resolution_->currentData().toString().split('x');
        s.width = wh.value(0).toInt();
        s.height = wh.value(1).toInt();
        s.aspect = aspect_->currentData().toString().toStdString();
        s.mode = mode_->currentData().toString().toStdString();
        s.deadzone = deadzone_->value();
        s.sensitivity = sensitivity_->value();
        s.channel = channel_->currentData().toString().toStdString();
        syncGraphics();
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
        if (!busy) { refreshStatus(); apply_->setEnabled(false); }
    }

    ji::Options jobOptions() {
        ji::Options opt;
        opt.use_wine = wine_->isChecked();
        // Desktop entry target: app/current survives updates (version dirs get pruned), then the AppImage, then this exe.
        opt.launcher_exec = fp(QCoreApplication::applicationFilePath());
        if (qEnvironmentVariableIsSet("APPIMAGE")) opt.launcher_exec = fp(qEnvironmentVariable("APPIMAGE"));
        if (QFileInfo(installedLauncher()).isExecutable()) opt.launcher_exec = fp(installedLauncher());
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
    void update() { runUpdater("apply"); }
    void rollback() { runUpdater("rollback"); }
    QString installedLauncher() const { return qs(paths_.game_root.parent_path() / "app/current/bin/joa-launcher"); }

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
            apply_->setEnabled(available);
        } else {
            append("joa-updater " + updaterCommand_ + ": done.");
            // apply/rollback swapped app/current (game and launcher ship together): restart into it.
            if (obj["from"].toString() != obj["to"].toString()) {
                QString self = installedLauncher();
                if (!QFileInfo(self).isExecutable())
                    self = qEnvironmentVariableIsSet("APPIMAGE") ? qEnvironmentVariable("APPIMAGE") : findTool("joa-launcher");
                if (QProcess::startDetached(self, {})) QCoreApplication::quit();
                else append("Restart the launcher to use the new version.");
            }
        }
    }
};

// An old AppImage keeps running its bundled binaries, so updates joa-updater installed into
// $XDG_DATA_HOME/joan-native/app/current would never run: exec that launcher when it is newer.
// It sees an equal version and stays (APPIMAGE remains set for relaunch/desktop-entry logic).
void handOffToInstalledLauncher(char** argv) {
    if (!std::getenv("APPIMAGE")) return;
    try {
        const auto app = ji::Paths::from_env().game_root.parent_path() / "app";
        const auto exe = app / "current/bin/joa-launcher";
        if (::access(exe.c_str(), X_OK) != 0 || joa::update::compare_versions(joa::update::active_version(app), JOA_VERSION) <= 0) return;
        char* const self = std::exchange(argv[0], const_cast<char*>(exe.c_str()));
        ::execv(exe.c_str(), argv);
        argv[0] = self; // exec failed: carry on with this launcher
    } catch (const std::exception&) {} // no/invalid installed version
}
} // namespace

int main(int argc, char** argv) {
    handOffToInstalledLauncher(argv);
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
