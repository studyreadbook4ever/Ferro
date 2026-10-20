#pragma once
#include "project.h"
#include <QWidget>
#include <functional>

class QTimer;

namespace fc {
class Timeline : public QWidget {
  public:
    explicit Timeline(Project& project, QWidget* parent = nullptr);
    int track = 0;
    uint64_t selectedClip = 0;
    double playBar = 0;
    double scale = 72;
    std::function<void(bool)> edited;
    std::function<void(double)> seek;
    std::function<void(QString)> message;
    void refresh();
    Clip* selection();
    const Clip* selection() const;
    void createClip(int track, int bar);
    void insertPhrase(int track, int bar, int pattern);
    void deleteSelected();
    void duplicateSelected();
    void makeIndependent();
    void fitZoom(int viewportWidth);
    static constexpr int Header = 160, Ruler = 32, Row = 49;

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

  private:
    Project& project;
    enum Drag { None, Move, Repeat, End } drag = None;
    Clip original, ghost;
    int sourceTrack = 0, targetTrack = 0;
    bool copy = false, valid = true;
    QPoint origin;
    QPoint dragGlobal;
    QTimer* scrollTimer = nullptr;
    int hit(const QPoint&, int& track) const;
    bool available(int track, int start, int length, uint64_t ignore = 0) const;
    int maxBars() const;
    void updateDrag(const QPoint& position);
    void scrollDrag();
};

class NoteEditor : public QWidget {
  public:
    explicit NoteEditor(Project& project, QWidget* parent = nullptr);
    int track = -1, pattern = -1, step = 0, selectedPitch = -1, baseNote = 48;
    double playStep = -1;
    std::function<void(bool)> edited;
    void select(int track, int pattern);
    void refresh();
    void fitNotes();
    Pattern* phrase();
    Note* selectedNote();
    int defaultNote() const;

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

  private:
    Project& project;
    bool drawing = false, deleting = false, changed = false;
    int lastStep = -1, lastNote = -1;
    void drawAt(QPoint, bool press = false, bool selectOnly = false);
};
} // namespace fc
