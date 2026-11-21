#include "WaveformStrip.hpp"

#include "Theme.hpp"
#include "WaveformPaint.hpp"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace {
/// Enough height for the envelope to say something about the sound, short
/// enough that the tree above it keeps the panel.
constexpr int kHeight = 56;
} // namespace

WaveformStrip::WaveformStrip(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kHeight);
    setCursor(Qt::PointingHandCursor);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void WaveformStrip::setPeaks(const daw::WaveformPeaks& peaks) {
    m_peaks = peaks;
    m_message.clear();
    m_playheadSeconds = -1.0;
    update();
}

void WaveformStrip::clear(const QString& message) {
    m_peaks = daw::WaveformPeaks{};
    m_message = message;
    m_playheadSeconds = -1.0;
    update();
}

void WaveformStrip::setPlayheadSeconds(double seconds) {
    // A playhead that moves less than a pixel is not worth a repaint: this is
    // polled several times a second while a file auditions.
    const double perPixel =
        width() > 0 && m_peaks.durationSeconds > 0.0
            ? m_peaks.durationSeconds / double(width())
            : 0.0;
    if (perPixel > 0.0 && m_playheadSeconds >= 0.0 && seconds >= 0.0 &&
        std::abs(seconds - m_playheadSeconds) < perPixel) {
        m_playheadSeconds = seconds;
        return;
    }
    m_playheadSeconds = seconds;
    update();
}

double WaveformStrip::secondsAt(double x) const {
    if (width() <= 0 || m_peaks.durationSeconds <= 0.0) return 0.0;
    const double fraction = std::clamp(x / double(width()), 0.0, 1.0);
    return fraction * m_peaks.durationSeconds;
}

void WaveformStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const Theme& t = th();
    const QRectF area = rect().adjusted(0, 0, -1, -1);
    p.fillRect(rect(), t.well());
    p.setPen(QPen(t.separator(), 1.0));
    p.drawLine(area.topLeft(), area.topRight());

    if (!m_peaks.isValid()) {
        if (!m_message.isEmpty()) {
            p.setPen(t.textSecondary);
            p.drawText(area, Qt::AlignCenter, m_message);
        }
        return;
    }

    // A centre line, so a quiet file still reads as audio rather than as an
    // empty box.
    p.setPen(QPen(mixColors(t.well(), t.textSecondary, 0.25), 1.0));
    p.drawLine(QPointF(area.left(), area.center().y()),
               QPointF(area.right(), area.center().y()));

    ui::PeakPaint how;
    how.sourceStartSeconds = 0.0;
    how.secondsPerPixel = m_peaks.durationSeconds / std::max(1.0, double(width()));
    how.clipLeft = 0.0;
    how.clipRight = double(width());
    how.gain = 1.0f;
    how.color = t.waveform;
    ui::paintPeaks(p, &m_peaks, area.adjusted(0, 3, 0, -3), how);

    if (m_playheadSeconds >= 0.0 && m_peaks.durationSeconds > 0.0) {
        const double x = std::clamp(m_playheadSeconds / m_peaks.durationSeconds,
                                    0.0, 1.0) *
                         double(width());
        p.setPen(QPen(t.cursor, 1.5));
        p.drawLine(QPointF(x, area.top() + 1.0), QPointF(x, area.bottom()));
    }
}

void WaveformStrip::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton || !m_peaks.isValid()) return;
    emit seekRequested(secondsAt(ev->position().x()));
    ev->accept();
}

void WaveformStrip::mouseMoveEvent(QMouseEvent* ev) {
    // Scrubbing: the same gesture continued, so dragging along the strip moves
    // the audition rather than needing a click per position.
    if (!(ev->buttons() & Qt::LeftButton) || !m_peaks.isValid()) return;
    emit seekRequested(secondsAt(ev->position().x()));
    ev->accept();
}
