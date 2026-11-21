#include "PluginEditorWindow.hpp"

#include "Controls.hpp"
#include "EngineController.hpp"
#include "SamplerPanel.hpp"
#include "Theme.hpp"

#include "Internal/SamplerInstance.hpp"

#include <QCloseEvent>
#include <cstdio>
#include <cstdlib>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QStyle>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

namespace {

/// Sliders are integers; parameters are doubles. One range for all of them, so
/// a 0…1 mix control and a 20…20000 Hz frequency get the same drag resolution.
constexpr int kSliderSteps = 1000;

/// How wide the parameter dock stands. Two knobs across, which is what fits
/// beside a plugin's own view without pushing it off the screen.
constexpr int kDockWidth = 208;

/// How often the fallback panel re-reads the plugin. Only for plugins with no
/// GUI of their own, and only while the window is open.
constexpr int kPollMs = 200;

} // namespace

PluginEditorWindow::PluginEditorWindow(daw::EngineController* controller,
                                       QString channelId, QString insertId,
                                       QWidget* parent)
    : QWidget(parent, Qt::Window), m_controller(controller),
      m_channelId(std::move(channelId)), m_insertId(std::move(insertId)) {
    setAttribute(Qt::WA_DeleteOnClose);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    buildWrapper();

    // Everything below the wrapper lives in one row: the plugin's own view (or
    // the fallback panel) on the left, our parameter dock on the right. A row
    // rather than a single slot because the dock has to be able to stand
    // *beside* a native view we are not allowed to draw into.
    m_content = new QWidget(this);
    m_contentRow = new QHBoxLayout(m_content);
    m_contentRow->setContentsMargins(0, 0, 0, 0);
    m_contentRow->setSpacing(0);
    m_layout->addWidget(m_content, 1);
    m_dock = buildParameterDock();
    m_contentRow->addWidget(m_dock);
    m_dock->setVisible(false);

    rebuildEditorContent();

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, [this] {
        refreshWrapper();
        refreshGenericEditor();
        refreshParameterDock();
    });
    m_poll->start();

    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &PluginEditorWindow::applyTheme);
    applyTheme();
}

PluginEditorWindow::~PluginEditorWindow() {
    // The plugin must let go of the view before Qt destroys it, or the plugin
    // is left drawing into freed memory.
    detachFromPlugin();
}

