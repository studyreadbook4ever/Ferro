#pragma once
#include "audio.h"
#include "project.h"
#include "supervisor.h"
#include "timeline.h"
#include <QMainWindow>
#include <QWidget>
#include <functional>
class QLabel;
class QTimer;
class QSpinBox;
class QDoubleSpinBox;
class QSlider;
class QPushButton;
class QLineEdit;
class QPlainTextEdit;
class QCheckBox;
class QComboBox;
class QScrollArea;
class QScrollBar;
class QAction;
namespace fc {
class AgentServer;
class Window : public QMainWindow {
  public:
    explicit Window(bool smoke = false);
    ~Window() override;
    int smokeTest(const QString& screenshot);
    bool openProjectPath(const QString& path, QString& error);

  protected:
    void closeEvent(QCloseEvent*) override;

  private:
    Project project = showcaseProject();
    AgentServer* agentServer = nullptr;
    AudioEngine audio;
    Supervisor supervisor;
    QString projectPath, autosavePath;
    bool dirty = false, updating = false, smoke = false, exportBusy = false;
    std::vector<Project> history;
    int historyIndex = 0;
    int64_t previewSeek = -1;
    double queuedSeekBar = -1;
    bool resumeAfterSeek = false;
    uint64_t audioRevision = 0;
    QJsonObject preparedState;
    QJsonObject savedState;
    Timeline* timeline = nullptr;
    NoteEditor* editor = nullptr;
    QScrollArea *timelineScroll = nullptr, *editorScroll = nullptr;
    QScrollBar* pitchScroll = nullptr;
    QTimer* renderTimer = nullptr;
    QLabel *clock = nullptr, *barClock = nullptr, *engineState = nullptr, *trackTitle = nullptr,
           *noteLabel = nullptr, *meterLabel = nullptr, *renderState = nullptr, *clipInfo = nullptr;
    QPlainTextEdit* workers = nullptr;
    QPushButton *playButton = nullptr, *muteButton = nullptr, *soloButton = nullptr, *sampleButton = nullptr;
    QLineEdit *title = nullptr, *trackName = nullptr, *phraseName = nullptr;
    QDoubleSpinBox *bpm = nullptr, *tail = nullptr;
    QSpinBox *note = nullptr, *velocity = nullptr, *songLength = nullptr, *phraseLength = nullptr;
    QComboBox* instrument = nullptr;
    QSlider *volume = nullptr, *pan = nullptr, *tone = nullptr, *decay = nullptr, *drive = nullptr,
            *master = nullptr, *delay = nullptr, *duck = nullptr, *zoom = nullptr;
    QCheckBox *click = nullptr, *loop = nullptr;
    std::array<QCheckBox*, 5> noteSoundOverride{};
    std::array<QSpinBox*, 5> noteSoundValue{};
    std::array<QSpinBox*, 5> trackSoundValue{};
    std::array<QSpinBox*, 3> masterSoundValue{};
    QPushButton* noteSoundReset = nullptr;
    QPushButton* moveNoteButton = nullptr;
    QAction* moveNoteAction = nullptr;
    void build();
    void change();
    void refreshSelection();
    void refreshNoteSound();
    void refreshProject();
    void tick();
    void togglePlay();
    void stopPlayback();
    void save(bool as = false);
    void open();
    void newSession(bool demo);
    void exportWave();
    bool confirmDiscard();
    bool confirmNoteDeletion(int count, const QString& reason);
    void moveNote();
    void report(const QString& message);
    void importSample();
    bool applySample(const QString& path);
    void resetHistory();
    void undo(int delta);
    void recoverSession(Project recovered);
    void resizePhrase(int bars);
    void requestSeek(double bar);
    void applyQueuedSeek();
    QJsonObject handleAgentRequest(const QJsonObject& request);
    void adoptProject(Project replacement, const QString& path = {}, bool showcasePreview = false);
    void selectShowcasePreview();
};
} // namespace fc
