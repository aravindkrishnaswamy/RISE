//////////////////////////////////////////////////////////////////////
//
//  NamedViewsPanel.cpp - Right-panel Named Views section (Windows Qt6).
//    Mirrors macOS NamedViewsPanel.swift.
//
//////////////////////////////////////////////////////////////////////

#include "NamedViewsPanel.h"
#include "ViewportBridge.h"
#include "Theme.h"

#include <QColor>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

// Pure-input clickable header row (no Q_OBJECT / moc entry needed),
// matching EnvironmentPanel.cpp and OutlinerWidget.cpp.
class ClickableHeaderRow : public QWidget
{
public:
    using ClickFn = std::function<void()>;

    explicit ClickableHeaderRow(ClickFn onClick, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_onClick(std::move(onClick))
    {
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && m_onClick) {
            m_onClick();
        }
        QWidget::mouseReleaseEvent(e);
    }

private:
    ClickFn m_onClick;
};

QColor withAlpha(const QColor& c, double alphaFraction)
{
    QColor result = c;
    result.setAlphaF(alphaFraction);
    return result;
}

QString captureButtonStyleSheet()
{
    return QStringLiteral(
        "QPushButton { color: %1; background-color: %2; border: none; border-radius: %3px; padding: 4px 10px; }"
        "QPushButton:disabled { color: %4; background-color: %5; }")
        .arg(Theme::hex(Theme::accentLight),
             Theme::rgba(withAlpha(Theme::accent, 0.16)))
        .arg(Theme::radiusMedium)
        .arg(Theme::hex(Theme::textDisabled),
             Theme::rgba(Theme::fillHover));
}

QString captureEditStyleSheet()
{
    return QStringLiteral(
        "QLineEdit { color: %1; background-color: %2; border: 1px solid %3; border-radius: %4px; padding: 3px 7px; }"
        "QLineEdit:disabled { color: %5; }")
        .arg(Theme::hex(Theme::textPrimary),
             Theme::hex(Theme::bgWell),
             Theme::hex(Theme::borderLight))
        .arg(Theme::radiusSmall)
        .arg(Theme::hex(Theme::textDisabled));
}

}  // namespace

NamedViewsPanel::NamedViewsPanel(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ---- Disclosure header --------------------------------------------
    auto* header = new ClickableHeaderRow([this]() {
        m_expanded = !m_expanded;
        if (m_headerLayout) {
            m_headerLayout->setContentsMargins(14, 12, 14, m_expanded ? 8 : 12);
        }
        if (m_chevronLabel) {
            Theme::bindIconLabel(m_chevronLabel,
                m_expanded ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"),
                9, []{ return Theme::textDim; });
        }
        if (m_contentWidget) {
            m_contentWidget->setVisible(m_expanded);
        }
    }, this);

    m_headerLayout = new QHBoxLayout(header);
    m_headerLayout->setContentsMargins(14, 12, 14, 8);
    m_headerLayout->setSpacing(8);

    m_chevronLabel = new QLabel(header);
    m_chevronLabel->setFixedSize(10, 10);
    m_chevronLabel->setAlignment(Qt::AlignCenter);
    Theme::bindIconLabel(m_chevronLabel, QStringLiteral("chevron-down"),
                         9, []{ return Theme::textDim; });
    m_headerLayout->addWidget(m_chevronLabel);

    m_titleLabel = new QLabel(tr("Named Views"), header);
    m_titleLabel->setFont(Theme::sans(12, QFont::DemiBold));
    m_headerLayout->addWidget(m_titleLabel);
    m_headerLayout->addStretch(1);

    m_countLabel = new QLabel(header);
    m_countLabel->setFont(Theme::mono(10));
    m_headerLayout->addWidget(m_countLabel);

    rootLayout->addWidget(header);

    // ---- Body (persistent capture row + dynamic bookmark list) --------
    m_contentWidget = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(m_contentWidget);
    contentLayout->setContentsMargins(14, 0, 14, 12);
    contentLayout->setSpacing(6);

    auto* captureRow = new QHBoxLayout();
    captureRow->setContentsMargins(0, 0, 0, 0);
    captureRow->setSpacing(6);

    m_captureEdit = new QLineEdit(m_contentWidget);
    m_captureEdit->setPlaceholderText(tr("View name"));
    m_captureEdit->setFont(Theme::sans(11));
    captureRow->addWidget(m_captureEdit, 1);

    m_captureBtn = new QPushButton(tr("Capture"), m_contentWidget);
    m_captureBtn->setFont(Theme::sans(11, QFont::DemiBold));
    m_captureBtn->setCursor(Qt::PointingHandCursor);
    m_captureBtn->setFlat(true);
    captureRow->addWidget(m_captureBtn);

    contentLayout->addLayout(captureRow);

    m_emptyLabel = new QLabel(tr("No saved views. Frame a shot, then Capture."), m_contentWidget);
    m_emptyLabel->setFont(Theme::sans(11));
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setContentsMargins(0, 2, 0, 2);
    contentLayout->addWidget(m_emptyLabel);

    m_rowsContainer = new QWidget(m_contentWidget);
    m_rowsLayout = new QVBoxLayout(m_rowsContainer);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(2);
    m_rowsContainer->setVisible(false);
    contentLayout->addWidget(m_rowsContainer);

    rootLayout->addWidget(m_contentWidget);

    connect(m_captureBtn, &QPushButton::clicked, this, &NamedViewsPanel::onCaptureClicked);
    connect(m_captureEdit, &QLineEdit::returnPressed, this, &NamedViewsPanel::onCaptureClicked);

    m_contentWidget->setEnabled(false);
    setVisible(false);

    // LIVE THEME-SWITCH CONTRACT (Theme.h): style persistent chrome once
    // at construction and re-invoke on QEvent::PaletteChange.
    m_themeReady = true;
    restyleTheme();
}