void PluginEditorWindow::buildWrapper() {
    m_wrapper = new QWidget(this);
    m_wrapper->setObjectName(QStringLiteral("PluginWrapper"));
    m_wrapper->setFixedHeight(38);
    auto* row = new QHBoxLayout(m_wrapper);
    row->setContentsMargins(7, 4, 7, 4);
    row->setSpacing(6);

    m_power = new ui::IconButton(icons::Glyph::Power,
                                 tr("Enable or bypass this plugin"), m_wrapper);
    m_power->setObjectName(QStringLiteral("PluginPower"));
    m_power->setCheckable(true);
    m_power->setButtonSize(28, 28);
    m_power->setAccessibleName(tr("Plugin enabled"));
    row->addWidget(m_power);

    auto* identity = new QWidget(m_wrapper);
    auto* identityLayout = new QVBoxLayout(identity);
    identityLayout->setContentsMargins(2, 0, 4, 0);
    identityLayout->setSpacing(0);
    m_pluginName = new QLabel(tr("Plugin"), identity);
    m_pluginName->setObjectName(QStringLiteral("PluginWrapperName"));
    m_pluginFormat = new QLabel(identity);
    m_pluginFormat->setObjectName(QStringLiteral("PluginWrapperFormat"));
    identityLayout->addWidget(m_pluginName);
    identityLayout->addWidget(m_pluginFormat);
    row->addWidget(identity, 1);

    m_dockToggle = new QToolButton(m_wrapper);
    m_dockToggle->setCheckable(true);
    m_dockToggle->setFixedSize(27, 27);
    m_dockToggle->setIcon(icons::icon(icons::Glyph::Automation, th().textPrimary));
    m_dockToggle->setToolTip(
        tr("Show this plugin's parameters — Alt/Option-double-click or "
           "right-click a knob to automate it"));
    m_dockToggle->setAccessibleName(tr("Show parameter panel"));
    connect(m_dockToggle, &QToolButton::toggled, this,
            [this](bool on) { setParameterDockVisible(on); });
    row->addWidget(m_dockToggle);

    m_channelMode = new QComboBox(m_wrapper);
    m_channelMode->setObjectName(QStringLiteral("PluginMode"));
    m_channelMode->setAccessibleName(tr("Plugin channel mode"));
    m_channelMode->setToolTip(
        tr("Choose automatic, mono, stereo, or two independent mono instances"));
    m_channelMode->addItem(tr("Auto"), int(daw::PluginChannelMode::Auto));
    m_channelMode->addItem(tr("Mono"), int(daw::PluginChannelMode::Mono));
    m_channelMode->addItem(tr("Stereo"), int(daw::PluginChannelMode::Stereo));
    m_channelMode->addItem(tr("Dual Mono"), int(daw::PluginChannelMode::DualMono));
    row->addWidget(m_channelMode);

    m_leftChannel = new QToolButton(m_wrapper);
    m_rightChannel = new QToolButton(m_wrapper);
    for (QToolButton* button : {m_leftChannel, m_rightChannel}) {
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setFixedSize(27, 27);
    }
    m_leftChannel->setText(QStringLiteral("L"));
    m_leftChannel->setToolTip(tr("Edit the left mono instance"));
    m_leftChannel->setAccessibleName(tr("Edit left dual-mono channel"));
    m_rightChannel->setText(QStringLiteral("R"));
    m_rightChannel->setToolTip(tr("Edit the right mono instance"));
    m_rightChannel->setAccessibleName(tr("Edit right dual-mono channel"));
    row->addWidget(m_leftChannel);
    row->addWidget(m_rightChannel);

    m_sidechain = new QComboBox(m_wrapper);
    m_sidechain->setObjectName(QStringLiteral("PluginSidechain"));
    m_sidechain->setAccessibleName(tr("Sidechain source"));
    m_sidechain->setMinimumWidth(150);
    row->addWidget(m_sidechain);
    m_layout->addWidget(m_wrapper);

    connect(m_power, &QAbstractButton::clicked, this, [this](bool enabled) {
        if (!m_controller || m_refreshingWrapper) return;
        m_controller->setInsertBypassed(m_channelId.toStdString(),
                                        m_insertId.toStdString(), !enabled);
        emit projectEdited();
    });
    connect(m_channelMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!m_controller || m_refreshingWrapper || index < 0) return;
        const auto mode = daw::PluginChannelMode(
            m_channelMode->itemData(index).toInt());
        detachFromPlugin();
        if (m_controller->setInsertChannelMode(m_channelId.toStdString(),
                                               m_insertId.toStdString(), mode)) {
            emit projectEdited();
        } else {
            m_channelMode->setToolTip(
                tr("This plugin cannot use the selected channel mode"));
        }
        rebuildEditorContent();
        refreshWrapper();
    });
    auto selectChannel = [this](daw::PluginEditorChannel channel, bool checked) {
        if (!checked || !m_controller || m_refreshingWrapper) return;
        detachFromPlugin();
        m_controller->setInsertEditorChannel(m_channelId.toStdString(),
                                             m_insertId.toStdString(), channel);
        emit projectEdited();
        rebuildEditorContent();
        refreshWrapper();
    };
    connect(m_leftChannel, &QToolButton::toggled, this,
            [selectChannel](bool checked) {
                selectChannel(daw::PluginEditorChannel::Left, checked);
            });
    connect(m_rightChannel, &QToolButton::toggled, this,
            [selectChannel](bool checked) {
                selectChannel(daw::PluginEditorChannel::Right, checked);
            });
    connect(m_sidechain, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!m_controller || m_refreshingWrapper || index < 0 ||
            !m_sidechain->isEnabled()) {
            return;
        }
        const std::string source = m_sidechain->itemData(index).toString().toStdString();
        if (m_controller->setInsertSidechainSource(m_channelId.toStdString(),
                                                   m_insertId.toStdString(), source)) {
            emit projectEdited();
        }
        refreshWrapper();
    });
    refreshWrapper();
}

void PluginEditorWindow::clearEditorContent() {
    detachFromPlugin();
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    if (m_container) {
        m_contentRow->removeWidget(m_container);
        delete m_container;
        m_container = nullptr;
    }
    if (m_generic) {
        m_contentRow->removeWidget(m_generic);
        delete m_generic;
        m_generic = nullptr;
    }
}

