#include "timeline.h"
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace fc {
namespace {
const QColor colors[] = {QColor("#c9a77b"), QColor("#aeb788"), QColor("#79aa9b"),
                         QColor("#8faaba"), QColor("#ba9cab"), QColor("#a7b8b0")};
}
Timeline::Timeline(Project& p, QWidget* parent) : QWidget(parent), project(p) {
    setObjectName("timeline");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setToolTip("Double-click empty space: new clip. Drag body: move. Ctrl-drag: copy. Drag right edge: "
               "repeat phrase. Click ruler: seek. Right-click empty space: insert stored phrase.");
    scrollTimer = new QTimer(this);
    scrollTimer->setInterval(30);
    connect(scrollTimer, &QTimer::timeout, this, [this] { scrollDrag(); });
    refresh();
}
int Timeline::maxBars() const {
    return std::clamp(int(std::floor((MaxSongSeconds - project.tailSeconds) * project.bpm / 240.0)), 1,
                      MaxSongBars);
}
void Timeline::refresh() {
    setFixedSize(Header + int(std::max(16, project.songBars + 2) * scale), Ruler + TrackCount * Row);
    update();
}
Clip* Timeline::selection() {
    if (track < 0 || track >= TrackCount)
        return nullptr;
    for (auto& c : project.tracks[track].clips)
        if (c.id == selectedClip)
            return &c;
    return nullptr;
}
const Clip* Timeline::selection() const {
    return const_cast<Timeline*>(this)->selection();
}
bool Timeline::available(int t, int start, int length, uint64_t ignore) const {
    if (t < 0 || t >= TrackCount || start < 0 || length < 1 || start + length > maxBars())
        return false;
    for (auto& c : project.tracks[t].clips)
        if (c.id != ignore && start < c.startBar + c.lengthBars && start + length > c.startBar)
            return false;
    return true;
}
int Timeline::hit(const QPoint& point, int& t) const {
    t = (point.y() - Ruler) / Row;
    if (point.y() < Ruler || t < 0 || t >= TrackCount || point.x() < Header)
        return -1;
    double bar = (point.x() - Header) / scale;
    const auto& clips = project.tracks[t].clips;
    for (size_t i = 0; i < clips.size(); ++i)
        if (bar >= clips[i].startBar && bar < clips[i].startBar + clips[i].lengthBars)
            return int(i);
    return -1;
}
void Timeline::createClip(int t, int bar) {
    if (!available(t, bar, 1)) {
        if (message)
            message("A new clip needs an empty bar within the song limit.");
        return;
    }
    auto& tr = project.tracks[t];
    if (tr.patterns.size() >= 128 || tr.clips.size() >= 256) {
        if (message)
            message(tr.patterns.size() >= 128
                        ? "This track has 128 stored phrases. Insert an existing phrase or use another track."
                        : "This track has reached its 256-clip limit.");
        return;
    }
    Pattern phrase;
    QString suffix = QString(" %1").arg(tr.patterns.size() + 1);
    phrase.name = tr.name.left(80 - suffix.size()) + suffix;
    Clip c{nextClipId(project), int(tr.patterns.size()), bar, 1};
    tr.patterns.push_back(std::move(phrase));
    tr.clips.push_back(c);
    project.songBars = std::max(project.songBars, bar + 1);
    track = t;
    selectedClip = c.id;
    refresh();
    if (edited)
        edited(true);
}
void Timeline::insertPhrase(int t, int bar, int pattern) {
    if (t < 0 || t >= TrackCount || pattern < 0 || pattern >= int(project.tracks[t].patterns.size()))
        return;
    auto& tr = project.tracks[t];
    int length = tr.patterns[pattern].bars;
    if (!available(t, bar, length) || tr.clips.size() >= 256) {
        if (message)
            message("This phrase needs enough empty bars within the song limit and fewer than 256 clips.");
        return;
    }
    Clip c{nextClipId(project), pattern, bar, length};
    tr.clips.push_back(c);
    project.songBars = std::max(project.songBars, bar + length);
    track = t;
    selectedClip = c.id;
    refresh();
    if (edited)
        edited(true);
}
void Timeline::deleteSelected() {
    if (!selection())
        return;
    auto& clips = project.tracks[track].clips;
    clips.erase(std::remove_if(clips.begin(), clips.end(), [&](auto& c) { return c.id == selectedClip; }),
                clips.end());
    selectedClip = 0;
    refresh();
    if (edited)
        edited(true);
}
void Timeline::duplicateSelected() {
    auto* selected = selection();
    if (!selected)
        return;
    Clip copy = *selected;
    int bar = copy.startBar + copy.lengthBars;
    while (bar + copy.lengthBars <= maxBars() && !available(track, bar, copy.lengthBars))
        ++bar;
    if (!available(track, bar, copy.lengthBars) || project.tracks[track].clips.size() >= 256) {
        if (message)
            message("No room to duplicate this clip.");
        return;
    }
    copy.id = nextClipId(project);
    copy.startBar = bar;
    project.tracks[track].clips.push_back(copy);
    selectedClip = copy.id;
    project.songBars = std::max(project.songBars, bar + copy.lengthBars);
    refresh();
    if (edited)
        edited(true);
}
void Timeline::makeIndependent() {
    auto* c = selection();
    if (!c)
        return;
    auto& tr = project.tracks[track];
    if (tr.patterns.size() >= 128) {
        if (message)
            message("This track has 128 stored phrases. A new independent phrase needs another track.");
        return;
    }
    Pattern phrase = tr.patterns[c->pattern];
    phrase.name = phrase.name.left(75) + " copy";
    c->pattern = int(tr.patterns.size());
    tr.patterns.push_back(std::move(phrase));
    if (edited)
        edited(true);
    update();
}
void Timeline::fitZoom(int width) {
    scale = std::clamp((width - Header - 24.) / std::max(1, project.songBars), 12., 160.);
    refresh();
}
void Timeline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#181d1e"));
    int bars = int((width() - Header) / scale);
    for (int t = 0; t < TrackCount; ++t) {
        QRect row(0, Ruler + t * Row, width(), Row);
        p.fillRect(row, t % 2 ? QColor("#1e2425") : QColor("#1a2021"));
        p.fillRect(QRect(0, row.y(), Header, Row), t == track ? QColor("#303936") : QColor("#252d2c"));
        p.fillRect(QRect(0, row.y() + 7, 3, Row - 14), colors[t]);
        p.setPen(project.tracks[t].mute ? QColor("#79837c") : QColor("#d4dcd4"));
        p.drawText(QRect(12, row.y() + 3, Header - 20, 23), Qt::AlignVCenter, project.tracks[t].name);
        QFont small = font();
        small.setPointSize(7);
        p.setFont(small);
        p.setPen(QColor("#83958b"));
        p.drawText(QRect(12, row.y() + 24, Header - 20, 18), Qt::AlignVCenter,
                   project.tracks[t].samplePath.isEmpty() ? instrumentName(project.tracks[t].instrument)
                                                          : "Sample");
        p.setFont(font());
    }
    for (int bar = 0; bar <= bars; ++bar) {
        int x = Header + int(bar * scale);
        p.setPen(QColor(bar % 4 == 0 ? "#4b5752" : "#303a36"));
        p.drawLine(x, 0, x, height());
        if (bar % std::max(1, int(std::ceil(36 / scale))) == 0) {
            p.setPen(QColor("#a8b5a9"));
            p.drawText(QRect(x + 5, 0, int(scale * 2), Ruler - 3), Qt::AlignVCenter,
                       QString::number(bar + 1));
        }
    }
    auto drawClip = [&](const Clip& c, int t, bool preview) {
        const Pattern* phrase = nullptr;
        if (preview) {
            if (sourceTrack < 0 || sourceTrack >= TrackCount || original.pattern < 0 ||
                original.pattern >= int(project.tracks[sourceTrack].patterns.size()))
                return;
            phrase = &project.tracks[sourceTrack].patterns[original.pattern];
        } else
            phrase = &project.tracks[t].patterns[c.pattern];
        QRectF box(Header + c.startBar * scale + 1, Ruler + t * Row + 5, c.lengthBars * scale - 2, Row - 10);
        QColor color = colors[t];
        if (project.tracks[t].mute)
            color.setAlpha(80);
        else
            color.setAlpha(preview ? 100 : 175);
        p.setPen(QPen(preview && !valid      ? QColor("#ce786a")
                      : c.id == selectedClip ? QColor("#e8e2c9")
                                             : colors[t],
                      c.id == selectedClip ? 1.5 : 1));
        p.setBrush(color);
        p.drawRoundedRect(box, 3, 3);
        p.save();
        p.setClipRect(box.adjusted(3, 1, -3, -1));
        p.setPen(QColor("#18221f"));
        if (box.width() > 42)
            p.drawText(box.adjusted(7, 2, -6, -16), Qt::AlignVCenter,
                       phrase->name +
                           QString("  ·  %1 bar%2").arg(c.lengthBars).arg(c.lengthBars == 1 ? "" : "s"));
        for (int b = 0; b < c.lengthBars; ++b) {
            if (b % phrase->bars == 0 && b > 0) {
                p.setPen(QColor("#697c70"));
                p.drawLine(QPointF(Header + (c.startBar + b) * scale, box.top() + 2),
                           QPointF(Header + (c.startBar + b) * scale, box.bottom() - 2));
            }
            int sourceBar = b % phrase->bars;
            for (int s = 0; s < Steps; ++s) {
                for (const auto& note : phrase->steps[sourceBar * Steps + s].notes)
                    if (note.velocity > 0) {
                        double x = Header + (c.startBar + b + s / 16.) * scale;
                        p.setPen(QColor("#263c33"));
                        double y = box.bottom() - 4 - (note.note % 12) * .6;
                        p.drawLine(QPointF(x + 2, y), QPointF(x + std::max(2., scale / 32), y));
                    }
            }
        }
        p.restore();
        p.setPen(QPen(QColor("#d4ddc4"), 2));
        p.drawLine(QPointF(box.right() - 4, box.top() + 10), QPointF(box.right() - 4, box.bottom() - 10));
    };
    for (int t = 0; t < TrackCount; ++t)
        for (auto& c : project.tracks[t].clips)
            drawClip(c, t, false);
    if (drag == Move || drag == Repeat)
        drawClip(ghost, targetTrack, true);
    int end = Header + int(project.songBars * scale);
    p.fillRect(QRect(end, 0, std::max(0, width() - end), height()), QColor(8, 14, 13, 125));
    p.setPen(QPen(QColor("#b79672"), 2));
    p.drawLine(end, 0, end, height());
    p.drawText(QRect(end - 58, 2, 54, 20), Qt::AlignRight, "END");
    int x = Header + int(playBar * scale);
    p.setPen(QPen(QColor("#e2cda1"), 1.6));
    p.drawLine(x, 0, x, height());
    p.fillRect(QRect(0, 0, Header, Ruler), QColor("#1e2524"));
    p.setPen(QColor("#a3b3a7"));
    p.drawText(QRect(12, 0, Header - 12, Ruler), Qt::AlignVCenter, "TRACKS / ARRANGEMENT");
}
void Timeline::mousePressEvent(QMouseEvent* e) {
    setFocus(Qt::MouseFocusReason);
    QPoint pos = e->position().toPoint();
    drag = None;
    scrollTimer->stop();
    dragGlobal = e->globalPosition().toPoint();
    if (pos.y() < Ruler && pos.x() >= Header) {
        if (std::abs(pos.x() - (Header + project.songBars * scale)) < 10 && e->button() == Qt::LeftButton) {
            drag = End;
            scrollTimer->start();
            return;
        }
        if (seek)
            seek(std::clamp((pos.x() - Header) / scale, 0., double(project.songBars)));
        return;
    }
    int t = -1;
    int index = hit(pos, t);
    if (t < 0 || t >= TrackCount)
        return;
    track = t;
    if (index < 0) {
        selectedClip = 0;
        update();
        if (edited)
            edited(false);
        if (e->button() == Qt::RightButton && pos.x() >= Header && pos.y() >= Ruler) {
            const int bar = int((pos.x() - Header) / scale);
            QMenu menu;
            menu.addAction("New clip", this, [this, t, bar] { createClip(t, bar); });
            auto* stored = menu.addMenu("Insert phrase");
            const auto& phrases = project.tracks[t].patterns;
            stored->setEnabled(!phrases.empty());
            for (int i = 0; i < int(phrases.size()); ++i) {
                const auto& phrase = phrases[i];
                QString label = QString("%1. %2 (%3 bar%4)")
                                    .arg(i + 1)
                                    .arg(phrase.name)
                                    .arg(phrase.bars)
                                    .arg(phrase.bars == 1 ? "" : "s");
                auto* action = stored->addAction(label, this, [this, t, bar, i] { insertPhrase(t, bar, i); });
                action->setEnabled(available(t, bar, phrase.bars) && project.tracks[t].clips.size() < 256);
            }
            menu.exec(e->globalPosition().toPoint());
        }
        return;
    }
    selectedClip = project.tracks[t].clips[index].id;
    original = project.tracks[t].clips[index];
    sourceTrack = targetTrack = t;
    ghost = original;
    origin = pos;
    valid = true;
    if (edited)
        edited(false);
    update();
    if (e->button() == Qt::RightButton) {
        QMenu menu;
        menu.addAction("Duplicate clip", this, [this] { duplicateSelected(); });
        menu.addAction("Make independent", this, [this] { makeIndependent(); });
        menu.addSeparator();
        menu.addAction("Delete clip", this, [this] { deleteSelected(); });
        menu.exec(e->globalPosition().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    copy = e->modifiers() & Qt::ControlModifier;
    double right = Header + (original.startBar + original.lengthBars) * scale;
    drag = std::abs(pos.x() - right) < 10 ? Repeat : Move;
    scrollTimer->start();
}
void Timeline::mouseDoubleClickEvent(QMouseEvent* e) {
    int t = -1;
    int i = hit(e->position().toPoint(), t);
    drag = None;
    scrollTimer->stop();
    if (e->button() == Qt::LeftButton && e->position().y() >= Ruler && i < 0 && t >= 0 && t < TrackCount &&
        e->position().x() >= Header)
        createClip(t, int((e->position().x() - Header) / scale));
}
void Timeline::mouseMoveEvent(QMouseEvent* e) {
    QPoint pos = e->position().toPoint();
    dragGlobal = e->globalPosition().toPoint();
    if (drag == None) {
        int t = -1;
        int i = hit(pos, t);
        bool edge = i >= 0 && std::abs(pos.x() - (Header + (project.tracks[t].clips[i].startBar +
                                                            project.tracks[t].clips[i].lengthBars) *
                                                               scale)) < 10;
        setCursor(edge ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }
    updateDrag(pos);
}
void Timeline::updateDrag(const QPoint& pos) {
    if (drag == None)
        return;
    if (drag != End) {
        const auto& source = project.tracks[sourceTrack];
        if (original.startBar >= maxBars() ||
            std::none_of(source.clips.begin(), source.clips.end(),
                         [this](const auto& c) { return c.id == original.id; })) {
            drag = None;
            scrollTimer->stop();
            refresh();
            return;
        }
    }
    if (drag == End) {
        int end = std::clamp(int(std::round((pos.x() - Header) / scale)), contentEndBar(project), maxBars());
        if (end != project.songBars) {
            project.songBars = end;
            setFixedWidth(Header + int(std::max(16, project.songBars + 2) * scale));
        }
        update();
        return;
    }
    if (drag == Repeat) {
        ghost.lengthBars = std::clamp(int(std::round((pos.x() - Header) / scale)) - original.startBar, 1,
                                      maxBars() - original.startBar);
        targetTrack = sourceTrack;
    } else {
        ghost.startBar = std::max(0, original.startBar + int(std::round((pos.x() - origin.x()) / scale)));
        targetTrack = std::clamp((pos.y() - Ruler) / Row, 0, TrackCount - 1);
    }
    valid = available(targetTrack, ghost.startBar, ghost.lengthBars, copy ? 0 : original.id);
    const int visibleEnd = std::min(maxBars(), std::max(project.songBars, ghost.startBar + ghost.lengthBars));
    setFixedWidth(Header + int(std::max(16, visibleEnd + 2) * scale));
    update();
}
void Timeline::scrollDrag() {
    if (drag == None) {
        scrollTimer->stop();
        return;
    }
    for (QWidget* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        auto* area = qobject_cast<QScrollArea*>(ancestor);
        if (!area)
            continue;
        QPoint viewportPosition = area->viewport()->mapFromGlobal(dragGlobal);
        int delta = viewportPosition.x() < 28                               ? -20
                    : viewportPosition.x() > area->viewport()->width() - 28 ? 20
                                                                            : 0;
        if (delta) {
            auto* scroll = area->horizontalScrollBar();
            int previous = scroll->value();
            scroll->setValue(previous + delta);
            if (scroll->value() != previous)
                updateDrag(mapFromGlobal(dragGlobal));
        }
        break;
    }
}
void Timeline::mouseReleaseEvent(QMouseEvent*) {
    scrollTimer->stop();
    if (drag == End) {
        drag = None;
        refresh();
        if (edited)
            edited(true);
        return;
    }
    if (drag != Move && drag != Repeat)
        return;
    const bool changed = ghost.startBar != original.startBar || ghost.lengthBars != original.lengthBars ||
                         sourceTrack != targetTrack;
    if ((drag == Move || drag == Repeat) && changed) {
        const auto& source = project.tracks[sourceTrack];
        const bool exists = std::any_of(source.clips.begin(), source.clips.end(),
                                        [this](const auto& c) { return c.id == original.id; });
        valid = exists && original.pattern >= 0 && original.pattern < int(source.patterns.size()) &&
                available(targetTrack, ghost.startBar, ghost.lengthBars, copy ? 0 : original.id);
        if (!valid) {
            if (message)
                message("Clips on the same track cannot overlap. Choose an empty bar within the song limit.");
            drag = None;
            refresh();
            return;
        }
        if ((copy || targetTrack != sourceTrack) && project.tracks[targetTrack].clips.size() >= 256) {
            if (message)
                message("This track has reached its 256-clip limit.");
            drag = None;
            refresh();
            return;
        }
        if (targetTrack != sourceTrack && project.tracks[targetTrack].patterns.size() >= 128) {
            if (message)
                message("The destination track has 128 stored phrases. Choose another track.");
            drag = None;
            refresh();
            return;
        }
        if (targetTrack != sourceTrack) {
            Pattern phrase = project.tracks[sourceTrack].patterns[original.pattern];
            ghost.pattern = int(project.tracks[targetTrack].patterns.size());
            project.tracks[targetTrack].patterns.push_back(std::move(phrase));
        }
        if (copy)
            ghost.id = nextClipId(project);
        if (!copy) {
            auto& clips = project.tracks[sourceTrack].clips;
            clips.erase(
                std::remove_if(clips.begin(), clips.end(), [&](auto& c) { return c.id == original.id; }),
                clips.end());
        }
        project.tracks[targetTrack].clips.push_back(ghost);
        project.songBars = std::max(project.songBars, ghost.startBar + ghost.lengthBars);
        track = targetTrack;
        selectedClip = ghost.id;
        drag = None;
        refresh();
        if (edited)
            edited(true);
        return;
    }
    if (changed && !valid && message)
        message("Clips on the same track cannot overlap. Choose an empty bar.");
    drag = None;
    refresh();
}
void Timeline::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        deleteSelected();
        e->accept();
    } else
        QWidget::keyPressEvent(e);
}

NoteEditor::NoteEditor(Project& p, QWidget* parent) : QWidget(parent), project(p) {
    setObjectName("noteEditor");
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(250);
    setToolTip("Click empty space to add a note; click an existing note to erase it. Drag from empty space "
               "to draw, or from a note to erase. Shift-click to select without editing. Right-drag also "
               "erases individual notes. Stack pitches in the same step to make chords. Mouse wheel "
               "shifts the pitch range.");
    refresh();
}
Pattern* NoteEditor::phrase() {
    if (track < 0 || track >= TrackCount || pattern < 0 ||
        pattern >= int(project.tracks[track].patterns.size()))
        return nullptr;
    return &project.tracks[track].patterns[pattern];
}
Note* NoteEditor::selectedNote() {
    auto* p = phrase();
    if (!p || step < 0 || step >= int(p->steps.size()))
        return nullptr;
    auto& notes = p->steps[step].notes;
    auto found =
        std::find_if(notes.begin(), notes.end(), [this](const Note& n) { return n.note == selectedPitch; });
    return found == notes.end() ? nullptr : &*found;
}
int NoteEditor::defaultNote() const {
    constexpr int roots[] = {36, 36, 60, 60, 60, 72};
    if (track < 0 || track >= TrackCount)
        return 60;
    const auto& tr = project.tracks[track];
    return tr.samplePath.isEmpty() ? roots[tr.instrument] : 60;
}
void NoteEditor::select(int t, int p) {
    bool different = track != t || pattern != p;
    track = t;
    pattern = p;
    step = 0;
    selectedPitch = -1;
    if (auto* selected = phrase())
        for (const auto& n : selected->steps[0].notes)
            if (n.velocity > 0) {
                selectedPitch = n.note;
                break;
            }
    if (different)
        fitNotes();
    refresh();
}
void NoteEditor::fitNotes() {
    auto* p = phrase();
    if (!p)
        return;
    int lo = 128;
    for (const auto& s : p->steps)
        for (const auto& n : s.notes)
            if (n.velocity > 0)
                lo = std::min(lo, n.note);
    if (lo == 128)
        lo = defaultNote();
    baseNote = std::clamp((lo / 12) * 12, 0, 104);
    update();
}
void NoteEditor::refresh() {
    auto* p = phrase();
    setMinimumWidth(p ? 64 + int(p->steps.size()) * 26 : 720);
    setMinimumHeight(250);
    update();
}
void NoteEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#181e1f"));
    auto* phrase = this->phrase();
    if (!phrase) {
        p.setPen(QColor("#95a69a"));
        p.drawText(rect(), Qt::AlignCenter,
                   "Double-click a track above to create a clip, then draw notes here.");
        return;
    }
    double cw = (width() - 64.) / phrase->steps.size(), rh = (height() - 24.) / 24;
    for (int row = 0; row < 24; ++row) {
        int note = baseNote + 23 - row;
        int key = note % 12;
        bool black = key == 1 || key == 3 || key == 6 || key == 8 || key == 10;
        double y = 24 + row * rh;
        p.fillRect(QRectF(0, y, width(), rh), QColor(black ? "#191f20" : "#222a29"));
        p.fillRect(QRectF(0, y, 62, rh), QColor(black ? "#323d38" : "#afb8a9"));
        p.setPen(black ? QColor("#bdc9bc") : QColor("#26392e"));
        QFont f = font();
        f.setPointSize(7);
        p.setFont(f);
        p.drawText(QRectF(4, y, 54, rh), Qt::AlignVCenter, noteName(note));
    }
    for (size_t s = 0; s <= phrase->steps.size(); ++s) {
        double x = 64 + s * cw;
        p.setPen(QColor(s % 16 == 0 ? "#637467" : s % 4 == 0 ? "#455448" : "#2d3832"));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
        if (s < phrase->steps.size() && s % 4 == 0) {
            p.setPen(QColor("#9aa99c"));
            p.drawText(QRectF(x + 4, 0, cw * 4, 22), Qt::AlignVCenter,
                       QString("%1.%2").arg(s / 16 + 1).arg(s % 16 / 4 + 1));
        }
    }
    for (size_t s = 0; s < phrase->steps.size(); ++s)
        for (const auto& note : phrase->steps[s].notes) {
            if (note.velocity <= 0 || note.note < baseNote || note.note >= baseNote + 24)
                continue;
            double y = 24 + (baseNote + 23 - note.note) * rh;
            QRectF box(65 + s * cw, y + 1, cw - 2, rh - 2);
            QColor color = colors[track];
            color.setAlpha(100 + int(note.velocity * 150));
            p.setBrush(color);
            p.setPen(int(s) == step && note.note == selectedPitch ? QColor("#f1dfb9") : QColor("#8da58f"));
            p.drawRoundedRect(box, 2, 2);
        }
    if (playStep >= 0) {
        p.setPen(QPen(QColor("#d7c49a"), 1));
        double x = 64 + playStep * cw;
        p.drawLine(QPointF(x, 24), QPointF(x, height()));
    }
}
void NoteEditor::drawAt(QPoint pos, bool press, bool selectOnly) {
    auto* p = phrase();
    if (!p || pos.x() < 64 || pos.y() < 24)
        return;
    int s = int((pos.x() - 64) / ((width() - 64.) / p->steps.size())),
        n = baseNote + 23 - int((pos.y() - 24) / ((height() - 24.) / 24));
    if (s < 0 || s >= int(p->steps.size()) || n < baseNote || n > baseNote + 23)
        return;
    step = s;
    selectedPitch = n;
    if (s == lastStep && n == lastNote)
        return;
    lastStep = s;
    lastNote = n;
    auto& notes = p->steps[s].notes;
    auto found = std::find_if(notes.begin(), notes.end(), [n](const Note& note) { return note.note == n; });
    if (press && !selectOnly && found != notes.end() && found->velocity > 0)
        deleting = true;
    bool modified = false;
    if (selectOnly) {
        // Selection leaves both audible notes and legacy silent placeholders unchanged.
    } else if (deleting) {
        if (found != notes.end()) {
            notes.erase(found);
            modified = true;
        }
    } else if (found == notes.end()) {
        notes.push_back({100. / 127, n});
        modified = true;
    } else if (found->velocity == 0) {
        found->velocity = 100. / 127;
        modified = true;
    }
    changed |= modified;
    if (edited)
        edited(false);
    update();
}
void NoteEditor::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton && e->button() != Qt::RightButton)
        return;
    setFocus(Qt::MouseFocusReason);
    bool selectOnly = e->button() == Qt::LeftButton && e->modifiers().testFlag(Qt::ShiftModifier);
    drawing = !selectOnly;
    deleting = e->button() == Qt::RightButton;
    changed = false;
    lastStep = lastNote = -1;
    drawAt(e->position().toPoint(), true, selectOnly);
}
void NoteEditor::mouseMoveEvent(QMouseEvent* e) {
    if (drawing && e->buttons())
        drawAt(e->position().toPoint());
}
void NoteEditor::mouseReleaseEvent(QMouseEvent*) {
    drawing = false;
    if (edited)
        edited(changed);
    changed = false;
}
void NoteEditor::wheelEvent(QWheelEvent* e) {
    baseNote = std::clamp(baseNote + (e->angleDelta().y() > 0 ? 1 : -1), 0, 104);
    update();
    e->accept();
}
} // namespace fc