void NamedViewsPanel::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void NamedViewsPanel::restyleTheme()
{
    m_themeEpochSeen = Theme::paletteEpoch();

    setStyleSheet(QStringLiteral("border-bottom: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Theme::bgPanel);
    setPalette(pal);

    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QStringLiteral("border: none; color: %1;").arg(Theme::hex(Theme::textPrimary)));
    }
    if (m_countLabel) {
        m_countLabel->setStyleSheet(QStringLiteral("border: none; color: %1;").arg(Theme::hex(Theme::textDim)));
    }
    if (m_captureEdit) {
        m_captureEdit->setStyleSheet(captureEditStyleSheet());
    }
    if (m_captureBtn) {
        m_captureBtn->setStyleSheet(captureButtonStyleSheet());
    }
    if (m_emptyLabel) {
        m_emptyLabel->setStyleSheet(QStringLiteral("border: none; color: %1;").arg(Theme::hex(Theme::textDim)));
    }

    // LIVE THEME-SWITCH CONTRACT point 5 (Theme.h): defer row widget
    // recreation out of the synchronous PaletteChange cascade.  rebuildRows()
    // reads only cached m_names (no bridge mutex round-trip).
    QTimer::singleShot(0, this, [this]() {
        if (m_bridge && m_rowsBuilt) {
            rebuildRows();
        }
    });
}

void NamedViewsPanel::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    m_names.clear();
    m_rowsBuilt = false;
    if (!m_bridge) {
        if (m_captureEdit) m_captureEdit->clear();
        if (m_countLabel) m_countLabel->clear();
        rebuildRows();
        setVisible(false);
        return;
    }
    setVisible(true);
    reload();
}

void NamedViewsPanel::setSceneEditable(bool editable)
{
    if (m_sceneEditable == editable) return;
    m_sceneEditable = editable;
    if (m_contentWidget) {
        m_contentWidget->setEnabled(m_sceneEditable);
    }
    if (m_sceneEditable && m_bridge) {
        reload();
    }
}

void NamedViewsPanel::onCaptureClicked()
{
    if (!m_bridge || !m_sceneEditable) return;
    const QString proposed = m_captureEdit ? m_captureEdit->text().trimmed() : QString();
    const QString name = proposed.isEmpty()
        ? tr("View %1").arg(m_names.size() + 1)
        : proposed;
    if (m_bridge->captureNamedView(name)) {
        if (m_captureEdit) m_captureEdit->clear();
        reload();
    }
}

void NamedViewsPanel::reload()
{
    if (!m_bridge) {
        setVisible(false);
        return;
    }
    setVisible(true);

    // namedViewNames() acquires the controller's commit mutex, which a
    // production or chat-owned render holds for its entire duration.
    // Mirror NamedViewsPanel.swift's isSceneEditableForAgents gate so
    // per-frame imageUpdated signals never block the GUI thread.
    if (!m_sceneEditable) return;

    const QStringList latest = m_bridge->namedViewNames();
    if (m_countLabel) {
        m_countLabel->setText(latest.isEmpty() ? QString() : QString::number(latest.size()));
    }
    if (!m_rowsBuilt || latest != m_names) {
        m_names = latest;
        m_rowsBuilt = true;
        rebuildRows();
    }
}