void PluginEditorWindow::rebuildEditorContent() {
    clearEditorContent();
    daw::plugins::PluginInstance* plugin = instance();
    QString title = tr("Plugin");
    if (plugin) {
        title = QString::fromStdString(plugin->descriptor().name);
        m_pluginUid = QString::fromStdString(plugin->descriptor().uid);
    }
    setWindowTitle(title);

    if (std::getenv("DAW_PLUGIN_DIAGNOSTICS")) {
        std::fprintf(stderr,
                     "editor for '%s' (%s): instance %s, hasEditor %s\n",
                     title.toUtf8().constData(),
                     plugin ? std::string(daw::plugins::toString(
                                              plugin->descriptor().format)).c_str()
                            : "-",
                     plugin ? "yes" : "NO",
                     plugin && plugin->hasEditor() ? "yes" : "no");
    }

    if (plugin && plugin->hasEditor()) {
        m_container = new QWidget(this);
        m_container->setAttribute(Qt::WA_NativeWindow);
        m_container->setAttribute(Qt::WA_DontCreateNativeAncestors);
        m_contentRow->insertWidget(0, m_container, 1);
        const WId handle = m_container->winId();
        m_embedded = plugin->openEditor(reinterpret_cast<void*>(handle), this);
        if (m_embedded) {
            m_openedOn = plugin;
            std::uint32_t width = 0, height = 0;
            if (plugin->editorSize(width, height) && width > 0 && height > 0) {
                m_applyingPluginSize = true;
                m_container->setFixedSize(int(width), int(height));
                resize(int(width) + dockWidth(),
                       int(height) + m_wrapper->height());
                m_applyingPluginSize = false;
            }
            if (plugin->editorCanResize()) {
                m_container->setMinimumSize(0, 0);
                m_container->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            } else {
                setFixedSize(size());
            }
        } else {
            m_contentRow->removeWidget(m_container);
            delete m_container;
            m_container = nullptr;
        }
    }

    if (!m_embedded &&
        dynamic_cast<daw::plugins::sampler::SamplerInstance*>(plugin)) {
        auto* samplerPanel =
            new SamplerPanel(m_controller, m_channelId, m_insertId, this);
        m_generic = samplerPanel;
        connect(samplerPanel, &SamplerPanel::pluginEditorRequested, this,
                &PluginEditorWindow::nestedPluginEditorRequested);
        connect(samplerPanel, &SamplerPanel::projectEdited, this,
                &PluginEditorWindow::projectEdited);
        connect(samplerPanel, &SamplerPanel::automationRequested, this,
                [this](const QString& parameterId) {
                    emit automationRequested(m_channelId, m_insertId, parameterId);
                });
        m_contentRow->insertWidget(0, m_generic, 1);
        setMinimumSize(860, 558);
        resize(1080, 688);
    } else if (!m_embedded) {
        buildGenericEditor();
    }
    // The dock exists to reach parameters *through* a foreign native view. With
    // no such view there is nothing it can do that the panel on screen does not
    // already do, so it is not offered.
    if (m_dockToggle) m_dockToggle->setVisible(m_embedded);
    if (!m_embedded && m_dock) {
        m_dockToggle->setChecked(false);
        m_dock->setVisible(false);
    }
    refreshWrapper();
}

void PluginEditorWindow::refreshWrapper() {
    if (!m_controller || !m_wrapper) return;
    const daw::InsertModel* model = m_controller->insertModel(
        m_channelId.toStdString(), m_insertId.toStdString());
    if (!model) return;
    m_refreshingWrapper = true;

    m_power->setChecked(!model->bypassed);
    const int modeIndex = m_channelMode->findData(int(model->channelMode));
    if (modeIndex >= 0) m_channelMode->setCurrentIndex(modeIndex);
    const bool dual = model->channelMode == daw::PluginChannelMode::DualMono;
    m_leftChannel->setVisible(dual);
    m_rightChannel->setVisible(dual);
    m_leftChannel->setChecked(model->editorChannel ==
                              daw::PluginEditorChannel::Left);
    m_rightChannel->setChecked(model->editorChannel ==
                               daw::PluginEditorChannel::Right);

    if (daw::plugins::PluginInstance* plugin = instance()) {
        m_pluginName->setText(QString::fromStdString(plugin->descriptor().name));
        m_pluginFormat->setText(QString::fromStdString(
            std::string(daw::plugins::toString(plugin->descriptor().format))));
    } else {
        m_pluginName->setText(QString::fromStdString(model->name));
        m_pluginFormat->setText(tr("not loaded"));
    }

    const bool supports = m_controller->insertSupportsSidechain(
        m_channelId.toStdString(), m_insertId.toStdString());
    QStringList signature;
    const auto sources = m_controller->insertSidechainSources(
        m_channelId.toStdString());
    signature << (supports ? QStringLiteral("supported")
                           : QStringLiteral("unsupported"));
    for (const auto& source : sources) {
        signature << QString::fromStdString(source.id);
    }
    const QString signatureText = signature.join(QLatin1Char('|'));
    if (m_sidechain->property("sourceSignature").toString() != signatureText) {
        m_sidechain->clear();
        if (!supports) {
            m_sidechain->addItem(tr("No sidechain input"));
        } else {
            m_sidechain->addItem(tr("Side Chain: Off"), QString());
            for (const auto& source : sources) {
                m_sidechain->addItem(
                    tr("Side Chain: %1").arg(QString::fromStdString(source.name)),
                    QString::fromStdString(source.id));
            }
        }
        m_sidechain->setProperty("sourceSignature", signatureText);
    }
    m_sidechain->setEnabled(supports);
    if (supports) {
        const QString current = QString::fromStdString(model->sidechainTrackId);
        int sourceIndex = m_sidechain->findData(current);
        if (sourceIndex < 0 && !current.isEmpty()) {
            m_sidechain->addItem(tr("Side Chain: missing source"), current);
            sourceIndex = m_sidechain->count() - 1;
        }
        m_sidechain->setCurrentIndex(std::max(sourceIndex, 0));
        m_sidechain->setToolTip(tr("Choose the post-fader signal sent to the plugin's auxiliary input"));
    } else {
        m_sidechain->setToolTip(tr("This plugin exposes no auxiliary audio input"));
    }
    m_refreshingWrapper = false;
}

