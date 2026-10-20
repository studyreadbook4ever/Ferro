#include "window.h"
#include "dsp.h"
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTest>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sndfile.h>

namespace fc {
namespace {
constexpr std::array<std::optional<double> NoteOverrides::*, 5> noteOverrideFields = {
    &NoteOverrides::level, &NoteOverrides::pan, &NoteOverrides::tone, &NoteOverrides::decay,
    &NoteOverrides::drive};
constexpr std::array<double SoundSettings::*, 5> noteSettingFields = {
    &SoundSettings::level, &SoundSettings::pan, &SoundSettings::tone, &SoundSettings::decay,
    &SoundSettings::drive};
QPushButton* button(const QString& text, const QString& tip = {}) {
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}
QLabel* caption(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("caption", true);
    return l;
}
QString timecode(uint64_t frames) {
    uint64_t ms = frames * 1000 / SampleRate;
    return QString("%1:%2.%3")
        .arg(ms / 60000, 2, 10, QChar('0'))
        .arg(ms / 1000 % 60, 2, 10, QChar('0'))
        .arg(ms % 1000, 3, 10, QChar('0'));
}
bool prepareSamplePitches(Track& track) {
    auto meaningful = [](const Note& note) {
        return note.velocity > 0 ||
               std::any_of(noteOverrideFields.begin(), noteOverrideFields.end(),
                           [&note](auto field) { return (note.overrides.*field).has_value(); });
    };
    for (const auto& pattern : track.patterns)
        for (const auto& step : pattern.steps)
            if (std::count_if(step.notes.begin(), step.notes.end(), meaningful) > 1)
                return true;
    for (auto& pattern : track.patterns)
        for (auto& step : pattern.steps) {
            auto meaningfulNote = std::find_if(step.notes.begin(), step.notes.end(), meaningful);
            if (meaningfulNote == step.notes.end())
                step.notes.clear();
            else {
                Note retuned = *meaningfulNote;
                retuned.note = 60;
                step.notes = {retuned};
            }
        }
    return false;
}
} // namespace
Window::Window(bool mode) : supervisor(this), smoke(mode) {
    setObjectName("ferroWindow");
    resize(1440, 1040);
    setMinimumSize(1040, 720);
    autosavePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/autosave.ferro";
    build();
    audio.setGain(1);
    renderTimer = new QTimer(this);
    renderTimer->setSingleShot(true);
    renderTimer->setInterval(180);
    connect(renderTimer, &QTimer::timeout, this, [this] {
        renderState->setText("RENDERING SONG");
        supervisor.render(project);
    });
    connect(&supervisor, &Supervisor::mixReady, this,
            [this](const std::vector<float>& data, const Project& p) {
                if (projectToJson(p) != projectToJson(project))
                    return;
                try {
                    audioRevision = audio.submit(data, p.bpm);
                    preparedState = audioRevision ? projectToJson(p) : QJsonObject();
                } catch (const std::exception& e) {
                    renderState->setText("AUDIO BUFFER ERROR");
                    report(QString::fromUtf8(e.what()));
                    return;
                }
                renderState->setText("READY · 48 kHz / stereo");
            });
    connect(&supervisor, &Supervisor::errorOccurred, this, [this](const QString& text) {
        renderState->setText("RENDER ERROR");
        report(text);
    });
    auto* timer = new QTimer(this);
    timer->setInterval(33);
    connect(timer, &QTimer::timeout, this, [this] { tick(); });
    timer->start();
    auto* autosave = new QTimer(this);
    autosave->setInterval(30000);
    connect(autosave, &QTimer::timeout, this, [this] {
        if (dirty) {
            QDir().mkpath(QFileInfo(autosavePath).absolutePath());
            QString e;
            if (!saveProject(autosavePath, project, e))
                report("Autosave failed: " + e);
        }
    });
    autosave->start();
    resetHistory();
    refreshProject();
    if (!smoke && QFileInfo::exists(autosavePath))
        QTimer::singleShot(0, this, [this] {
            if (QMessageBox::question(this, "Recover song", "An autosaved song is available. Recover it?") ==
                QMessageBox::Yes) {
                Project p;
                QString e;
                if (loadProject(autosavePath, p, e))
                    recoverSession(std::move(p));
                else
                    report(e);
            }
        });
    renderTimer->start();
}
Window::~Window() {
    audio.close();
    supervisor.stop();
}
void Window::build() {
    setStyleSheet(R"(
        QMainWindow,QWidget{background:#1c2223;color:#d8dfd5;font-family:"Noto Sans","DejaVu Sans",sans-serif;font-size:12px;}
        QMenuBar{background:#171d1e;border-bottom:1px solid #38433c;}QMenuBar::item{padding:5px 12px;}QMenuBar::item:selected,QMenu::item:selected{background:#43564a;}QMenu{border:1px solid #57665a;}
        QLabel[caption="true"]{color:#93a396;font-size:10px;letter-spacing:.7px;}
        QPushButton{background:#303b37;border:1px solid #4b5c50;border-radius:3px;padding:6px 11px;}QPushButton:hover{background:#405247;border-color:#8a9c89;}QPushButton:checked{background:#b69b72;color:#18221b;border-color:#d2b787;}QPushButton:disabled{color:#6b7b6e;}
        QLineEdit,QSpinBox,QDoubleSpinBox,QComboBox{background:#161e1b;border:1px solid #445348;border-radius:3px;padding:5px;selection-background-color:#577962;}
        QGroupBox{border:1px solid #3f4d43;border-radius:3px;margin-top:17px;padding:9px;}QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 4px;color:#aebbac;}
        QSlider::groove:horizontal{height:4px;background:#121a15;border-radius:2px;}QSlider::sub-page:horizontal{background:#819b84;border-radius:2px;}QSlider::handle:horizontal{background:#c6d1bd;width:10px;margin:-5px 0;border-radius:2px;}
        QStatusBar{background:#161e1a;border-top:1px solid #3a4b3e;color:#a6b8a5;}QScrollArea{border:1px solid #35443b;}
        QPlainTextEdit{background:#18211b;border:0;color:#9eb39a;font-family:"DejaVu Sans Mono",monospace;font-size:10px;}QToolTip{background:#d5dfcc;color:#19251c;border:1px solid #8b9e84;padding:5px;}
    )");
    auto* file = menuBar()->addMenu("Song");
    file->addAction("New song", QKeySequence::New, this, [this] { newSession(false); });
    file->addAction("Open demo", this, [this] { newSession(true); });
    file->addAction("Open…", QKeySequence::Open, this, [this] { open(); });
    file->addAction("Save", QKeySequence::Save, this, [this] { save(); });
    file->addAction("Save as…", QKeySequence::SaveAs, this, [this] { save(true); });
    file->addSeparator();
    file->addAction("Export song WAV…", QKeySequence("Ctrl+E"), this, [this] { exportWave(); });
    auto* edit = menuBar()->addMenu("Edit");
    edit->addAction("Undo", QKeySequence::Undo, this, [this] { undo(-1); });
    edit->addAction("Redo", QKeySequence("Ctrl+Shift+Z"), this, [this] { undo(1); });
    edit->addAction("Duplicate clip", QKeySequence("Ctrl+D"), this,
                    [this] { timeline->duplicateSelected(); });
    edit->addAction("Make clip independent", this, [this] { timeline->makeIndependent(); });
    moveNoteAction =
        edit->addAction("Move note…", QKeySequence("Ctrl+Shift+M"), this, [this] { moveNote(); });
    auto* engine = menuBar()->addMenu("Engine");
    engine->addAction("Reload workers", QKeySequence("Ctrl+R"), this, [this] { supervisor.reload(); });
    engine->addAction("Retry audio device", this, [this] {
        audio.close();
        QString e;
        if (!audio.open(e))
            report(e);
        else
            report("Connected: " + audio.deviceName());
    });
    engine->addAction("Output device…", this, [this] {
        QDialog dialog(this);
        dialog.setWindowTitle("Audio output");
        auto* layout = new QVBoxLayout(&dialog);
        auto* combo = new QComboBox;
        combo->addItem("System default", -1);
        for (auto& name : AudioEngine::devices())
            combo->addItem(name, name.section(':', 0, 0).toInt());
        layout->addWidget(new QLabel("48 kHz / stereo output"));
        layout->addWidget(combo);
        auto* connectButton = button("Connect");
        layout->addWidget(connectButton);
        connect(connectButton, &QPushButton::clicked, &dialog, &QDialog::accept);
        if (dialog.exec() == QDialog::Accepted) {
            audio.close();
            QString e;
            if (!audio.open(e, combo->currentData().toInt()))
                report(e);
        }
    });
    auto* help = menuBar()->addMenu("Help");
    help->addAction("Quick reference", this, [this] {
        QMessageBox::information(
            this, "FERRO · Quick reference",
            "Arrange a complete song on the timeline.\n\nDouble-click empty track space: new clip\nDrag clip "
            "body: move (also between tracks)\nCtrl-drag: copy a clip\nDrag clip right edge: repeat its "
            "phrase\nRight-click clip: duplicate, make independent, delete\nClick timeline ruler: seek\nDrag "
            "END marker or set Song bars: song length\n\nSelect a clip to edit notes below.\nClick empty "
            "space: add a note. Click a note: request deletion.\nDrag from empty space: draw; from a note: "
            "request deletion.\nShift-click: select without editing. Right-drag: request deletion.\n"
            "Deletion asks for confirmation before removing notes and their settings.\n"
            "Move note… / Ctrl+Shift+M: move a selected note in time, keeping all settings.\nStack pitches "
            "in a "
            "sixteenth-note step to make chords. Scroll: change pitch "
            "range.\nNOTE SOUND: check a setting to override it on the selected note; uncheck to follow "
            "TRACK. Reset to track defaults clears all five note overrides.\n"
            "Copies on the same track share a phrase. Make independent for variations.\n\nSpace: "
            "Play / pause   Home: Stop and rewind\nCtrl+S: Save   Ctrl+E: Export entire song once\nLoop is "
            "optional and off by default.\nExport includes the ending tail and does not use transport Loop.");
    });
    auto* content = new QWidget;
    auto* outer = new QScrollArea;
    outer->setWidgetResizable(true);
    outer->setWidget(content);
    setCentralWidget(outer);
    auto* root = new QVBoxLayout(content);
    root->setSizeConstraint(QLayout::SetMinimumSize);
    root->setContentsMargins(18, 12, 18, 10);
    root->setSpacing(9);
    auto* top = new QHBoxLayout;
    auto* logo = new QLabel("F E R R O");
    logo->setStyleSheet("font-size:24px;font-weight:700;color:#e1c8a2;");
    top->addWidget(logo);
    top->addWidget(caption("SONG STUDIO"));
    top->addStretch();
    title = new QLineEdit;
    title->setMaxLength(256);
    title->setMinimumWidth(280);
    top->addWidget(title);
    auto* saveButton = button("Save", "Ctrl+S");
    auto* exportButton = button("Export song", "One complete timeline + ending tail · Ctrl+E");
    top->addWidget(saveButton);
    top->addWidget(exportButton);
    root->addLayout(top);
    connect(title, &QLineEdit::textEdited, this, [this](const QString& text) {
        project.title = text;
        change();
    });
    connect(saveButton, &QPushButton::clicked, this, [this] { save(); });
    connect(exportButton, &QPushButton::clicked, this, [this] { exportWave(); });
    auto* transport = new QHBoxLayout;
    playButton = button("▶ PLAY", "Space");
    playButton->setObjectName("playButton");
    playButton->setMinimumWidth(96);
    auto* stop = button("■ STOP", "Home");
    transport->addWidget(playButton);
    transport->addWidget(stop);
    transport->addSpacing(12);
    clock = new QLabel;
    clock->setStyleSheet("font-family:monospace;font-size:20px;color:#d5dfca;");
    transport->addWidget(clock);
    barClock = caption("BAR 1 · BEAT 1");
    transport->addWidget(barClock);
    transport->addStretch();
    transport->addWidget(caption("TEMPO"));
    bpm = new QDoubleSpinBox;
    bpm->setRange(40, 300);
    bpm->setDecimals(1);
    bpm->setSuffix(" BPM");
    bpm->setKeyboardTracking(false);
    transport->addWidget(bpm);
    loop = new QCheckBox("Loop");
    loop->setObjectName("loop");
    loop->setToolTip("Optional whole-song repeat for listening. Export always writes one song.");
    transport->addWidget(loop);
    click = new QCheckBox("Click");
    transport->addWidget(click);
    root->addLayout(transport);
    connect(playButton, &QPushButton::clicked, this, [this] { togglePlay(); });
    connect(stop, &QPushButton::clicked, this, [this] { stopPlayback(); });
    connect(bpm, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        if (updating)
            return;
        double duration = project.songBars * 240. / value + project.tailSeconds;
        if (duration > MaxSongSeconds) {
            QSignalBlocker b(bpm);
            bpm->setValue(project.bpm);
            report("This tempo exceeds the 10-minute song limit. Shorten the song first.");
            return;
        }
        project.bpm = value;
        refreshProject();
        change();
    });
    connect(loop, &QCheckBox::toggled, this, [this](bool v) { audio.setLooping(v); });
    connect(click, &QCheckBox::toggled, this, [this](bool v) {
        if (!updating) {
            project.metronome = v;
            change();
        }
    });
    auto* playShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(playShortcut, &QShortcut::activated, this, [this] { togglePlay(); });
    auto* stopShortcut = new QShortcut(QKeySequence(Qt::Key_Home), this);
    connect(stopShortcut, &QShortcut::activated, this, [this] { stopPlayback(); });
    connect(qApp, &QApplication::focusChanged, this, [playShortcut, stopShortcut](QWidget*, QWidget* now) {
        bool typing = qobject_cast<QLineEdit*>(now) || qobject_cast<QAbstractSpinBox*>(now);
        playShortcut->setEnabled(!typing);
        stopShortcut->setEnabled(!typing);
    });
    auto* arrangement = new QHBoxLayout;
    arrangement->addWidget(caption("ARRANGEMENT"));
    songLength = new QSpinBox;
    songLength->setRange(1, MaxSongBars);
    songLength->setPrefix("Song: ");
    songLength->setSuffix(" bars");
    songLength->setKeyboardTracking(false);
    arrangement->addWidget(songLength);
    tail = new QDoubleSpinBox;
    tail->setRange(0, 8);
    tail->setDecimals(1);
    tail->setPrefix("Tail: ");
    tail->setSuffix(" s");
    tail->setKeyboardTracking(false);
    tail->setToolTip("Extra ending time for synth releases and delay; included in playback and export.");
    arrangement->addWidget(tail);
    auto* fitEnd = button("Fit end", "Set song end to the last clip");
    arrangement->addWidget(fitEnd);
    arrangement->addStretch();
    arrangement->addWidget(caption("DRAG EDGE TO REPEAT · CTRL-DRAG TO COPY"));
    arrangement->addWidget(caption("ZOOM"));
    zoom = new QSlider(Qt::Horizontal);
    zoom->setRange(12, 160);
    zoom->setValue(72);
    zoom->setMaximumWidth(130);
    arrangement->addWidget(zoom);
    auto* fitView = button("Fit view");
    arrangement->addWidget(fitView);
    root->addLayout(arrangement);
    timeline = new Timeline(project);
    timelineScroll = new QScrollArea;
    timelineScroll->setWidget(timeline);
    timelineScroll->setFixedHeight(Timeline::Ruler + TrackCount * Timeline::Row + 24);
    timelineScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(timelineScroll);
    timeline->edited = [this](bool modified) {
        refreshSelection();
        if (modified) {
            songLength->blockSignals(true);
            songLength->setValue(project.songBars);
            songLength->blockSignals(false);
            change();
        }
    };
    timeline->message = [this](QString text) { report(text); };
    timeline->seek = [this](double bar) { requestSeek(bar); };
    connect(songLength, qOverload<int>(&QSpinBox::valueChanged), this, [this](int bars) {
        if (updating)
            return;
        int max = int(std::floor((MaxSongSeconds - project.tailSeconds) * project.bpm / 240.));
        project.songBars = std::clamp(bars, contentEndBar(project), std::min(MaxSongBars, max));
        refreshProject();
        change();
    });
    connect(tail, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double seconds) {
        if (updating)
            return;
        if (project.songBars * 240. / project.bpm + seconds > MaxSongSeconds) {
            QSignalBlocker blocker(tail);
            tail->setValue(project.tailSeconds);
            report("Song plus ending tail must fit within 10 minutes.");
            return;
        }
        project.tailSeconds = seconds;
        change();
    });
    connect(fitEnd, &QPushButton::clicked, this, [this] {
        project.songBars = contentEndBar(project);
        refreshProject();
        change();
    });
    connect(zoom, &QSlider::valueChanged, this, [this](int value) {
        timeline->scale = value;
        timeline->refresh();
    });
    connect(fitView, &QPushButton::clicked, this, [this] {
        timeline->fitZoom(timelineScroll->viewport()->width());
        QSignalBlocker b(zoom);
        zoom->setValue(int(timeline->scale));
    });
    auto* phraseRow = new QHBoxLayout;
    phraseRow->addWidget(caption("CLIP NOTES"));
    phraseName = new QLineEdit;
    phraseName->setMaxLength(80);
    phraseName->setMaximumWidth(230);
    phraseRow->addWidget(phraseName);
    phraseLength = new QSpinBox;
    phraseLength->setRange(1, 16);
    phraseLength->setPrefix("Phrase: ");
    phraseLength->setSuffix(" bars");
    phraseLength->setKeyboardTracking(false);
    phraseRow->addWidget(phraseLength);
    auto* unique = button("Make independent", "Give this clip its own editable notes");
    phraseRow->addWidget(unique);
    auto* clear = button("Clear notes", "Clear the selected phrase; shared clips follow this edit");
    clear->setObjectName("clearNotes");
    phraseRow->addWidget(clear);
    auto* fitNotes = button("Fit notes");
    phraseRow->addWidget(fitNotes);
    clipInfo = caption("SELECT A CLIP");
    phraseRow->addStretch();
    phraseRow->addWidget(clipInfo);
    root->addLayout(phraseRow);
    editor = new NoteEditor(project);
    editorScroll = new QScrollArea;
    editorScroll->setWidgetResizable(true);
    editorScroll->setWidget(editor);
    editorScroll->setFixedHeight(276);
    editorScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(editorScroll);
    editor->edited = [this](bool modified) {
        refreshSelection();
        if (modified) {
            timeline->update();
            change();
        }
    };
    editor->confirmErase = [this](int count) { return confirmNoteDeletion(count, "Erase selected notes"); };
    connect(phraseName, &QLineEdit::textEdited, this, [this](const QString& name) {
        if (auto* p = editor->phrase()) {
            p->name = name;
            timeline->update();
            change();
        }
    });
    connect(phraseLength, qOverload<int>(&QSpinBox::valueChanged), this, [this](int bars) {
        if (!updating)
            resizePhrase(bars);
    });
    connect(unique, &QPushButton::clicked, this, [this] { timeline->makeIndependent(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (auto* p = editor->phrase()) {
            int count = 0;
            for (const auto& s : p->steps)
                count += int(s.notes.size());
            if (count == 0 || !confirmNoteDeletion(count, "Clear this phrase"))
                return;
            p = editor->phrase();
            if (!p)
                return;
            for (auto& s : p->steps)
                s.notes.clear();
            refreshSelection();
            change();
        }
    });
    connect(fitNotes, &QPushButton::clicked, editor, [this] { editor->fitNotes(); });
    auto* bottom = new QHBoxLayout;
    auto* trackBox = new QGroupBox("TRACK");
    auto* trackLayout = new QVBoxLayout(trackBox);
    auto* trackRow = new QHBoxLayout;
    trackName = new QLineEdit;
    trackName->setMaxLength(80);
    trackName->setMaximumWidth(160);
    trackRow->addWidget(trackName);
    instrument = new QComboBox;
    for (int i = 0; i < TrackCount; ++i)
        instrument->addItem(instrumentName(i), i);
    trackRow->addWidget(instrument);
    trackTitle = caption("SOUND");
    trackRow->addWidget(trackTitle);
    trackRow->addStretch();
    muteButton = button("MUTE");
    soloButton = button("SOLO");
    muteButton->setCheckable(true);
    soloButton->setCheckable(true);
    trackRow->addWidget(muteButton);
    trackRow->addWidget(soloButton);
    trackLayout->addLayout(trackRow);
    auto control = [&](QBoxLayout* layout, const QString& name, QSlider*& slider, int lo, int hi, int value,
                       std::function<void(int)> cb) {
        auto* col = new QVBoxLayout;
        auto* row = new QHBoxLayout;
        row->addWidget(caption(name));
        auto* number = caption(QString::number(value));
        number->setAlignment(Qt::AlignRight);
        row->addWidget(number);
        col->addLayout(row);
        slider = new QSlider(Qt::Horizontal);
        slider->setRange(lo, hi);
        slider->setValue(value);
        slider->setMinimumWidth(90);
        col->addWidget(slider);
        layout->addLayout(col);
        connect(slider, &QSlider::valueChanged, this, [this, number, cb](int x) {
            number->setText(QString::number(x));
            if (!updating) {
                cb(x);
                refreshNoteSound();
                change();
            }
        });
    };
    auto* sounds = new QHBoxLayout;
    control(sounds, "LEVEL", volume, 0, 150, 75,
            [this](int v) { project.tracks[timeline->track].volume = v / 100.; });
    control(sounds, "PAN", pan, -100, 100, 0,
            [this](int v) { project.tracks[timeline->track].pan = v / 100.; });
    control(sounds, "TONE", tone, 0, 100, 50,
            [this](int v) { project.tracks[timeline->track].tone = v / 100.; });
    control(sounds, "DECAY", decay, 0, 100, 50,
            [this](int v) { project.tracks[timeline->track].decay = v / 100.; });
    control(sounds, "DRIVE", drive, 0, 100, 20,
            [this](int v) { project.tracks[timeline->track].drive = v / 100.; });
    trackLayout->addLayout(sounds);
    auto* sampleRow = new QHBoxLayout;
    sampleButton = button("Load sample…");
    auto* synth = button("Use instrument");
    sampleRow->addWidget(sampleButton);
    sampleRow->addWidget(synth);
    sampleRow->addStretch();
    sampleRow->addWidget(caption("SAMPLE ROOT: C4 / MIDI 60"));
    trackLayout->addLayout(sampleRow);
    bottom->addWidget(trackBox, 4);
    connect(trackName, &QLineEdit::textEdited, this, [this](const QString& text) {
        project.tracks[timeline->track].name = text;
        timeline->update();
        change();
    });
    connect(instrument, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (!updating) {
            project.tracks[timeline->track].instrument = i;
            timeline->update();
            change();
        }
    });
    connect(muteButton, &QPushButton::toggled, this, [this](bool v) {
        if (!updating) {
            project.tracks[timeline->track].mute = v;
            timeline->update();
            change();
        }
    });
    connect(soloButton, &QPushButton::toggled, this, [this](bool v) {
        if (!updating) {
            project.tracks[timeline->track].solo = v;
            change();
        }
    });
    connect(sampleButton, &QPushButton::clicked, this, [this] { importSample(); });
    connect(synth, &QPushButton::clicked, this, [this] {
        project.tracks[timeline->track].samplePath.clear();
        refreshSelection();
        change();
    });
    auto* stepBox = new QGroupBox("NOTE");
    auto* stepLayout = new QVBoxLayout(stepBox);
    auto* pitchRow = new QHBoxLayout;
    pitchRow->addWidget(caption("PITCH"));
    note = new QSpinBox;
    note->setRange(0, 127);
    note->setKeyboardTracking(false);
    pitchRow->addWidget(note);
    noteLabel = new QLabel;
    pitchRow->addWidget(noteLabel);
    stepLayout->addLayout(pitchRow);
    auto* velRow = new QHBoxLayout;
    velRow->addWidget(caption("VELOCITY"));
    velocity = new QSpinBox;
    velocity->setRange(0, 127);
    velRow->addWidget(velocity);
    stepLayout->addLayout(velRow);
    moveNoteButton = button("Move note…", "Move this note in time, keeping its pitch, velocity and sound");
    moveNoteButton->setObjectName("moveNote");
    stepLayout->addWidget(moveNoteButton);
    connect(moveNoteButton, &QPushButton::clicked, this, [this] { moveNote(); });
    stepLayout->addWidget(caption("RIGHT-DRAG TO ERASE"));
    bottom->addWidget(stepBox, 1);
    connect(note, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        noteLabel->setText(noteName(value));
        if (!updating)
            if (auto* selected = editor->selectedNote()) {
                auto& notes = editor->phrase()->steps[editor->step].notes;
                if (std::any_of(notes.begin(), notes.end(), [selected, value](const Note& other) {
                        return &other != selected && other.note == value && other.velocity > 0;
                    })) {
                    refreshSelection();
                    report("That pitch already has a note in this step. Select it to edit.");
                    return;
                }
                if (selected->note != value) {
                    const int silentCount =
                        int(std::count_if(notes.begin(), notes.end(), [value](const Note& other) {
                            return other.note == value && other.velocity <= 0;
                        }));
                    if (silentCount &&
                        !confirmNoteDeletion(silentCount, "Replace a silent note at this pitch")) {
                        refreshSelection();
                        return;
                    }
                    auto& currentNotes = editor->phrase()->steps[editor->step].notes;
                    currentNotes.erase(std::remove_if(currentNotes.begin(), currentNotes.end(),
                                                      [value](const Note& other) {
                                                          return other.note == value && other.velocity <= 0;
                                                      }),
                                       currentNotes.end());
                    selected = editor->selectedNote();
                }
                selected->note = value;
                editor->selectedPitch = value;
                editor->update();
                timeline->update();
                change();
            }
    });
    connect(velocity, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        if (!updating)
            if (auto* selected = editor->selectedNote()) {
                selected->velocity = value / 127.;
                editor->update();
                timeline->update();
                change();
            }
    });
    auto* masterBox = new QGroupBox("MASTER");
    auto* ml = new QVBoxLayout(masterBox);
    control(ml, "OUTPUT", master, 0, 100, 70, [this](int x) { project.master = x / 100.; });
    control(ml, "DELAY", delay, 0, 100, 15, [this](int x) { project.delay = x / 100.; });
    control(ml, "BASS DUCK", duck, 0, 100, 35, [this](int x) { project.duck = x / 100.; });
    bottom->addWidget(masterBox, 1);
    root->addLayout(bottom);
    auto* noteSoundBox = new QGroupBox("NOTE SOUND");
    auto* noteSoundLayout = new QVBoxLayout(noteSoundBox);
    noteSoundLayout->addWidget(caption("UNCHECKED: FOLLOW TRACK · CHECKED: OVERRIDE SELECTED NOTE"));
    auto* noteSoundRow = new QHBoxLayout;
    const QStringList soundNames = {"LEVEL", "PAN", "TONE", "DECAY", "DRIVE"};
    for (size_t i = 0; i < noteSoundOverride.size(); ++i) {
        auto* field = new QVBoxLayout;
        auto* enabled = new QCheckBox(soundNames[int(i)]);
        auto* value = new QSpinBox;
        noteSoundOverride[i] = enabled;
        noteSoundValue[i] = value;
        enabled->setObjectName("note" + soundNames[int(i)] + "Override");
        value->setObjectName("note" + soundNames[int(i)] + "Value");
        enabled->setToolTip("Checked: use this value for the selected note. Unchecked: follow TRACK.");
        value->setRange(i == 1 ? -100 : 0, i == 0 ? 150 : 100);
        value->setSuffix("%");
        value->setKeyboardTracking(false);
        field->addWidget(enabled);
        field->addWidget(value);
        noteSoundRow->addLayout(field, 1);
        connect(enabled, &QCheckBox::toggled, this, [this, i](bool checked) {
            if (updating)
                return;
            if (auto* selected = editor->selectedNote()) {
                auto& override = selected->overrides.*noteOverrideFields[i];
                if (checked) {
                    const auto effective = resolvedNoteSettings(project.tracks[editor->track], *selected);
                    override = effective.*noteSettingFields[i];
                } else
                    override.reset();
                refreshNoteSound();
                change();
            }
        });
        connect(value, qOverload<int>(&QSpinBox::valueChanged), this, [this, i](int number) {
            if (updating)
                return;
            if (auto* selected = editor->selectedNote()) {
                selected->overrides.*noteOverrideFields[i] = number / 100.;
                refreshNoteSound();
                change();
            }
        });
    }
    noteSoundReset = button("Reset to track defaults", "Clear all five sound overrides on the selected note");
    noteSoundReset->setObjectName("resetNoteSound");
    noteSoundRow->addWidget(noteSoundReset);
    noteSoundLayout->addLayout(noteSoundRow);
    root->addWidget(noteSoundBox);
    connect(noteSoundReset, &QPushButton::clicked, this, [this] {
        if (auto* selected = editor->selectedNote()) {
            selected->overrides = {};
            refreshNoteSound();
            change();
        }
    });
    auto* enginePanel = new QWidget;
    auto* engineLayout = new QHBoxLayout(enginePanel);
    engineLayout->setContentsMargins(0, 0, 0, 0);
    workers = new QPlainTextEdit;
    workers->setReadOnly(true);
    workers->setFixedHeight(65);
    engineLayout->addWidget(workers);
    auto* reload = button("Reload workers");
    engineLayout->addWidget(reload);
    connect(reload, &QPushButton::clicked, this, [this] { supervisor.reload(); });
    enginePanel->hide();
    root->addWidget(enginePanel);
    auto* monitor = engine->addAction("Show engine monitor");
    monitor->setCheckable(true);
    connect(monitor, &QAction::toggled, enginePanel, &QWidget::setVisible);
    renderState = caption("PREPARING SONG");
    statusBar()->addWidget(renderState);
    engineState = caption("AUDIO STANDBY");
    statusBar()->addPermanentWidget(engineState);
    meterLabel = caption("PEAK — / XRUN 0");
    statusBar()->addPermanentWidget(meterLabel);
}
void Window::refreshSelection() {
    updating = true;
    auto& tr = project.tracks[timeline->track];
    auto* clip = timeline->selection();
    int selectedPattern = clip ? clip->pattern : -1;
    if (editor->track != timeline->track || editor->pattern != selectedPattern)
        editor->select(timeline->track, selectedPattern);
    auto* p = editor->phrase();
    phraseName->setEnabled(p);
    phraseLength->setEnabled(p);
    if (p) {
        editor->step = std::clamp(editor->step, 0, int(p->steps.size()) - 1);
        phraseName->setText(p->name);
        phraseLength->setValue(p->bars);
        int count = 0;
        for (auto& c : tr.clips)
            if (c.pattern == clip->pattern)
                ++count;
        clipInfo->setText(QString("%1 BAR%2 · %3 LINKED CLIP%4")
                              .arg(clip->lengthBars)
                              .arg(clip->lengthBars == 1 ? "" : "S")
                              .arg(count)
                              .arg(count == 1 ? "" : "S"));
    } else {
        phraseName->clear();
        phraseName->setPlaceholderText("Select or create a clip");
        clipInfo->setText("DOUBLE-CLICK EMPTY TRACK SPACE TO CREATE A CLIP");
    }
    auto* selected = editor->selectedNote();
    note->setEnabled(selected);
    velocity->setEnabled(selected);
    moveNoteButton->setEnabled(selected);
    moveNoteAction->setEnabled(selected);
    note->setValue(selected ? selected->note : editor->defaultNote());
    noteLabel->setText(selected ? noteName(selected->note) : "—");
    velocity->setValue(selected ? int(std::round(selected->velocity * 127)) : 0);
    refreshNoteSound();
    trackName->setText(tr.name);
    instrument->setCurrentIndex(tr.instrument);
    trackTitle->setText(tr.samplePath.isEmpty() ? "INSTRUMENT" : "SAMPLE");
    volume->setValue(int(std::round(tr.volume * 100)));
    pan->setValue(int(std::round(tr.pan * 100)));
    tone->setValue(int(std::round(tr.tone * 100)));
    decay->setValue(int(std::round(tr.decay * 100)));
    drive->setValue(int(std::round(tr.drive * 100)));
    muteButton->setChecked(tr.mute);
    soloButton->setChecked(tr.solo);
    sampleButton->setText(tr.samplePath.isEmpty() ? "Load sample…"
                                                  : QFileInfo(tr.samplePath).fileName().left(24));
    sampleButton->setToolTip(tr.samplePath);
    updating = false;
    editor->refresh();
    timeline->update();
}
void Window::refreshNoteSound() {
    if (!editor || !noteSoundReset)
        return;
    const auto* selected = editor->selectedNote();
    const auto effective =
        resolvedNoteSettings(project.tracks[timeline->track], selected ? *selected : Note{});
    bool anyOverride = false;
    for (size_t i = 0; i < noteSoundOverride.size(); ++i) {
        const bool overridden = selected && (selected->overrides.*noteOverrideFields[i]).has_value();
        anyOverride |= overridden;
        const QSignalBlocker blockOverride(noteSoundOverride[i]), blockValue(noteSoundValue[i]);
        noteSoundOverride[i]->setEnabled(selected);
        noteSoundOverride[i]->setChecked(overridden);
        noteSoundValue[i]->setEnabled(selected && overridden);
        noteSoundValue[i]->setValue(int(std::round((effective.*noteSettingFields[i]) * 100)));
    }
    noteSoundReset->setEnabled(selected && anyOverride);
}
void Window::refreshProject() {
    updating = true;
    title->setText(project.title);
    bpm->setValue(project.bpm);
    click->setChecked(project.metronome);
    songLength->setValue(project.songBars);
    tail->setValue(project.tailSeconds);
    master->setValue(int(std::round(project.master * 100)));
    delay->setValue(int(std::round(project.delay * 100)));
    duck->setValue(int(std::round(project.duck * 100)));
    updating = false;
    timeline->track = std::clamp(timeline->track, 0, TrackCount - 1);
    if (timeline->selectedClip && !timeline->selection())
        timeline->selectedClip = 0;
    timeline->refresh();
    refreshSelection();
    setWindowTitle("FERRO · " + project.title + (dirty ? " *" : ""));
}
void Window::resizePhrase(int bars) {
    auto* p = editor->phrase();
    auto* clip = timeline->selection();
    if (!p || !clip)
        return;
    int old = p->bars;
    if (bars < old) {
        int removed = 0;
        for (size_t i = size_t(bars * Steps); i < p->steps.size(); ++i)
            removed += int(p->steps[i].notes.size());
        if (removed && !confirmNoteDeletion(removed, "Shorten this phrase")) {
            refreshSelection();
            return;
        }
        p = editor->phrase();
        clip = timeline->selection();
        if (!p || !clip)
            return;
    }
    if (clip->lengthBars == old) {
        int end = clip->startBar + bars;
        bool room = end <= MaxSongBars && end * 240. / project.bpm + project.tailSeconds <= MaxSongSeconds;
        for (auto& c : project.tracks[timeline->track].clips)
            if (c.id != clip->id && clip->startBar < c.startBar + c.lengthBars && end > c.startBar)
                room = false;
        if (!room) {
            refreshSelection();
            report("There is no room to extend this phrase. Move the next clip first.");
            return;
        }
        clip->lengthBars = bars;
        project.songBars = std::max(project.songBars, end);
    }
    p->bars = bars;
    p->steps.resize(bars * Steps);
    editor->step = std::min(editor->step, bars * Steps - 1);
    refreshProject();
    change();
}
bool Window::confirmNoteDeletion(int count, const QString& reason) {
    if (count <= 0)
        return true;
    const auto before = projectToJson(project);
    const int trackBefore = timeline->track, editorTrack = editor->track, patternBefore = editor->pattern,
              stepBefore = editor->step, pitchBefore = editor->selectedPitch;
    const uint64_t clipBefore = timeline->selectedClip;
    QMessageBox dialog(QMessageBox::Warning, "Delete notes?",
                       QString("%1\n\nDelete %2 note%3 and their pitch, velocity and sound settings?\n"
                               "Clips using the affected phrases will also change.\n"
                               "You can undo this deletion with Ctrl+Z.")
                           .arg(reason)
                           .arg(count)
                           .arg(count == 1 ? "" : "s"),
                       QMessageBox::Discard | QMessageBox::Cancel, this);
    dialog.setObjectName("confirmNoteDeletion");
    dialog.button(QMessageBox::Discard)->setText("Delete notes");
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.setEscapeButton(QMessageBox::Cancel);
    if (dialog.exec() != QMessageBox::Discard)
        return false;
    if (projectToJson(project) != before || timeline->track != trackBefore ||
        timeline->selectedClip != clipBefore || editor->track != editorTrack ||
        editor->pattern != patternBefore || editor->step != stepBefore ||
        editor->selectedPitch != pitchBefore) {
        report("The song or selection changed. Review the notes and try deleting again.");
        return false;
    }
    return true;
}
void Window::moveNote() {
    const auto* selected = editor->selectedNote();
    const auto* phrase = editor->phrase();
    if (!selected || !phrase) {
        report("Shift-click a note to select it before moving.");
        return;
    }
    const auto before = projectToJson(project);
    const int sourceTrack = editor->track, sourcePattern = editor->pattern, sourceStep = editor->step,
              sourcePitch = editor->selectedPitch, timelineTrack = timeline->track;
    const uint64_t sourceClip = timeline->selectedClip;
    QDialog dialog(this);
    dialog.setObjectName("moveNoteDialog");
    dialog.setWindowTitle("Move note");
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QString("Move %1 within this phrase.\n"
                                         "Pitch, velocity and all sound settings stay with the note.\n"
                                         "Clips sharing this phrase follow the move.")
                                     .arg(noteName(selected->note))));
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel("Bar"));
    auto* targetBar = new QSpinBox;
    targetBar->setObjectName("moveNoteBar");
    targetBar->setRange(1, phrase->bars);
    targetBar->setValue(editor->step / Steps + 1);
    row->addWidget(targetBar);
    row->addWidget(new QLabel("Step"));
    auto* targetStep = new QSpinBox;
    targetStep->setObjectName("moveNoteStep");
    targetStep->setRange(1, Steps);
    targetStep->setValue(editor->step % Steps + 1);
    row->addWidget(targetStep);
    layout->addLayout(row);
    auto* error = new QLabel;
    error->setObjectName("moveNoteError");
    error->setWordWrap(true);
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Move");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this] {
        if (projectToJson(project) != before || editor->track != sourceTrack ||
            editor->pattern != sourcePattern || editor->step != sourceStep ||
            editor->selectedPitch != sourcePitch || timeline->track != timelineTrack ||
            timeline->selectedClip != sourceClip) {
            error->setText("The song or selection changed. Cancel and select the note again.");
            return;
        }
        const int destination = (targetBar->value() - 1) * Steps + targetStep->value() - 1;
        if (destination == editor->step || editor->moveSelectedNote(destination))
            dialog.accept();
        else
            error->setText("That step already contains this pitch, or the note is no longer available.");
    });
    dialog.exec();
}
void Window::resetHistory() {
    history = {project};
    historyIndex = 0;
    savedState = projectToJson(project);
}
void Window::undo(int delta) {
    int index = historyIndex + delta;
    if (index < 0 || index >= int(history.size()))
        return;
    historyIndex = index;
    project = history[index];
    refreshProject();
    change();
}
void Window::change() {
    if (updating)
        return;
    auto state = projectToJson(project);
    if (history.empty()) {
        history = {project};
        historyIndex = 0;
    } else if (state != projectToJson(history[historyIndex])) {
        history.resize(historyIndex + 1);
        history.push_back(project);
        if (history.size() > 64)
            history.erase(history.begin());
        historyIndex = int(history.size()) - 1;
    }
    dirty = state != savedState;
    setWindowTitle("FERRO · " + project.title + (dirty ? " *" : ""));
    timeline->update();
    editor->update();
    if (renderTimer) {
        renderState->setText("CHANGES PENDING");
        renderTimer->start();
    }
}
void Window::tick() {
    audio.collect();
    applyQueuedSeek();
    if (queuedSeekBar < 0 && audio.frames() > 0)
        previewSeek = -1;
    uint64_t f = audio.position();
    if (previewSeek >= 0)
        f = uint64_t(previewSeek);
    uint64_t total = audio.frames() ? audio.frames() : uint64_t(renderFrames(project));
    clock->setText(timecode(f) + " / " + timecode(total));
    double barFrames = SampleRate * 240. / (previewSeek < 0 && audio.frames() ? audio.tempo() : project.bpm);
    double bar = f / barFrames;
    barClock->setText(f >= total && total > 0
                          ? "END OF SONG"
                          : QString("BAR %1 · BEAT %2").arg(int(bar) + 1).arg(int(bar * 4) % 4 + 1));
    timeline->playBar = std::min(double(project.songBars), bar);
    timeline->update();
    editor->playStep = -1;
    if (auto* c = timeline->selection())
        if (audio.playing() && bar >= c->startBar && bar < c->startBar + c->lengthBars)
            if (auto* p = editor->phrase())
                editor->playStep = std::fmod((bar - c->startBar) * Steps, p->steps.size());
    editor->update();
    playButton->setText(queuedSeekBar >= 0 && resumeAfterSeek ? "Ⅱ WAITING"
                        : audio.playing()                     ? "Ⅱ PAUSE"
                                                              : "▶ PLAY");
    double peak = audio.peak();
    meterLabel->setText(QString("PEAK %1 dBFS / XRUN %2 / CPU %3%")
                            .arg(peak > 0.00001 ? 20 * std::log10(peak) : -100., 0, 'f', 1)
                            .arg(audio.xruns())
                            .arg(audio.cpuLoad() * 100., 0, 'f', 1));
    engineState->setText(audio.deviceName().isEmpty() ? "AUDIO STANDBY" : audio.deviceName().left(24));
    auto status = supervisor.statusLines().join("\n");
    if (workers->toPlainText() != status)
        workers->setPlainText(status);
}
void Window::togglePlay() {
    if (audio.deviceName().isEmpty()) {
        QString e;
        if (!audio.open(e)) {
            QMessageBox::warning(this, "Audio output",
                                 e + "\n\nSelect an output in Engine → Output device. Song editing and WAV "
                                     "export remain available.");
            return;
        }
    }
    if (queuedSeekBar >= 0) {
        resumeAfterSeek = !resumeAfterSeek;
        applyQueuedSeek();
        return;
    }
    audio.setPlaying(!audio.playing());
}
void Window::requestSeek(double bar) {
    queuedSeekBar = std::clamp(bar, 0., double(project.songBars));
    resumeAfterSeek = audio.playing() || resumeAfterSeek;
    previewSeek = int64_t(std::llround(queuedSeekBar * SampleRate * 240. / project.bpm));
    if (!audioRevision || audio.revision() != audioRevision || preparedState != projectToJson(project))
        audio.setPlaying(false);
    applyQueuedSeek();
    tick();
}
void Window::applyQueuedSeek() {
    if (queuedSeekBar < 0 || !audioRevision || audio.revision() != audioRevision ||
        preparedState != projectToJson(project))
        return;
    const auto frame = uint64_t(std::llround(std::clamp(queuedSeekBar, 0., double(project.songBars)) *
                                             SampleRate * 240. / audio.tempo()));
    audio.seek(frame);
    audio.setPlaying(resumeAfterSeek);
    queuedSeekBar = -1;
    previewSeek = -1;
    resumeAfterSeek = false;
}
void Window::stopPlayback() {
    audio.setPlaying(false);
    audio.rewind();
    queuedSeekBar = -1;
    resumeAfterSeek = false;
    previewSeek = audio.deviceName().isEmpty() && audio.frames() == 0 ? 0 : -1;
}
void Window::report(const QString& message) {
    statusBar()->showMessage(message, 15000);
}
void Window::recoverSession(Project p) {
    audio.clear();
    previewSeek = 0;
    queuedSeekBar = -1;
    resumeAfterSeek = false;
    audioRevision = 0;
    preparedState = {};
    project = std::move(p);
    projectPath.clear();
    timeline->selectedClip = 0;
    resetHistory();
    savedState = {};
    refreshProject();
    change();
}
void Window::newSession(bool demo) {
    if (!confirmDiscard())
        return;
    audio.clear();
    previewSeek = 0;
    queuedSeekBar = -1;
    resumeAfterSeek = false;
    audioRevision = 0;
    preparedState = {};
    project = demo ? demoProject() : emptyProject();
    projectPath.clear();
    dirty = false;
    timeline->selectedClip = 0;
    timeline->track = 0;
    resetHistory();
    refreshProject();
    renderTimer->start();
}
void Window::open() {
    if (!confirmDiscard())
        return;
    QString path = QFileDialog::getOpenFileName(this, "Open song", QDir::homePath(),
                                                "FERRO song (*.ferro);;JSON (*.json)");
    if (path.isEmpty())
        return;
    Project p;
    QString e;
    if (!loadProject(path, p, e)) {
        QMessageBox::warning(this, "Open failed", e);
        return;
    }
    audio.clear();
    previewSeek = 0;
    queuedSeekBar = -1;
    resumeAfterSeek = false;
    audioRevision = 0;
    preparedState = {};
    project = std::move(p);
    projectPath = path;
    dirty = false;
    timeline->selectedClip = 0;
    resetHistory();
    refreshProject();
    renderTimer->start();
    report("Opened " + path);
}
bool Window::confirmDiscard() {
    if (!dirty)
        return true;
    auto result = QMessageBox::question(this, "Unsaved song", "Save changes before continuing?",
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (result == QMessageBox::Cancel)
        return false;
    if (result == QMessageBox::Save) {
        save();
        return !dirty;
    }
    return true;
}
void Window::save(bool as) {
    QString path = projectPath;
    if (as || path.isEmpty())
        path = QFileDialog::getSaveFileName(
            this, "Save song",
            projectPath.isEmpty() ? QDir::homePath() + "/Music/" + project.title + ".ferro" : projectPath,
            "FERRO song (*.ferro)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".ferro", Qt::CaseInsensitive))
        path += ".ferro";
    QString e;
    if (!saveProject(path, project, e)) {
        QMessageBox::warning(this, "Save failed", e);
        return;
    }
    projectPath = path;
    dirty = false;
    savedState = projectToJson(project);
    QFile::remove(autosavePath);
    setWindowTitle("FERRO · " + project.title);
    report("Saved " + path);
}
void Window::importSample() {
    QString path = QFileDialog::getOpenFileName(this, "Load sample (up to 16 seconds)", QDir::homePath(),
                                                "Audio (*.wav *.flac *.aif *.aiff *.ogg);;All files (*)");
    if (path.isEmpty())
        return;
    SF_INFO info{};
    auto* file = sf_open(QFile::encodeName(path).constData(), SFM_READ, &info);
    if (!file) {
        QMessageBox::warning(this, "Sample", QString::fromUtf8(sf_strerror(nullptr)));
        return;
    }
    sf_close(file);
    if (info.frames < 1 || info.channels < 1 || info.channels > 8 || info.samplerate < 8000 ||
        info.samplerate > 384000 || info.frames > qint64(info.samplerate) * 16) {
        QMessageBox::warning(this, "Sample", "Use a sample up to 16 seconds, 1–8 channels, 8–384 kHz.");
        return;
    }
    applySample(path);
}
bool Window::applySample(const QString& path) {
    auto& tr = project.tracks[timeline->track];
    Track candidate = tr;
    candidate.samplePath = path;
    bool preservedChords = prepareSamplePitches(candidate);
    int previousCount = 0, nextCount = 0;
    for (const auto& pattern : tr.patterns)
        for (const auto& step : pattern.steps)
            previousCount += int(step.notes.size());
    for (const auto& pattern : candidate.patterns)
        for (const auto& step : pattern.steps)
            nextCount += int(step.notes.size());
    if (previousCount > nextCount &&
        !confirmNoteDeletion(previousCount - nextCount, "Remove silent notes while retuning this sample"))
        return false;
    project.tracks[timeline->track] = std::move(candidate);
    if (!preservedChords && editor->selectedPitch >= 0)
        editor->selectedPitch = 60;
    editor->fitNotes();
    refreshSelection();
    change();
    if (preservedChords)
        report("Sample loaded; existing note pitches preserved (sample root C4).");
    return true;
}
void Window::exportWave() {
    if (exportBusy) {
        report("A song export is already running.");
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, "Export complete song",
                                                QDir::homePath() + "/Music/" + project.title + ".wav",
                                                "WAV audio (*.wav)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".wav", Qt::CaseInsensitive))
        path += ".wav";
    Project snapshot = project;
    exportBusy = true;
    report("Rendering complete song + ending tail…");
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, path] {
        QString e = watcher->result();
        exportBusy = false;
        watcher->deleteLater();
        if (e.isEmpty())
            report("Exported complete song: " + path);
        else
            QMessageBox::warning(this, "Export failed", e);
    });
    watcher->setFuture(QtConcurrent::run([snapshot, path]() -> QString {
        try {
            auto data = renderProject(snapshot);
            QString e;
            if (!writeWave(path, data, e))
                return e;
            return {};
        } catch (const std::exception& e) {
            return QString::fromUtf8(e.what());
        }
    }));
}
void Window::closeEvent(QCloseEvent* e) {
    if (exportBusy) {
        report("Wait for the song export to finish before closing.");
        e->ignore();
        return;
    }
    if (!confirmDiscard()) {
        e->ignore();
        return;
    }
    QFile::remove(autosavePath);
    audio.close();
    supervisor.stop();
    e->accept();
}
int Window::smokeTest(const QString& screenshot) {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) {
            std::cerr << "GUI FAIL: " << message << '\n';
            ++failures;
        }
    };
    std::vector<bool> deletionAnswers;
    size_t nextDeletionAnswer = 0;
    int deletionDialogs = 0;
    QTimer deletionResponder;
    connect(&deletionResponder, &QTimer::timeout, this, [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "confirmNoteDeletion")
            return;
        ++deletionDialogs;
        check(dialog->defaultButton() == dialog->button(QMessageBox::Cancel),
              "note deletion defaults to Cancel");
        check(nextDeletionAnswer < deletionAnswers.size(),
              "every note deletion displays its expected warning");
        const bool accepted =
            nextDeletionAnswer < deletionAnswers.size() ? deletionAnswers[nextDeletionAnswer++] : false;
        dialog->button(accepted ? QMessageBox::Discard : QMessageBox::Cancel)->click();
    });
    deletionResponder.start(5);
    auto answerDeletion = [&](bool accepted) { deletionAnswers.push_back(accepted); };
    show();
    project = emptyProject();
    project.songBars = 8;
    timeline->selectedClip = 0;
    resetHistory();
    refreshProject();
    QTest::qWait(80);
    QPoint empty(Timeline::Header + int(timeline->scale * .5), Timeline::Ruler + 2 * Timeline::Row + 20);
    QTest::mouseDClick(timeline, Qt::LeftButton, Qt::NoModifier, empty);
    check(project.tracks[2].clips.size() == 1, "double click creates timeline clip");
    check(editor->phrase() != nullptr, "new clip opens note editor");
    double rh = (editor->height() - 24.) / 24;
    auto noteAt = [&](int pitch, int position = 0) {
        return QPoint(64 + int((editor->width() - 64.) / 16 * (position + .5)),
                      int(24 + (editor->baseNote + 23 - pitch + .5) * rh));
    };
    auto voice = [&](int pitch) -> const Note* {
        if (!editor->phrase())
            return nullptr;
        const auto& notes = editor->phrase()->steps[0].notes;
        auto found =
            std::find_if(notes.begin(), notes.end(), [pitch](const Note& n) { return n.note == pitch; });
        return found == notes.end() ? nullptr : &*found;
    };
    check(!note->isEnabled() && !velocity->isEnabled(), "empty step has no selected note to edit");
    check(!noteSoundOverride[0]->isEnabled() && !noteSoundValue[0]->isEnabled() &&
              !noteSoundReset->isEnabled(),
          "note sound controls are disabled without a selected note");
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, noteAt(64));
    check(voice(64) && voice(64)->velocity > 0, "draw note in clip");
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, noteAt(60));
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, noteAt(67));
    check(editor->phrase()->steps[0].notes.size() == 3 && voice(60) && voice(64) && voice(67),
          "stack three simultaneous pitches without replacing notes");
    const auto fullChord = projectToJson(project);
    const int beforeDeleteHistory = historyIndex;
    answerDeletion(false);
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, noteAt(64));
    check(projectToJson(project) == fullChord && historyIndex == beforeDeleteHistory,
          "canceling a click deletion preserves the entire project and history");
    answerDeletion(true);
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, noteAt(64));
    check(!voice(64) && voice(60) && voice(67) && editor->phrase()->steps[0].notes.size() == 2 &&
              !note->isEnabled() && !velocity->isEnabled(),
          "left-click erases only the targeted chord tone and clears its inspector");
    undo(-1);
    check(projectToJson(project) == fullChord, "undo restores a left-click deletion");
    undo(1);
    check(!voice(64) && voice(60) && voice(67), "redo reapplies a left-click deletion");
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, noteAt(64));
    check(voice(64) && voice(64)->velocity > 0 && editor->phrase()->steps[0].notes.size() == 3,
          "clicking the erased cell again adds its note");
    auto beforeSelection = projectToJson(project);
    QTest::mouseClick(editor, Qt::LeftButton, Qt::ShiftModifier, noteAt(62));
    check(projectToJson(project) == beforeSelection && !note->isEnabled(),
          "shift-click on empty space does not add a note");
    QTest::mouseClick(editor, Qt::LeftButton, Qt::ShiftModifier, noteAt(64));
    check(editor->selectedPitch == 64 && note->value() == 64 && projectToJson(project) == beforeSelection,
          "shift-click selects a chord tone without editing");
    auto inheritsSound = [](const Note* n) {
        return n && !n->overrides.level && !n->overrides.pan && !n->overrides.tone && !n->overrides.decay &&
               !n->overrides.drive;
    };
    check(inheritsSound(voice(64)) && !noteSoundOverride[3]->isChecked() && !noteSoundValue[3]->isEnabled(),
          "new notes inherit track sound settings");
    decay->setValue(79);
    check(noteSoundValue[3]->value() == 79 && inheritsSound(voice(64)),
          "changing track decay refreshes inherited note sound without creating overrides");
    noteSoundOverride[3]->setChecked(true);
    check(voice(64) && voice(64)->overrides.decay == .79 && noteSoundValue[3]->isEnabled(),
          "enabling a sound override pins the current inherited value");
    const std::array<int, 5> soundValues = {0, -40, 64, 22, 0};
    for (size_t i = 0; i < soundValues.size(); ++i) {
        noteSoundOverride[i]->setChecked(true);
        noteSoundValue[i]->setValue(soundValues[i]);
    }
    check(voice(64) && voice(64)->overrides.level == 0 && voice(64)->overrides.pan == -.4 &&
              voice(64)->overrides.tone == .64 && voice(64)->overrides.decay == .22 &&
              voice(64)->overrides.drive == 0 && inheritsSound(voice(60)) && inheritsSound(voice(67)),
          "all five sound controls affect only the selected chord tone and preserve explicit zero");
    volume->setValue(88);
    pan->setValue(25);
    tone->setValue(90);
    decay->setValue(60);
    drive->setValue(35);
    bool pinnedUnchanged = true;
    for (size_t i = 0; i < soundValues.size(); ++i)
        pinnedUnchanged &= noteSoundValue[i]->value() == soundValues[i];
    check(pinnedUnchanged, "track sound edits leave all pinned note controls unchanged");
    noteSoundOverride[0]->setChecked(false);
    check(voice(64) && !voice(64)->overrides.level && noteSoundValue[0]->value() == 88 &&
              !noteSoundValue[0]->isEnabled() && voice(64)->overrides.decay == .22,
          "unchecking zero level restores inheritance without clearing other sound overrides");
    noteSoundOverride[0]->setChecked(true);
    noteSoundValue[0]->setValue(0);
    QTest::mouseClick(editor, Qt::LeftButton, Qt::ShiftModifier, noteAt(60));
    noteSoundOverride[1]->setChecked(true);
    noteSoundValue[1]->setValue(-15);
    QTest::mouseClick(editor, Qt::LeftButton, Qt::ShiftModifier, noteAt(64));
    check(noteSoundValue[1]->value() == -40 && voice(60) && voice(60)->overrides.pan == -.15,
          "selecting another chord tone displays and edits its own independent sound settings");
    const auto soundEdited = projectToJson(project);
    noteSoundReset->click();
    check(inheritsSound(voice(64)) && voice(60) && voice(60)->overrides.pan == -.15 &&
              inheritsSound(voice(67)) && !noteSoundReset->isEnabled() && noteSoundValue[3]->value() == 60,
          "reset clears every selected-note sound override and displays track defaults");
    undo(-1);
    check(projectToJson(project) == soundEdited && noteSoundOverride[0]->isChecked() &&
              noteSoundValue[0]->value() == 0 && noteSoundValue[3]->value() == 22,
          "undo restores note sound overrides including explicit zero and inspector state");
    undo(1);
    check(inheritsSound(voice(64)), "redo reapplies note sound reset");
    undo(-1);
    note->setValue(65);
    velocity->setValue(88);
    check(voice(65) && !voice(64) && voice(60) && voice(67),
          "pitch inspector edits only selected chord tone");
    check(voice(65) && voice(65)->overrides.level == 0 && voice(65)->overrides.pan == -.4 &&
              voice(65)->overrides.tone == .64 && voice(65)->overrides.decay == .22 &&
              voice(65)->overrides.drive == 0,
          "changing pitch preserves all selected-note sound overrides");
    check(voice(65) && std::abs(voice(65)->velocity - 88. / 127) < .0001 && voice(60) &&
              std::abs(voice(60)->velocity - 100. / 127) < .0001 && voice(67) &&
              std::abs(voice(67)->velocity - 100. / 127) < .0001,
          "velocity inspector preserves sibling chord tones");
    auto beforeCollision = projectToJson(project);
    note->setValue(67);
    check(note->value() == 65 && editor->selectedPitch == 65 && projectToJson(project) == beforeCollision,
          "pitch collision is rejected without losing either chord tone");
    velocity->setValue(0);
    check(voice(65) && voice(65)->velocity == 0 && velocity->isEnabled(),
          "zero velocity retains the selected note for editing");
    beforeSelection = projectToJson(project);
    QTest::mouseClick(editor, Qt::LeftButton, Qt::ShiftModifier, noteAt(65));
    check(projectToJson(project) == beforeSelection && velocity->value() == 0,
          "shift-click selects a silent note without reactivating it");
    QTest::mouseClick(editor, Qt::LeftButton, Qt::NoModifier, noteAt(65));
    check(voice(65) && voice(65)->velocity > 0 && editor->phrase()->steps[0].notes.size() == 3,
          "normal click reactivates an invisible silent note instead of erasing it");
    velocity->setValue(88);
    QTest::mouseClick(editor, Qt::RightButton, Qt::NoModifier, noteAt(62));
    check(editor->phrase()->steps[0].notes.size() == 3, "erasing an empty pitch preserves its chord");
    const auto beforeRightDelete = projectToJson(project);
    answerDeletion(false);
    QTest::mouseClick(editor, Qt::RightButton, Qt::NoModifier, noteAt(65));
    check(projectToJson(project) == beforeRightDelete && voice(65) && voice(65)->overrides.decay == .22,
          "canceling right-click deletion preserves note sound settings");
    answerDeletion(true);
    QTest::mouseClick(editor, Qt::RightButton, Qt::NoModifier, noteAt(65));
    check(!voice(65) && voice(60) && voice(67) && editor->phrase()->steps[0].notes.size() == 2,
          "right-click erases only the targeted chord tone");
    undo(-1);
    check(voice(65) && voice(60) && voice(67) && editor->phrase()->steps[0].notes.size() == 3,
          "undo restores an erased chord tone and its siblings");
    check(voice(65) && voice(65)->overrides.level == 0 && voice(65)->overrides.decay == .22,
          "undo note deletion restores its sound overrides");
    auto* clearNotes = findChild<QPushButton*>("clearNotes");
    check(clearNotes != nullptr, "clear notes control exists");
    if (clearNotes) {
        const auto beforeClear = projectToJson(project);
        const int beforeClearHistory = historyIndex;
        answerDeletion(false);
        clearNotes->click();
        check(projectToJson(project) == beforeClear && historyIndex == beforeClearHistory,
              "canceling Clear notes preserves all notes, overrides and history");
        const int selectedBeforeClear = editor->selectedPitch;
        answerDeletion(true);
        QTimer::singleShot(0, this, [&] {
            editor->selectedPitch = 60;
            refreshSelection();
        });
        clearNotes->click();
        check(projectToJson(project) == beforeClear && historyIndex == beforeClearHistory,
              "a stale deletion confirmation cannot erase notes after selection changes");
        editor->selectedPitch = selectedBeforeClear;
        refreshSelection();
        answerDeletion(true);
        clearNotes->click();
    }
    check(editor->phrase()->steps[0].notes.empty() && !note->isEnabled(),
          "clear notes removes every chord tone and clears inspector");
    undo(-1);
    check(voice(65) && voice(60) && voice(67), "undo clear restores the full chord");
    auto& withPlaceholder = editor->phrase()->steps[0].notes;
    withPlaceholder.insert(withPlaceholder.begin(), Note{0, 64});
    change();
    QTest::mouseClick(editor, Qt::LeftButton, Qt::ShiftModifier, noteAt(65));
    const auto beforeSilentReplacement = projectToJson(project);
    answerDeletion(false);
    note->setValue(64);
    check(projectToJson(project) == beforeSilentReplacement && note->value() == 65,
          "canceling silent pitch replacement preserves both notes and restores pitch inspector");
    answerDeletion(true);
    note->setValue(64);
    check(voice(64) && std::abs(voice(64)->velocity - 88. / 127) < .0001 && !voice(65) && voice(60) &&
              voice(67) && editor->phrase()->steps[0].notes.size() == 3 && editor->selectedPitch == 64,
          "retuning replaces a silent legacy placeholder while preserving active siblings");
    beforeCollision = projectToJson(project);
    note->setValue(60);
    check(note->value() == 64 && projectToJson(project) == beforeCollision,
          "retuning still rejects an active sibling after replacing a silent placeholder");
    auto answerMove = [&](bool accepted, int bar, int step, bool expectCollision = false) {
        QTimer::singleShot(0, this, [&, accepted, bar, step, expectCollision] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog && dialog->objectName() == "moveNoteDialog", "Move note opens its command dialog");
            if (!dialog || dialog->objectName() != "moveNoteDialog")
                return;
            auto* barField = dialog->findChild<QSpinBox*>("moveNoteBar");
            auto* stepField = dialog->findChild<QSpinBox*>("moveNoteStep");
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            check(barField && stepField && buttons, "move dialog contains Bar, Step and action buttons");
            if (!barField || !stepField || !buttons) {
                dialog->reject();
                return;
            }
            check(barField->value() == editor->step / Steps + 1 &&
                      stepField->value() == editor->step % Steps + 1,
                  "move dialog defaults to the selected note's current location");
            barField->setValue(bar);
            stepField->setValue(step);
            buttons->button(accepted ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
            if (expectCollision) {
                auto* error = dialog->findChild<QLabel*>("moveNoteError");
                check(dialog->isVisible() && error && !error->text().isEmpty(),
                      "move dialog reports a pitch collision and stays open");
                dialog->reject();
            }
        });
    };
    const auto beforeMove = projectToJson(project);
    int beforeMoveHistory = historyIndex;
    check(!editor->moveSelectedNote(-1) && !editor->moveSelectedNote(Steps) &&
              projectToJson(project) == beforeMove && historyIndex == beforeMoveHistory,
          "out-of-phrase moves leave the entire project and history unchanged");
    answerMove(false, 1, 16);
    moveNoteButton->click();
    check(projectToJson(project) == beforeMove && historyIndex == beforeMoveHistory,
          "canceling Move note preserves all notes and history");
    answerMove(true, 1, 1);
    moveNoteAction->trigger();
    check(projectToJson(project) == beforeMove && historyIndex == beforeMoveHistory,
          "moving to the current step is a no-op without a history entry");
    const int beforeMoveDialogs = deletionDialogs;
    answerMove(true, 1, 16);
    moveNoteButton->click();
    const auto* movedNote = editor->selectedNote();
    check(editor->step == 15 && movedNote && movedNote->note == 64 &&
              std::abs(movedNote->velocity - 88. / 127) < .0001 && movedNote->overrides.level == 0 &&
              movedNote->overrides.pan == -.4 && movedNote->overrides.tone == .64 &&
              movedNote->overrides.decay == .22 && movedNote->overrides.drive == 0 && !voice(64) &&
              voice(60) && voice(60)->overrides.pan == -.15 && voice(67) &&
              deletionDialogs == beforeMoveDialogs,
          "Move note preserves every setting and sibling while moving to the final step without deletion");
    const auto afterMove = projectToJson(project);
    undo(-1);
    check(projectToJson(project) == beforeMove, "undo restores the complete note at its original step");
    undo(1);
    check(projectToJson(project) == afterMove, "redo restores the complete moved note");
    undo(-1);
    QTest::mouseClick(editor, Qt::LeftButton, Qt::ShiftModifier, noteAt(64));
    editor->phrase()->steps[15].notes = {{0, 64}};
    change();
    const auto beforeMoveCollision = projectToJson(project);
    beforeMoveHistory = historyIndex;
    answerMove(true, 1, 16, true);
    moveNoteAction->trigger();
    check(projectToJson(project) == beforeMoveCollision && historyIndex == beforeMoveHistory,
          "a move onto a silent same-pitch note preserves both notes and settings");
    undo(-1);
    resizePhrase(2);
    editor->phrase()->steps[20].notes = {*voice(64)};
    change();
    const auto beforeShrink = projectToJson(project);
    const int beforeShrinkHistory = historyIndex;
    answerDeletion(false);
    phraseLength->setValue(1);
    check(projectToJson(project) == beforeShrink && historyIndex == beforeShrinkHistory &&
              phraseLength->value() == 2 && timeline->selection()->lengthBars == 2,
          "canceling phrase shrink restores length controls and leaves clips, notes and history unchanged");
    answerDeletion(true);
    phraseLength->setValue(1);
    check(editor->phrase()->bars == 1 && editor->phrase()->steps.size() == Steps &&
              timeline->selection()->lengthBars == 1 && voice(64) && voice(64)->overrides.decay == .22,
          "confirmed phrase shrink removes only the truncated notes");
    undo(-1);
    check(projectToJson(project) == beforeShrink, "undo phrase shrink restores truncated notes and settings");
    undo(-1);
    undo(-1);
    check(projectToJson(project) == beforeMove,
          "temporary move and shrink exercises restore the original phrase");
    auto noteStroke = [&](int from, int to, int past) {
        QTest::mousePress(editor, Qt::LeftButton, Qt::NoModifier, noteAt(72, from));
        QTest::mouseMove(editor, noteAt(72, to), 20);
        QTest::mouseMove(editor, noteAt(72, past), 20);
        QTest::mouseMove(editor, noteAt(72, from), 20);
        QTest::mouseRelease(editor, Qt::LeftButton, Qt::NoModifier, noteAt(72, from));
    };
    noteStroke(1, 2, 2);
    check(editor->phrase()->steps[1].notes.size() == 1 && editor->phrase()->steps[2].notes.size() == 1,
          "drawing a stroke and revisiting its start keeps both notes");
    const auto beforeEraseStroke = projectToJson(project);
    const int beforeEraseStrokeHistory = historyIndex;
    int beforeStrokeDialogs = deletionDialogs;
    answerDeletion(false);
    noteStroke(1, 2, 3);
    check(projectToJson(project) == beforeEraseStroke && historyIndex == beforeEraseStrokeHistory &&
              deletionDialogs == beforeStrokeDialogs + 1,
          "canceling an erase stroke keeps all notes and shows only one warning");
    beforeStrokeDialogs = deletionDialogs;
    answerDeletion(true);
    noteStroke(1, 2, 3);
    check(editor->phrase()->steps[1].notes.empty() && editor->phrase()->steps[2].notes.empty() &&
              editor->phrase()->steps[3].notes.empty() && voice(60) && voice(64) && voice(67),
          "a stroke starting on a note only erases, including when revisiting erased cells");
    check(deletionDialogs == beforeStrokeDialogs + 1, "confirming an erase stroke uses one warning");
    undo(-1);
    check(editor->phrase()->steps[1].notes.size() == 1 && editor->phrase()->steps[2].notes.size() == 1,
          "one undo restores the entire erase stroke");
    undo(1);
    {
        const int priorTrack = timeline->track;
        const uint64_t priorClip = timeline->selectedClip;
        auto& sampleTrack = project.tracks[5];
        sampleTrack.patterns.resize(1);
        sampleTrack.patterns[0].steps[0].notes = {{.7, 48}, {0, 60}};
        sampleTrack.patterns[0].steps[0].notes[0].overrides.decay = .45;
        timeline->track = 5;
        timeline->selectedClip = 0;
        refreshSelection();
        change();
        const auto beforeSample = projectToJson(project);
        const int beforeSampleHistory = historyIndex;
        answerDeletion(false);
        check(!applySample("/unused-test-sample.wav") && projectToJson(project) == beforeSample &&
                  historyIndex == beforeSampleHistory,
              "canceling sample cleanup leaves sample path, notes, settings and history unchanged");
        answerDeletion(true);
        check(applySample("/unused-test-sample.wav") &&
                  project.tracks[5].patterns[0].steps[0].notes.size() == 1 &&
                  project.tracks[5].patterns[0].steps[0].notes[0].note == 60 &&
                  project.tracks[5].patterns[0].steps[0].notes[0].overrides.decay == .45,
              "confirmed sample cleanup removes only silent placeholders and preserves surviving settings");
        undo(-1);
        check(projectToJson(project) == beforeSample, "undo sample cleanup restores removed silent notes");
        undo(-1);
        timeline->track = priorTrack;
        timeline->selectedClip = priorClip;
        refreshSelection();
    }
    auto drag = [&](QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::mousePress(timeline, Qt::LeftButton, modifiers, from);
        QTest::mouseMove(timeline, to, 20);
        QTest::mouseRelease(timeline, Qt::LeftButton, modifiers, to);
    };
    int y = Timeline::Ruler + 2 * Timeline::Row + 20;
    drag(QPoint(Timeline::Header + int(timeline->scale) - 4, y),
         QPoint(Timeline::Header + int(4 * timeline->scale) - 4, y));
    check(timeline->selection() && timeline->selection()->lengthBars == 4, "drag clip edge repeats phrase");
    drag(QPoint(Timeline::Header + int(.4 * timeline->scale), y),
         QPoint(Timeline::Header + int(2.4 * timeline->scale), y));
    check(timeline->selection() && timeline->selection()->startBar == 2, "drag clip body moves in song");
    auto originalId = timeline->selectedClip;
    timeline->duplicateSelected();
    check(project.tracks[2].clips.size() == 2 && project.songBars == 10,
          "duplicate places clip and extends song");
    auto* clone = timeline->selection();
    check(clone && clone->pattern == project.tracks[2].clips.front().pattern,
          "copies initially share phrase");
    timeline->makeIndependent();
    check(timeline->selection()->pattern != project.tracks[2].clips.front().pattern,
          "make independent separates clip notes");
    undo(-1);
    check(project.tracks[2].clips.size() == 2 &&
              project.tracks[2].clips[0].pattern == project.tracks[2].clips[1].pattern,
          "undo independent clip edit");
    timeline->selectedClip = originalId;
    refreshSelection();
    drag(QPoint(Timeline::Header + int(2.4 * timeline->scale), y),
         QPoint(Timeline::Header + int(.4 * timeline->scale), y + Timeline::Row), Qt::ControlModifier);
    check(project.tracks[2].clips.size() == 2 && project.tracks[3].clips.size() == 1,
          "Ctrl drag copies between tracks");
    size_t beforeCtrlClick = project.tracks[3].clips.size();
    QTest::mouseClick(timeline, Qt::LeftButton, Qt::ControlModifier,
                      QPoint(Timeline::Header + int(.4 * timeline->scale), y + Timeline::Row));
    check(project.tracks[3].clips.size() == beforeCtrlClick,
          "Ctrl-click without dragging never overlaps a clip");
    size_t beforeRuler = project.tracks[0].clips.size();
    QTest::mouseDClick(timeline, Qt::LeftButton, Qt::NoModifier,
                       QPoint(Timeline::Header + int(timeline->scale), 12));
    check(project.tracks[0].clips.size() == beforeRuler,
          "ruler double-click does not create track-zero clips");
    stopPlayback();
    {
        Project names = emptyProject();
        names.tracks[0].name = QString(80, 'x');
        names.tracks[0].samplePath = "/unused-test-sample.wav";
        names.tracks[0].patterns.resize(4);
        Timeline scratch(names);
        scratch.createClip(0, 0);
        check(names.tracks[0].patterns.size() == 5, "creating a clip preserves unused imported phrases");
        NoteEditor sampleEditor(names);
        sampleEditor.select(0, int(names.tracks[0].patterns.size()) - 1);
        check(sampleEditor.defaultNote() == 60 && sampleEditor.baseNote == 60 &&
                  names.tracks[0].patterns.back().steps[0].notes.empty(),
              "empty sample clips open at original sample pitch");
        names.tracks[0].patterns.back().name = QString(80, 'p');
        scratch.makeIndependent();
        scratch.insertPhrase(0, 4, 1);
        Project parsed;
        QString why;
        check(projectFromJson(projectToJson(names), parsed, why),
              "long generated names and stored-phrase insertion remain valid");
        scratch.deleteSelected();
        check(names.tracks[0].patterns.size() == 6, "deleting a clip preserves stored phrases");
    }
    {
        Track sampleTrack;
        sampleTrack.patterns.resize(1);
        sampleTrack.patterns[0].steps[0].notes = {{.8, 60}, {.6, 64}, {.7, 67}};
        sampleTrack.patterns[0].steps[0].notes[1].overrides.decay = .42;
        sampleTrack.patterns[0].steps[1].notes = {{.5, 72}};
        check(prepareSamplePitches(sampleTrack) && sampleTrack.patterns[0].steps[0].notes.size() == 3 &&
                  sampleTrack.patterns[0].steps[0].notes[1].note == 64 &&
                  sampleTrack.patterns[0].steps[0].notes[1].overrides.decay == .42 &&
                  sampleTrack.patterns[0].steps[0].notes[2].note == 67 &&
                  sampleTrack.patterns[0].steps[1].notes[0].note == 72,
              "sample import preserves chord intervals and all other pitches");
        sampleTrack.patterns[0].steps[0].notes = {{.8, 36}, {0, 40}};
        sampleTrack.patterns[0].steps[0].notes[0].overrides.level = 0;
        sampleTrack.patterns[0].steps[0].notes[0].overrides.pan = -.3;
        check(!prepareSamplePitches(sampleTrack) && sampleTrack.patterns[0].steps[0].notes.size() == 1 &&
                  sampleTrack.patterns[0].steps[0].notes[0].note == 60 &&
                  sampleTrack.patterns[0].steps[0].notes[0].overrides.level == 0 &&
                  sampleTrack.patterns[0].steps[0].notes[0].overrides.pan == -.3 &&
                  sampleTrack.patterns[0].steps[1].notes[0].note == 60,
              "monophonic sample import retains original pitch behavior without duplicates");
        sampleTrack.patterns[0].steps[0].notes = {{0, 40}, {0, 36}};
        sampleTrack.patterns[0].steps[0].notes[1].overrides.level = 0;
        check(!prepareSamplePitches(sampleTrack) && sampleTrack.patterns[0].steps[0].notes.size() == 1 &&
                  sampleTrack.patterns[0].steps[0].notes[0].note == 60 &&
                  sampleTrack.patterns[0].steps[0].notes[0].velocity == 0 &&
                  sampleTrack.patterns[0].steps[0].notes[0].overrides.level == 0,
              "sample import keeps and retunes a silent note with an explicit zero override");
        sampleTrack.patterns[0].steps[0].notes = {{.8, 36}, {0, 40}};
        sampleTrack.patterns[0].steps[0].notes[1].overrides.pan = 0;
        sampleTrack.patterns[0].steps[1].notes = {{.5, 72}};
        check(prepareSamplePitches(sampleTrack) && sampleTrack.patterns[0].steps[0].notes.size() == 2 &&
                  sampleTrack.patterns[0].steps[0].notes[0].note == 36 &&
                  sampleTrack.patterns[0].steps[0].notes[1].note == 40 &&
                  sampleTrack.patterns[0].steps[0].notes[1].velocity == 0 &&
                  sampleTrack.patterns[0].steps[0].notes[1].overrides.pan == 0 &&
                  sampleTrack.patterns[0].steps[1].notes[0].note == 72,
              "sample import preserves all pitches when a silent sibling has an override");
    }
    QString error;
    QDir().mkpath("artifacts");
    check(saveProject("artifacts/gui-song.ferro", project, error), "save timeline song");
    Project loaded;
    check(loadProject("artifacts/gui-song.ferro", loaded, error) &&
              projectToJson(loaded) == projectToJson(project),
          "timeline save roundtrip");
    check(!loaded.tracks[2].patterns.empty() && loaded.tracks[2].patterns[0].steps[0].notes.size() == 3,
          "timeline save roundtrip retains all chord tones");
    if (!loaded.tracks[2].patterns.empty()) {
        const auto& savedNotes = loaded.tracks[2].patterns[0].steps[0].notes;
        auto savedNote =
            std::find_if(savedNotes.begin(), savedNotes.end(), [](const Note& n) { return n.note == 64; });
        check(savedNote != savedNotes.end() && savedNote->overrides.level == 0 &&
                  savedNote->overrides.pan == -.4 && savedNote->overrides.tone == .64 &&
                  savedNote->overrides.decay == .22 && savedNote->overrides.drive == 0,
              "save and reload retain every per-note sound override including zero");
    }
    check(!loop->isChecked() && !audio.looping(), "song transport loop defaults off");
    title->setFocus();
    QTest::keyClicks(title, " a b");
    check(title->text().endsWith(" a b"), "title accepts spaces");
    // Seeking while a newer tempo/length render is pending must wait for that
    // buffer's callback acknowledgement, preserving the requested musical bar.
    audio.clear();
    project = emptyProject();
    project.songBars = 4;
    project.tailSeconds = 0;
    auto scratchAudio = std::vector<float>(size_t(renderFrames(project)) * 2, .1f);
    audioRevision = audio.submit(scratchAudio, project.bpm);
    preparedState = projectToJson(project);
    std::vector<float> seekOut(4096);
    audio.processForTest(seekOut.data(), 2048);
    requestSeek(1);
    audio.setPlaying(true);
    audio.processForTest(seekOut.data(), 2048);
    tick();
    check(previewSeek < 0 && timeline->playBar > 1, "clock advances after seek beyond one GUI tick");
    project.bpm = 60;
    project.songBars = 8;
    requestSeek(5);
    check(queuedSeekBar == 5 && !audio.playing(), "pending tempo seek pauses and retains target bar");
    scratchAudio.assign(size_t(renderFrames(project)) * 2, .1f);
    audioRevision = audio.submit(scratchAudio, project.bpm);
    preparedState = projectToJson(project);
    audio.processForTest(seekOut.data(), 2048);
    tick();
    audio.processForTest(seekOut.data(), 2048);
    tick();
    check(queuedSeekBar < 0 && timeline->playBar > 5 && timeline->playBar < 5.1,
          "seek uses acknowledged longer render and its actual tempo");
    dirty = false;
    newSession(true);
    timeline->track = 0;
    timeline->selectedClip = project.tracks[0].clips.empty() ? 0 : project.tracks[0].clips.front().id;
    resetHistory();
    refreshProject();
    timeline->fitZoom(timelineScroll->viewport()->width());
    editor->fitNotes();
    renderTimer->start();
    QElapsedTimer deadline;
    deadline.start();
    do {
        QTest::qWait(40);
    } while ((!renderState->text().startsWith("READY") || deadline.elapsed() < 800) &&
             deadline.elapsed() < 25000);
    check(renderState->text().startsWith("READY"), "worker renders full timeline");
    check(!screenshot.isEmpty() ? grab().save(screenshot) : true, "screenshot");
    dirty = false;
    newSession(false);
    std::vector<float> out(1024);
    audio.setPlaying(true);
    audio.processForTest(out.data(), 512);
    check(audio.frames() == 0 && audio.peak() == 0, "new song cannot play old song");
    audio.setPlaying(false);
    Project recovered = demoProject();
    recovered.title = "Recovered song";
    recoverSession(recovered);
    undo(-1);
    check(dirty && project.title == "Recovered song", "recovery history preserves unsaved work");
    resize(1040, 720);
    QTest::qWait(30);
    auto* outer = qobject_cast<QScrollArea*>(centralWidget());
    check(outer && outer->widget()->height() > outer->viewport()->height(),
          "small window scrolls without hiding controls");
    check(nextDeletionAnswer == deletionAnswers.size(),
          "all expected deletion confirmations were displayed and answered");
    dirty = false;
    return failures;
}
} // namespace fc