void NamedViewsPanel::rebuildRows()
{
    if (!m_rowsLayout || !m_rowsContainer || !m_emptyLabel) return;

    QLayoutItem* item;
    while ((item = m_rowsLayout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    const bool empty = m_names.isEmpty();
    m_emptyLabel->setVisible(empty);
    m_rowsContainer->setVisible(!empty);
    if (empty) return;

    auto makeActionBtn = [](QWidget* parent, const QString& iconName, const QString& helpText) {
        auto* btn = new QToolButton(parent);
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedSize(20, 18);
        btn->setIconSize(QSize(12, 12));
        btn->setIcon(Theme::icon(iconName, 12, Theme::textDim, Theme::textDisabled, Theme::textPrimary));
        btn->setToolTip(helpText);
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { border: none; background: transparent; padding: 0px; }"
            "QToolButton:hover:!disabled { background-color: %1; border-radius: %2px; }")
            .arg(Theme::rgba(Theme::fillHover))
            .arg(Theme::radiusSmall));
        return btn;
    };

    for (int idx = 0; idx < m_names.size(); ++idx) {
        const QString name = m_names.at(idx);

        auto* rowWidget = new QWidget(m_rowsContainer);
        rowWidget->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 1, 0, 1);
        rowLayout->setSpacing(6);

        // Primary non-destructive action: restore into transient free-fly pose.
        auto* restoreBtn = new QToolButton(rowWidget);
        restoreBtn->setAutoRaise(true);
        restoreBtn->setCursor(Qt::PointingHandCursor);
        restoreBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        restoreBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        restoreBtn->setIconSize(QSize(12, 12));
        restoreBtn->setIcon(Theme::icon(QStringLiteral("eye"), 12, Theme::textDim, Theme::textDisabled));
        restoreBtn->setFont(Theme::sans(11));
        QString elided = restoreBtn->fontMetrics().elidedText(name, Qt::ElideMiddle, 175);
        elided.replace(QLatin1Char('&'), QStringLiteral("&&"));
        restoreBtn->setText(elided);
        restoreBtn->setToolTip(tr("Restore \"%1\" (non-destructive)").arg(name));
        restoreBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; background: transparent; text-align: left; padding: 2px 4px; }"
            "QToolButton:disabled { color: %2; }"
            "QToolButton:hover:!disabled { background-color: %3; border-radius: %4px; }")
            .arg(Theme::hex(Theme::textPrimary),
                 Theme::hex(Theme::textDisabled),
                 Theme::rgba(Theme::fillHover))
            .arg(Theme::radiusSmall));
        connect(restoreBtn, &QToolButton::clicked, this, [this, idx]() {
            if (!m_bridge || !m_sceneEditable) return;
            m_bridge->restoreNamedView(idx);
        });
        rowLayout->addWidget(restoreBtn, 1);

        auto* updateBtn = makeActionBtn(rowWidget, QStringLiteral("refresh-cw"),
                                        tr("Update to current view"));
        connect(updateBtn, &QToolButton::clicked, this, [this, idx]() {
            if (!m_bridge || !m_sceneEditable) return;
            m_bridge->updateNamedView(idx);
            reload();
        });
        rowLayout->addWidget(updateBtn);

        auto* promoteBtn = makeActionBtn(rowWidget, QStringLiteral("camera"),
                                         tr("Promote to a scene camera"));
        connect(promoteBtn, &QToolButton::clicked, this, [this, idx, name]() {
            if (!m_bridge || !m_sceneEditable) return;
            const QString created = m_bridge->promoteNamedView(idx, name);
            if (!created.isEmpty()) {
                emit cameraPromoted();
            }
        });
        rowLayout->addWidget(promoteBtn);

        auto* deleteBtn = makeActionBtn(rowWidget, QStringLiteral("trash-2"),
                                        tr("Delete this view"));
        connect(deleteBtn, &QToolButton::clicked, this, [this, idx]() {
            if (!m_bridge || !m_sceneEditable) return;
            m_bridge->deleteNamedView(idx);
            reload();
        });
        rowLayout->addWidget(deleteBtn);

        m_rowsLayout->addWidget(rowWidget);
    }
}