void PluginEditorWindow::detachFromPlugin() {
    if (!m_embedded) return;
    m_embedded = false;
    // Only the instance the view was opened on may be told to close it. After a
    // Replace the slot holds a different plugin, and `closeEditor` on that one
    // would be a call about a window it never opened.
    if (daw::plugins::PluginInstance* plugin = instance();
        plugin && plugin == m_openedOn) {
        plugin->closeEditor();
    }
    m_openedOn = nullptr;
}

daw::plugins::PluginInstance* PluginEditorWindow::instance() const {
    if (!m_controller) return nullptr;
    return m_controller->insertInstance(m_channelId.toStdString(),
                                        m_insertId.toStdString());
}

void PluginEditorWindow::closeEvent(QCloseEvent* event) {
    emit closing(m_channelId, m_insertId);
    QWidget::closeEvent(event);
}

void PluginEditorWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!m_embedded || m_applyingPluginSize || !m_container) return;
    daw::plugins::PluginInstance* plugin = instance();
    if (!plugin || !plugin->editorCanResize()) return;

    // Offer the new size; the plugin snaps it to something it can draw and
    // writes back what it settled on, which the container then takes.
    std::uint32_t width = std::uint32_t(m_container->width());
    std::uint32_t height = std::uint32_t(m_container->height());
    if (!plugin->setEditorSize(width, height)) return;
    if (int(width) == m_container->width() && int(height) == m_container->height()) {
        return;
    }
    m_applyingPluginSize = true;
    m_container->resize(int(width), int(height));
    m_applyingPluginSize = false;
}

int PluginEditorWindow::dockWidth() const {
    return m_dock && m_dock->isVisible() ? kDockWidth : 0;
}

void PluginEditorWindow::onEditorResized(std::uint32_t width,
                                         std::uint32_t height) noexcept {
    if (!m_container || width == 0 || height == 0) return;
    m_applyingPluginSize = true;
    if (maximumSize() == minimumSize()) {
        // Fixed-size window: unpin, take the new size, pin again.
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        m_container->setFixedSize(int(width), int(height));
        const int totalWidth = int(width) + dockWidth();
        const int totalHeight = int(height) + (m_wrapper ? m_wrapper->height() : 0);
        resize(totalWidth, totalHeight);
        setFixedSize(totalWidth, totalHeight);
    } else {
        m_container->resize(int(width), int(height));
        resize(int(width) + dockWidth(),
               int(height) + (m_wrapper ? m_wrapper->height() : 0));
    }
    m_applyingPluginSize = false;
}

void PluginEditorWindow::onEditorClosed() noexcept {
    // The plugin closed itself. Tearing the window down from inside a plugin
    // callback would destroy the very object still on the stack, so it is
    // deferred to the event loop.
    QMetaObject::invokeMethod(this, [this] { close(); }, Qt::QueuedConnection);
}

double PluginEditorWindow::contentScaleFactor() const noexcept {
    return devicePixelRatioF();
}

// ── Fallback panel ─────────────────────────────────────────────────────────

