//////////////////////////////////////////////////////////////////////
//
//  EnvironmentPanel.cpp - Right-panel Environment / IBL section.
//    See header for the macOS EnvironmentPanel.swift cross-reference.
//
//////////////////////////////////////////////////////////////////////

#include "EnvironmentPanel.h"
#include "ViewportBridge.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QToolButton>
#include <QMouseEvent>
#include <QPalette>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMessageBox>
#include <QLayoutItem>
#include <QColor>

#include <cmath>
#include <utility>

namespace {

// A row that reports a plain left-click via a std::function callback.
// No Q_OBJECT / signals -- mirrors OutlinerWidget.cpp's ClickableRow and
// ViewportProperties.cpp's ScrubHandle, which document why a pure-input
// helper widget local to one .cpp doesn't need moc registration.
class ClickableRow : public QWidget
{
public:
    using ClickFn = std::function<void()>;

    explicit ClickableRow(ClickFn onClick, QWidget* parent = nullptr)
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

// Shared well chrome (Theme::bgWell fill + hairline border) -- mirrors
// ViewportProperties.cpp's wellStyleSheet() so numeric cells match the
// single-entity inspector below this panel exactly.
QString wellStyleSheet()
{
    return QStringLiteral("background-color: %1; border: 1px solid %2; border-radius: %3px;")
        .arg(Theme::hex(Theme::bgWell), Theme::hex(Theme::borderLight))
        .arg(Theme::radiusSmall);
}

QString lineEditStyleSheet(const QColor& textColor)
{
    return QStringLiteral("QLineEdit { background: transparent; border: none; color: %1; }")
        .arg(Theme::hex(textColor));
}

// Whole numbers show without a trailing ".0"; others keep %g precision.
// Mirrors the Mac panel's trimNumber(_:).
QString trimNumber(double v)
{
    // Non-finite -> "0" (parity with the Mac panel; a degenerate inf/nan value
    // in a field is meaningless).  Guarding it also fences the long long cast
    // below, which is UB on inf / |v| beyond int64 range.
    if (!std::isfinite(v)) {
        return QStringLiteral("0");
    }
    if (v == std::floor(v) && std::abs(v) < 1e15) {
        return QString::number(static_cast<long long>(v));
    }
    return QString::asprintf("%g", v);
}

}  // namespace

// ============================================================
// Construction
// ============================================================

EnvironmentPanel::EnvironmentPanel(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        setPalette(pal);
    }
    setStyleSheet(QStringLiteral("border-bottom: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));

    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    rebuild();
}

// ============================================================
// Bridge attach + gating
// ============================================================

void EnvironmentPanel::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    reload();
}

void EnvironmentPanel::setSceneEditable(bool editable)
{
    if (m_sceneEditable == editable) return;
    m_sceneEditable = editable;
    // Re-derive the controls' enable state immediately rather than
    // waiting for the next imageUpdated-driven reload() -- a render
    // finishing (or starting) should flip enablement on the SAME tick,
    // matching OutlinerWidget::setSceneEditable / the Mac panel's live
    // canEdit binding.
    reload();
}

bool EnvironmentPanel::canEdit() const
{
    return m_env.editable && m_sceneEditable;
}

// ============================================================
// Reload + rebuild
// ============================================================

bool EnvironmentPanel::anyFieldFocused() const
{
    return (m_scaleEdit   && m_scaleEdit->hasFocus())
        || (m_orientXEdit && m_orientXEdit->hasFocus())
        || (m_orientYEdit && m_orientYEdit->hasFocus())
        || (m_orientZEdit && m_orientZEdit->hasFocus());
}

void EnvironmentPanel::reload()
{
    // Don't fight an in-progress edit: while one of this panel's numeric
    // fields has focus, a live preview frame (which drives reload via
    // imageUpdated) must not rebuild the rows and delete the field the
    // user is typing in.  Values re-sync on the next frame once focus
    // leaves.  Windows equivalent of the Mac panel's focusedFieldIsActive
    // guard.
    if (anyFieldFocused()) return;

    m_hasInfo = m_bridge && m_bridge->environmentInfo(&m_env);
    if (!m_hasInfo) m_env = EnvironmentInfo{};
    rebuild();
}

void EnvironmentPanel::rebuild()
{
    // Full teardown + rebuild on every reload -- mirrors
    // OutlinerWidget::rebuild()'s declarative reload.  The section is
    // tiny (<= 6 rows), so this is cheap relative to a render frame and
    // avoids persistent-widget/bridge-state sync bugs.
    m_scaleEdit = nullptr;
    m_orientXEdit = nullptr;
    m_orientYEdit = nullptr;
    m_orientZEdit = nullptr;

    QLayoutItem* item;
    while ((item = m_rootLayout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    // The environment is meaningful only once a scene with an active
    // rasterizer is loaded; hide the whole section otherwise so an empty
    // app window isn't cluttered (mirrors the Mac panel's `if let e`).
    if (!m_hasInfo) {
        setVisible(false);
        return;
    }
    setVisible(true);

    // ---- Header: arrow + "Environment" + status chip ------------------
    // ClickableRow toggles expansion, matching OutlinerWidget's category
    // headers.
    auto* header = new ClickableRow([this]() {
        m_expanded = !m_expanded;
        rebuild();
    }, this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 12, 14, m_expanded ? 8 : 12);
    headerLayout->setSpacing(8);

    auto* arrow = new QLabel(m_expanded
        ? QString::fromUtf8("\xE2\x96\xBE")    // v (down triangle)
        : QString::fromUtf8("\xE2\x96\xB8"),   // > (right triangle)
        header);
    arrow->setFont(Theme::sans(8));
    arrow->setFixedWidth(10);
    arrow->setAlignment(Qt::AlignCenter);
    arrow->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    headerLayout->addWidget(arrow);

    auto* title = new QLabel(tr("Environment"), header);
    title->setFont(Theme::sans(12, QFont::DemiBold));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);

    // Status chip: filename / "procedural sky" / "none".
    QString chip;
    if (m_env.proceduralSky) {
        chip = tr("procedural sky");
    } else if (m_env.hasEnvironment) {
        const QString fileName = QFileInfo(m_env.file).fileName();
        chip = fileName.isEmpty() ? m_env.painterName : fileName;
    } else {
        chip = tr("none");
    }
    auto* chipLabel = new QLabel(chip, header);
    chipLabel->setFont(Theme::mono(10));
    chipLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    // Clip an over-long chip in the middle rather than forcing the panel
    // wider (mirrors the Mac chip's lineLimit(1)).
    chipLabel->setText(chipLabel->fontMetrics().elidedText(chip, Qt::ElideMiddle, 150));
    chipLabel->setToolTip(chip);
    headerLayout->addWidget(chipLabel);

    m_rootLayout->addWidget(header);

    if (!m_expanded) return;

    // ---- Content ------------------------------------------------------
    auto* content = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(14, 0, 14, 12);
    contentLayout->setSpacing(10);

    // Small helpers reused across the states.  Each is used by only a
    // subset of the branches below, so [[maybe_unused]] keeps every
    // compiler quiet about the branch that doesn't reach one of them.
    [[maybe_unused]] auto makeFieldLabel = [content](const QString& s) {
        auto* lbl = new QLabel(s, content);
        lbl->setFont(Theme::mono(9));
        lbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        return lbl;
    };
    [[maybe_unused]] auto makeInfoNote = [content](const QString& s) {
        auto* lbl = new QLabel(s, content);
        lbl->setFont(Theme::sans(10));
        lbl->setWordWrap(true);
        lbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        return lbl;
    };
    // "Editing paused..." vs "needs a PT/BDPT/VCM rasterizer", chosen from
    // env.editable exactly like the Mac panel's disabledReasonNote.
    [[maybe_unused]] auto disabledReasonText = [this]() {
        return m_env.editable
            ? tr("Editing is paused while a render is in flight.")
            : tr("Environment editing needs a Path Tracing / BDPT / VCM rasterizer.");
    };

    if (m_env.proceduralSky) {
        // ---- Procedural sky (read-only) -------------------------------
        contentLayout->addWidget(makeInfoNote(
            tr("Procedural sky (Hosek\xE2\x80\x93Wilkie) is providing the environment. "
               "Edit it in the scene file.")));
    } else if (!m_env.hasEnvironment) {
        // ---- Empty state ----------------------------------------------
        contentLayout->addWidget(makeInfoNote(tr("No environment")));
        if (canEdit()) {
            auto* addBtn = new QToolButton(content);
            addBtn->setText(tr("Add HDRI\xE2\x80\xA6"));
            addBtn->setFont(Theme::sans(11, QFont::Medium));
            addBtn->setCursor(Qt::PointingHandCursor);
            addBtn->setStyleSheet(QStringLiteral(
                "QToolButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 6px 12px; }")
                .arg(Theme::hex(Theme::accentLight),
                     Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(),
                                         Theme::accent.blue(), static_cast<int>(0.3 * 255))))
                .arg(Theme::radiusMedium));
            connect(addBtn, &QToolButton::clicked, this, [this]() {
                if (!m_bridge || !canEdit()) return;
                pickFile([this](const QString& path) {
                    QString outName;
                    QString outMessage;
                    const bool ok = m_bridge->addEnvironment(path, &outName, &outMessage);
                    finishEdit(ok, outMessage);
                });
            });
            auto* btnRow = new QHBoxLayout;
            btnRow->setContentsMargins(0, 0, 0, 0);
            btnRow->addWidget(addBtn);
            btnRow->addStretch(1);
            contentLayout->addLayout(btnRow);
        } else {
            contentLayout->addWidget(makeInfoNote(disabledReasonText()));
        }
    } else {
        // ---- Bound state ----------------------------------------------
        const bool editable = canEdit();

        // File row.
        contentLayout->addWidget(makeFieldLabel(tr("Image")));
        {
            auto* fileRow = new QHBoxLayout;
            fileRow->setContentsMargins(0, 0, 0, 0);
            fileRow->setSpacing(6);

            const QString fileName = QFileInfo(m_env.file).fileName();
            const QString displayName = fileName.isEmpty() ? m_env.painterName : fileName;
            auto* fileLabel = new QLabel(content);
            fileLabel->setFont(Theme::mono(11));
            fileLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textTertiary)));
            fileLabel->setText(fileLabel->fontMetrics().elidedText(displayName, Qt::ElideMiddle, 180));
            fileLabel->setToolTip(m_env.file.isEmpty() ? m_env.painterName : m_env.file);
            fileRow->addWidget(fileLabel);
            fileRow->addStretch(1);

            if (editable) {
                auto* swapBtn = new QToolButton(content);
                swapBtn->setText(tr("Swap\xE2\x80\xA6"));
                swapBtn->setFont(Theme::sans(10, QFont::Medium));
                swapBtn->setCursor(Qt::PointingHandCursor);
                swapBtn->setStyleSheet(QStringLiteral(
                    "QToolButton { color: %1; border: none; padding: 2px 6px; }")
                    .arg(Theme::hex(Theme::accentLight)));
                connect(swapBtn, &QToolButton::clicked, this, [this]() {
                    if (!m_bridge || !canEdit()) return;
                    pickFile([this](const QString& path) {
                        finishEdit(m_bridge->setEnvironmentFile(path), QString());
                    });
                });
                fileRow->addWidget(swapBtn);
            }
            contentLayout->addLayout(fileRow);
        }

        // Intensity.
        contentLayout->addWidget(makeFieldLabel(tr("Intensity")));
        {
            auto* well = new QWidget(content);
            well->setStyleSheet(wellStyleSheet());
            auto* wellLayout = new QHBoxLayout(well);
            wellLayout->setContentsMargins(8, 5, 8, 5);
            auto* edit = new QLineEdit(trimNumber(m_env.scale), well);
            edit->setFont(Theme::mono(11));
            edit->setFrame(false);
            edit->setEnabled(editable);
            edit->setStyleSheet(lineEditStyleSheet(editable ? Theme::textPrimary : Theme::textDisabled));
            wellLayout->addWidget(edit);
            m_scaleEdit = edit;
            connect(edit, &QLineEdit::editingFinished, this, [this]() {
                if (!m_scaleEdit || !m_bridge) return;
                bool ok = false;
                const double v = m_scaleEdit->text().toDouble(&ok);
                // Re-check canEdit at commit time (a render may have started
                // while the field held focus, past the .disabled gate) and
                // REVERT non-numeric text so the buffer never diverges from
                // (or lands on) the scene -- mirrors the Mac commitOrRevert.
                if (!canEdit() || !ok) { m_scaleEdit->setText(trimNumber(m_env.scale)); return; }
                finishEdit(m_bridge->setEnvironmentScale(v), QString());
            });
            contentLayout->addWidget(well);
        }

        // Rotation (degrees) -- three fields, committed together.
        contentLayout->addWidget(makeFieldLabel(tr("Rotation (\xC2\xB0)")));
        {
            auto* rotRow = new QHBoxLayout;
            rotRow->setContentsMargins(0, 0, 0, 0);
            rotRow->setSpacing(6);

            struct AxisSpec { const char* label; double value; QPointer<QLineEdit>* slot; };
            AxisSpec axes[3] = {
                { "X", m_env.orientX, &m_orientXEdit },
                { "Y", m_env.orientY, &m_orientYEdit },
                { "Z", m_env.orientZ, &m_orientZEdit },
            };
            for (const AxisSpec& axis : axes) {
                auto* axisWrap = new QWidget(content);
                auto* axisLayout = new QHBoxLayout(axisWrap);
                axisLayout->setContentsMargins(0, 0, 0, 0);
                axisLayout->setSpacing(3);

                auto* axisLabel = new QLabel(QString::fromUtf8(axis.label), axisWrap);
                axisLabel->setFont(Theme::mono(9));
                axisLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDisabled)));
                axisLayout->addWidget(axisLabel);

                auto* well = new QWidget(axisWrap);
                well->setStyleSheet(wellStyleSheet());
                auto* wellLayout = new QHBoxLayout(well);
                wellLayout->setContentsMargins(8, 5, 8, 5);
                auto* edit = new QLineEdit(trimNumber(axis.value), well);
                edit->setFont(Theme::mono(11));
                edit->setFrame(false);
                edit->setEnabled(editable);
                edit->setStyleSheet(lineEditStyleSheet(editable ? Theme::textPrimary : Theme::textDisabled));
                wellLayout->addWidget(edit);
                axisLayout->addWidget(well, 1);

                *axis.slot = edit;
                connect(edit, &QLineEdit::editingFinished, this, [this]() { commitRotation(); });

                rotRow->addWidget(axisWrap, 1);
            }
            contentLayout->addLayout(rotRow);
        }

        // Background toggle.
        {
            auto* check = new QCheckBox(tr("Show in background"), content);
            check->setFont(Theme::sans(11));
            check->setChecked(m_env.background);
            check->setEnabled(editable);
            check->setStyleSheet(QStringLiteral("QCheckBox { color: %1; }")
                .arg(Theme::hex(editable ? Theme::textTertiary : Theme::textDisabled)));
            // Connect AFTER setChecked so the initial state doesn't fire a
            // spurious commit.
            connect(check, &QCheckBox::toggled, this, [this](bool on) {
                if (!m_bridge || !canEdit()) return;
                finishEdit(m_bridge->setEnvironmentBackground(on), QString());
            });
            contentLayout->addWidget(check);
        }

        // Remove.
        if (editable) {
            auto* removeBtn = new QToolButton(content);
            removeBtn->setText(tr("Remove"));
            removeBtn->setFont(Theme::sans(10, QFont::Medium));
            removeBtn->setCursor(Qt::PointingHandCursor);
            removeBtn->setStyleSheet(QStringLiteral(
                "QToolButton { color: %1; border: none; padding: 2px 6px; }")
                .arg(Theme::hex(Theme::error)));
            connect(removeBtn, &QToolButton::clicked, this, [this]() {
                if (!m_bridge || !canEdit()) return;
                finishEdit(m_bridge->removeEnvironment(), QString());
            });
            auto* removeRow = new QHBoxLayout;
            removeRow->setContentsMargins(0, 0, 0, 0);
            removeRow->addStretch(1);
            removeRow->addWidget(removeBtn);
            contentLayout->addLayout(removeRow);
        } else {
            contentLayout->addWidget(makeInfoNote(disabledReasonText()));
        }
    }

    m_rootLayout->addWidget(content);
}