QWidget* PluginEditorWindow::buildParameterDock() {
    auto* dock = new QWidget(m_content);
    dock->setObjectName(QStringLiteral("PluginParamDock"));
    dock->setFixedWidth(kDockWidth);
    auto* outer = new QVBoxLayout(dock);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(6);
    outer->addWidget(ui::sectionLabel(tr("PARAMETERS"), dock));

    auto* hint = new QLabel(
        tr("Alt/Option-double-click or right-click a knob to automate it"), dock);
    hint->setObjectName(QStringLiteral("PluginHint"));
    hint->setWordWrap(true);
    outer->addWidget(hint);

    auto* scroll = new QScrollArea(dock);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* grid = new QWidget(scroll);
    m_dockGrid = new QGridLayout(grid);
    m_dockGrid->setContentsMargins(0, 0, 0, 0);
    m_dockGrid->setHorizontalSpacing(4);
    m_dockGrid->setVerticalSpacing(6);

    const std::vector<daw::plugins::ParameterInfo> parameters =
        m_controller ? m_controller->insertParameters(m_channelId.toStdString(),
                                                      m_insertId.toStdString())
                     : std::vector<daw::plugins::ParameterInfo>{};
    for (const daw::plugins::ParameterInfo& parameter : parameters) {
        // A parameter the plugin says cannot be automated has no business on a
        // panel whose whole purpose is automating things.
        if (!parameter.isAutomatable) continue;
        const QString parameterId = QString::fromStdString(parameter.id);

        // One cell per parameter: a badge that says when the plugin is moving
        // this one, the knob, and the value in the plugin's own words. The
        // badge keeps its height while it is empty, so a parameter becoming
        // active does not shuffle the grid under the pointer.
        auto* cell = new QWidget(grid);
        cell->setObjectName(QStringLiteral("PluginParamCell"));
        cell->setProperty("active", false);
        cell->setProperty("parameterId", parameterId);
        auto* column = new QVBoxLayout(cell);
        column->setContentsMargins(3, 2, 3, 3);
        column->setSpacing(1);

        auto* badge = new QLabel(cell);
        badge->setObjectName(QStringLiteral("PluginParamBadge"));
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedHeight(11);
        column->addWidget(badge);

        auto* knob = new ui::Knob(QString::fromStdString(parameter.name), cell);
        knob->setRange(parameter.minValue, parameter.maxValue);
        knob->setDefaultValue(parameter.defaultValue);
        knob->setStepped(parameter.isStepped);
        knob->setCompact(true);
        knob->setAutomatable(true);
        // The caption elides at this width, so the full name has to be
        // reachable some other way.
        knob->setToolTip(QString::fromStdString(parameter.name));
        knob->setProperty("parameterId", parameterId);
        knob->setValue(m_controller ? m_controller->insertParameter(
                                          m_channelId.toStdString(),
                                          m_insertId.toStdString(), parameter.id)
                                    : parameter.defaultValue);
        // The plugin's own words for the value — "440 Hz", "2:1" — rather than
        // a raw number, which is often meaningless.
        knob->setFormatter([this, parameterId](double plain) -> QString {
            return parameterText(parameterId, plain);
        });
        connect(knob, &ui::Knob::valueChanged, this, [this, parameterId](double plain) {
            if (!m_controller) return;
            m_controller->setInsertParameter(m_channelId.toStdString(),
                                             m_insertId.toStdString(),
                                             parameterId.toStdString(), plain);
        });
        connect(knob, &ui::Knob::editFinished, this, [this] { emit projectEdited(); });
        connect(knob, &ui::Knob::automateRequested, this, [this, parameterId] {
            emit automationRequested(m_channelId, m_insertId, parameterId);
        });
        column->addWidget(knob);

        auto* value = new QLabel(cell);
        value->setObjectName(QStringLiteral("PluginParamValue"));
        value->setAlignment(Qt::AlignCenter);
        value->setText(parameterText(parameterId, knob->value()));
        column->addWidget(value);

        m_dockCells.append(cell);
    }
    if (m_dockCells.isEmpty()) {
        auto* empty = new QLabel(tr("Nothing here can be automated."), grid);
        empty->setObjectName(QStringLiteral("PluginHint"));
        empty->setWordWrap(true);
        m_dockGrid->addWidget(empty, 0, 0, 1, 2);
    }
    layOutParameterDock();
    scroll->setWidget(grid);
    outer->addWidget(scroll, 1);
    return dock;
}

QString PluginEditorWindow::parameterText(const QString& parameterId,
                                          double plain) const {
    if (daw::plugins::PluginInstance* live = instance()) {
        const std::int32_t index =
            live->parameterIndexForId(parameterId.toStdString());
        if (index >= 0) {
            const std::string text =
                live->parameterText(std::uint32_t(index), plain);
            if (!text.empty()) return QString::fromStdString(text);
        }
    }
    return QString::number(plain, 'g', 4);
}

void PluginEditorWindow::layOutParameterDock() {
    if (!m_dockGrid) return;
    for (QWidget* cell : m_dockCells) m_dockGrid->removeWidget(cell);
    int at = 0;
    for (QWidget* cell : m_dockCells) {
        m_dockGrid->addWidget(cell, at / 2, at % 2);
        ++at;
    }
    m_dockGrid->setRowStretch(m_dockGrid->rowCount(), 1);
}

void PluginEditorWindow::refreshParameterDock() {
    if (!m_dock || !m_dock->isVisible() || !m_controller) return;

    QString touched;
    for (QWidget* cell : m_dockCells) {
        auto* knob = cell->findChild<ui::Knob*>();
        if (!knob) continue;
        const QString parameterId = knob->property("parameterId").toString();
        const double plain = m_controller->insertParameter(
            m_channelId.toStdString(), m_insertId.toStdString(),
            parameterId.toStdString());
        // Never fight a knob the user is holding.
        if (knob->isEditing()) continue;
        if (std::abs(plain - knob->value()) > 1e-9) {
            // The value moved and it was not this knob that moved it — so it
            // was moved in the plugin's own GUI. That is the parameter the user
            // has their hand on, and the one worth putting at the top.
            touched = parameterId;
            QSignalBlocker block(knob);
            knob->setValue(plain);
        }
        if (auto* label = cell->findChild<QLabel*>(QStringLiteral("PluginParamValue"))) {
            label->setText(parameterText(parameterId, plain));
        }
    }
    if (touched.isEmpty() || touched == m_dockActive) return;

    m_dockActive = touched;
    for (QWidget* cell : m_dockCells) {
        const bool active = cell->property("parameterId").toString() == touched;
        if (cell->property("active").toBool() == active) continue;
        cell->setProperty("active", active);
        if (auto* badge =
                cell->findChild<QLabel*>(QStringLiteral("PluginParamBadge"))) {
            badge->setText(active ? tr("ACTIVE") : QString());
        }
        // A dynamic property only reaches the stylesheet after a repolish.
        cell->style()->unpolish(cell);
        cell->style()->polish(cell);
    }
    // To the front, so the one being turned is the one in view — a plugin with
    // eighty parameters is otherwise a scroll hunt every time.
    for (int i = 0; i < m_dockCells.size(); ++i) {
        if (m_dockCells.at(i)->property("parameterId").toString() != touched) continue;
        m_dockCells.move(i, 0);
        break;
    }
    layOutParameterDock();
}

void PluginEditorWindow::setParameterDockVisibleForTest(bool visible) {
    setParameterDockVisible(visible);
    if (m_dockToggle) {
        QSignalBlocker block(m_dockToggle);
        m_dockToggle->setChecked(visible);
    }
}

void PluginEditorWindow::pollForTest() {
    refreshWrapper();
    refreshGenericEditor();
    refreshParameterDock();
}

QStringList PluginEditorWindow::parameterDockOrderForTest() const {
    QStringList order;
    for (const QWidget* cell : m_dockCells) {
        order << cell->property("parameterId").toString();
    }
    return order;
}

void PluginEditorWindow::setParameterDockVisible(bool visible) {
    if (!m_dock || m_dock->isVisible() == visible) return;

    // The dock grows the *window*; it never takes width from what is already
    // in it. Taking it from the plugin's view crops the plugin — and since the
    // view is what the width is measured back from, every toggle would crop it
    // again. So the change is a fixed step on the window, symmetric, applied to
    // whatever width the window has right now.
    const int step = visible ? kDockWidth : -kDockWidth;
    const bool pinned = minimumSize() == maximumSize();
    const int wanted = std::max(200, width() + step);

    m_applyingPluginSize = true;
    if (pinned) {
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    }
    m_dock->setVisible(visible);
    resize(wanted, height());
    if (pinned) setFixedSize(wanted, height());
    m_applyingPluginSize = false;

    if (visible) refreshParameterDock();
}