// ============================================================
// Edits
// ============================================================

void EnvironmentPanel::finishEdit(bool ok, const QString& failureMessage)
{
    // Present a message whenever one is provided: an AddEnvironment can land the
    // live bind (ok == true) yet return a "partial" message when it couldn't be
    // recorded in the scene file -- swallowing that on the ok path would leave
    // the user unaware a save will drop the binding (mirrors the Mac fix).
    if (!failureMessage.isEmpty()) {
        QMessageBox::warning(this, tr("Environment"), failureMessage);
    }
    reload();
    emit environmentEdited();
}

void EnvironmentPanel::commitRotation()
{
    if (!m_bridge) return;
    // Re-check canEdit at commit time: a render may have started while an axis
    // field held focus (past the .setEnabled gate).  Revert any in-progress
    // text and don't commit -- the UI must not depend on the core serializing
    // on mRendering.  Mirrors the Mac commitOrRevert guard.
    if (!canEdit()) {
        if (m_orientXEdit) m_orientXEdit->setText(trimNumber(m_env.orientX));
        if (m_orientYEdit) m_orientYEdit->setText(trimNumber(m_env.orientY));
        if (m_orientZEdit) m_orientZEdit->setText(trimNumber(m_env.orientZ));
        return;
    }
    // radiance_orient is one vec3: committing any axis re-sends all three.  A
    // field whose text doesn't parse is REVERTED to the last snapshot value and
    // that value is used (mirrors the Mac panel's commitRotation fallback).
    auto axisValue = [](const QPointer<QLineEdit>& edit, double fallback) {
        if (!edit) return fallback;
        bool ok = false;
        const double v = edit->text().toDouble(&ok);
        if (!ok) { edit->setText(trimNumber(fallback)); return fallback; }
        return v;
    };
    const double x = axisValue(m_orientXEdit, m_env.orientX);
    const double y = axisValue(m_orientYEdit, m_env.orientY);
    const double z = axisValue(m_orientZEdit, m_env.orientZ);
    finishEdit(m_bridge->setEnvironmentOrient(x, y, z), QString());
}

void EnvironmentPanel::pickFile(const std::function<void(const QString&)>& chosen)
{
    const QString picked = QFileDialog::getOpenFileName(
        this,
        tr("Choose an HDRI environment image"),
        QString(),
        tr("HDRI Images (*.hdr *.exr)"));
    if (picked.isEmpty()) return;   // user cancelled
    chosen(picked);
}