void PluginEditorWindow::buildGenericEditor() {
    m_generic = new QWidget(this);
    auto* outer = new QVBoxLayout(m_generic);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(8);

    daw::plugins::PluginInstance* plugin = instance();
    const std::vector<daw::plugins::ParameterInfo> parameters =
        m_controller ? m_controller->insertParameters(m_channelId.toStdString(),
                                                      m_insertId.toStdString())
                     : std::vector<daw::plugins::ParameterInfo>{};

    auto* header = ui::sectionLabel(
        plugin ? tr("NO EDITOR — GENERIC CONTROLS") : tr("PLUGIN NOT LOADED"), m_generic);
    outer->addWidget(header);

    if (parameters.empty()) {
        auto* empty = new QLabel(tr("This plugin exposes no parameters."), m_generic);
        empty->setObjectName(QStringLiteral("PluginHint"));
        outer->addWidget(empty);
        outer->addStretch(1);
        m_contentRow->insertWidget(0, m_generic, 1);
        resize(420, 200);
        return;
    }

    auto* scroll = new QScrollArea(m_generic);
    scroll->setWidgetResizable(true);
    auto* rows = new QWidget(scroll);
    auto* grid = new QGridLayout(rows);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);

    int row = 0;
    for (const daw::plugins::ParameterInfo& parameter : parameters) {
        const QString parameterId = QString::fromStdString(parameter.id);
        auto* name = new QLabel(QString::fromStdString(parameter.name), rows);
        auto* slider = new QSlider(Qt::Horizontal, rows);
        slider->setRange(0, kSliderSteps);
        auto* value = new QLabel(rows);
        value->setObjectName(QStringLiteral("PluginHint"));
        value->setMinimumWidth(90);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        // The id, not the index: a plugin can renumber its parameters between
        // versions, and the id is what the document stores.
        slider->setProperty("parameterId", parameterId);
        slider->setProperty("minValue", parameter.minValue);
        slider->setProperty("maxValue", parameter.maxValue);
        slider->setProperty("valueLabel", QVariant::fromValue<QObject*>(value));

        const double current =
            m_controller ? m_controller->insertParameter(m_channelId.toStdString(),
                                                         m_insertId.toStdString(),
                                                         parameter.id)
                         : parameter.defaultValue;
        const double span = parameter.maxValue - parameter.minValue;
        const double fraction = span > 0.0 ? (current - parameter.minValue) / span : 0.0;
        slider->setValue(int(fraction * kSliderSteps));

        connect(slider, &QSlider::valueChanged, this, [this, slider](int position) {
            if (!m_controller) return;
            const double minimum = slider->property("minValue").toDouble();
            const double maximum = slider->property("maxValue").toDouble();
            const double plain =
                minimum + (maximum - minimum) * (double(position) / kSliderSteps);
            const std::string parameterId =
                slider->property("parameterId").toString().toStdString();
            m_controller->setInsertParameter(m_channelId.toStdString(),
                                             m_insertId.toStdString(), parameterId,
                                             plain);
            if (auto* label = qobject_cast<QLabel*>(
                    slider->property("valueLabel").value<QObject*>())) {
                if (daw::plugins::PluginInstance* live = instance()) {
                    const std::int32_t index = live->parameterIndexForId(parameterId);
                    if (index >= 0) {
                        label->setText(QString::fromStdString(
                            live->parameterText(std::uint32_t(index), plain)));
                    }
                }
            }
        });
        // Undo gets one entry per gesture, not one per pixel of drag — the
        // same split `commitLaneEdit` uses for automation.
        connect(slider, &QSlider::sliderPressed, this, [this, slider] {
            slider->setProperty(
                "beforeValue",
                m_controller ? m_controller->insertParameter(
                                   m_channelId.toStdString(), m_insertId.toStdString(),
                                   slider->property("parameterId").toString().toStdString())
                             : 0.0);
        });
        connect(slider, &QSlider::sliderReleased, this, [this, slider] {
            if (!m_controller) return;
            const QVariant before = slider->property("beforeValue");
            if (!before.isValid()) return;
            m_controller->commitInsertParameterEdit(
                m_channelId.toStdString(), m_insertId.toStdString(),
                slider->property("parameterId").toString().toStdString(),
                before.toDouble(), "Change Plugin Parameter");
        });

        // The dock is hidden in this branch — it would only repeat the list
        // that is already on screen — so the automate gesture gets a control of
        // its own on each row instead.
        auto* automate =
            new ui::IconButton(icons::Glyph::Automation,
                               tr("Automate this parameter"), rows);
        automate->setButtonSize(22, 22);
        automate->setEnabled(parameter.isAutomatable);
        connect(automate, &QAbstractButton::clicked, this, [this, parameterId] {
            emit automationRequested(m_channelId, m_insertId, parameterId);
        });

        grid->addWidget(name, row, 0);
        grid->addWidget(slider, row, 1);
        grid->addWidget(value, row, 2);
        grid->addWidget(automate, row, 3);
        ++row;
    }

    scroll->setWidget(rows);
    outer->addWidget(scroll, 1);
    m_contentRow->insertWidget(0, m_generic, 1);
    resize(520, std::min(640, 120 + row * 30));
    refreshGenericEditor();
}

void PluginEditorWindow::refreshGenericEditor() {
    if (!m_generic || !m_controller) return;
    daw::plugins::PluginInstance* plugin = instance();
    if (!plugin) return;

    for (QSlider* slider : m_generic->findChildren<QSlider*>()) {
        // Never fight the user: a slider being dragged owns its own value.
        if (slider->isSliderDown()) continue;
        const std::string parameterId =
            slider->property("parameterId").toString().toStdString();
        const double plain = m_controller->insertParameter(
            m_channelId.toStdString(), m_insertId.toStdString(), parameterId);
        const double minimum = slider->property("minValue").toDouble();
        const double maximum = slider->property("maxValue").toDouble();
        const double span = maximum - minimum;
        const int position =
            span > 0.0 ? int(((plain - minimum) / span) * kSliderSteps) : 0;
        if (position != slider->value()) {
            const QSignalBlocker block(slider);
            slider->setValue(position);
        }
        if (auto* label = qobject_cast<QLabel*>(
                slider->property("valueLabel").value<QObject*>())) {
            const std::int32_t index = plugin->parameterIndexForId(parameterId);
            if (index >= 0) {
                label->setText(QString::fromStdString(
                    plugin->parameterText(std::uint32_t(index), plain)));
            }
        }
    }
}

void PluginEditorWindow::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
PluginEditorWindow { background: %BG%; }
#PluginWrapper {
    background: %SURFACE%;
    border-bottom: 1px solid %SEPARATOR%;
}
#PluginWrapperName { color: %TEXT%; font-size: 12px; font-weight: 600; }
#PluginWrapperFormat { color: %TEXT2%; font-size: 9px; text-transform: uppercase; }
#PluginMode, #PluginSidechain {
    color: %TEXT%;
    background: %WELL%;
    border: 1px solid %SEPARATOR%;
    border-radius: 5px;
    padding: 3px 24px 3px 8px;
    min-height: 20px;
}
#PluginMode:hover, #PluginSidechain:hover { border-color: %ACCENT%; }
#PluginMode:focus, #PluginSidechain:focus { border: 1px solid %ACCENT%; }
#PluginMode QAbstractItemView, #PluginSidechain QAbstractItemView {
    color: %TEXT%;
    background: %ELEVATED%;
    border: 1px solid %SEPARATOR%;
    selection-background-color: %SELECTION%;
    selection-color: %TEXT%;
    outline: none;
}
#PluginWrapper QToolButton {
    color: %TEXT2%;
    background: %WELL%;
    border: 1px solid %SEPARATOR%;
    border-radius: 5px;
    font-weight: 700;
}
#PluginWrapper QToolButton:hover { color: %TEXT%; border-color: %ACCENT%; }
#PluginWrapper QToolButton:checked {
    color: %TEXT%;
    background: %SELECTION%;
    border-color: %ACCENT%;
}
#PluginHint { color: %TEXT2%; font-size: 11px; }
#PluginParamDock {
    background: %SURFACE%;
    border-left: 1px solid %SEPARATOR%;
}
#PluginParamCell {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
}
#PluginParamCell[active="true"] {
    background: %WELL%;
    border: 1px solid %ACCENT%;
}
#PluginParamBadge {
    color: %ACCENT%;
    font-size: 8px;
    font-weight: 700;
    letter-spacing: 1px;
}
#PluginParamValue { color: %TEXT2%; font-size: 9px; }
)")
        .replace("%BG%", t.background.name())
        .replace("%SURFACE%", t.surface.name())
        .replace("%ELEVATED%", t.surfaceElevated.name())
        .replace("%WELL%", t.well().name())
        .replace("%SEPARATOR%", t.separator().name())
        .replace("%SELECTION%", t.selection.name())
        .replace("%ACCENT%", t.accent.name())
        .replace("%TEXT%", t.textPrimary.name())
        .replace("%TEXT2%", t.textSecondary.name()));
}
