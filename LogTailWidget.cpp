// Copyright (C) 2026 Sean Moon
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "LogTailWidget.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

#include <QDialog>
#include <QFile>
#include <QFileInfo>

// ── Config struct ─────────────────────────────────────────────────────────────

struct LogTailConfig {
    enum class Source { None, File, Journalctl };
    Source  source      = Source::None;
    QString filePath;
    QString journalUnit;   // empty = no -u filter
    int     maxLines    = 500;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace {

QColor colorForLine(const QString& line) {
    // Check the first ~40 chars; avoids scanning megabyte-long lines
    const QString head = line.left(40).toUpper();
    if (head.contains("ERROR")  || head.contains("FATAL") ||
        head.contains("CRIT")   || head.contains("EMERG") ||
        head.contains("ALERT"))
        return QColor("#ff5555");
    if (head.contains("WARN"))
        return QColor("#ffb86c");
    if (head.contains("DEBUG") || head.contains("TRACE") || head.contains("VERBOSE"))
        return QColor("#6272a4");
    if (head.contains("INFO")  || head.contains("NOTICE"))
        return QColor("#8be9fd");
    return QColor("#c8cee8");
}

}  // namespace

// ── LogTailDisplay ────────────────────────────────────────────────────────────

class LogTailDisplay : public QWidget {
    Q_OBJECT

public:
    explicit LogTailDisplay(QWidget* parent = nullptr) : QWidget(parent) {
        setupUi();
    }

    ~LogTailDisplay() override { stopSource(); }

    QJsonObject saveConfig() const {
        QJsonObject obj;
        switch (config_.source) {
            case LogTailConfig::Source::File:        obj["sourceType"] = "file";        break;
            case LogTailConfig::Source::Journalctl:  obj["sourceType"] = "journalctl"; break;
            default:                                 obj["sourceType"] = "";            break;
        }
        obj["filePath"]    = config_.filePath;
        obj["journalUnit"] = config_.journalUnit;
        obj["maxLines"]    = config_.maxLines;
        return obj;
    }

    void loadConfig(const QJsonObject& obj) {
        const QString type = obj["sourceType"].toString();
        if      (type == "file")        config_.source = LogTailConfig::Source::File;
        else if (type == "journalctl")  config_.source = LogTailConfig::Source::Journalctl;
        else                            config_.source = LogTailConfig::Source::None;

        config_.filePath    = obj["filePath"].toString();
        config_.journalUnit = obj["journalUnit"].toString();
        config_.maxLines    = obj.value("maxLines").toInt(500);

        applySource();
    }

private:
    // ── UI setup ──────────────────────────────────────────────────────────────
    void setupUi() {
        setStyleSheet(
            "QWidget { background: transparent; }"
            "QPlainTextEdit {"
            "  background: #0d1117; color: #c8cee8;"
            "  border: none; font-family: monospace; font-size: 11px; }"
            "QScrollBar:vertical { background: #0d1117; width: 6px; border: none; }"
            "QScrollBar::handle:vertical { background: #2d3748; border-radius: 3px; min-height: 20px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

        auto* vbox = new QVBoxLayout(this);
        vbox->setContentsMargins(0, 0, 0, 0);
        vbox->setSpacing(0);

        // ── Header bar ────────────────────────────────────────────────────────
        auto* header = new QWidget(this);
        header->setStyleSheet("background: #161b22; border-bottom: 1px solid #2d3748;");
        header->setFixedHeight(28);
        auto* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(8, 0, 4, 0);
        headerLayout->setSpacing(4);

        sourceLabel_ = new QLabel("not configured", header);
        sourceLabel_->setStyleSheet(
            "color: #5588cc; font-size: 10px; font-weight: bold; font-family: monospace;"
            "background: transparent; border: none;");

        configBtn_ = new QPushButton("⚙", header);
        configBtn_->setFixedSize(22, 22);
        configBtn_->setToolTip("Configure source");
        configBtn_->setStyleSheet(
            "QPushButton { background: transparent; border: none;"
            "  color: #5588cc; font-size: 14px; padding: 0; }"
            "QPushButton:hover { color: #88bbff; }");

        headerLayout->addWidget(sourceLabel_, 1);
        headerLayout->addWidget(configBtn_);
        vbox->addWidget(header);

        // ── Stacked body ──────────────────────────────────────────────────────
        stack_ = new QStackedWidget(this);
        vbox->addWidget(stack_, 1);

        // Page 0: placeholder shown when unconfigured
        auto* placeholder = new QWidget(stack_);
        auto* phLayout = new QVBoxLayout(placeholder);
        auto* phLabel = new QLabel(
            "No log source configured.\nClick \u2699 to set up.", placeholder);
        phLabel->setAlignment(Qt::AlignCenter);
        phLabel->setStyleSheet(
            "color: #404060; font-size: 12px; background: transparent;");
        phLayout->addWidget(phLabel);
        stack_->addWidget(placeholder);   // index 0

        // Page 1: the log view
        logView_ = new QPlainTextEdit(stack_);
        logView_->setReadOnly(true);
        logView_->setLineWrapMode(QPlainTextEdit::NoWrap);
        logView_->setMaximumBlockCount(500);  // updated in applySource()
        stack_->addWidget(logView_);          // index 1

        connect(configBtn_, &QPushButton::clicked, this, &LogTailDisplay::openConfig);
    }

    // ── Source management ─────────────────────────────────────────────────────
    void stopSource() {
        if (watcher_) {
            watcher_->removePaths(watcher_->files());
            delete watcher_;
            watcher_ = nullptr;
        }
        if (process_) {
            process_->kill();
            process_->waitForFinished(500);
            delete process_;
            process_ = nullptr;
        }
    }

    void applySource() {
        stopSource();
        updateSourceLabel();
        logView_->setMaximumBlockCount(config_.maxLines);
        logView_->clear();

        if (config_.source == LogTailConfig::Source::None) {
            stack_->setCurrentIndex(0);
            return;
        }

        stack_->setCurrentIndex(1);

        if (config_.source == LogTailConfig::Source::File) {
            startFileTail();
        } else {
            startJournalctl();
        }
    }

    void updateSourceLabel() {
        switch (config_.source) {
            case LogTailConfig::Source::File:
                sourceLabel_->setText(QFileInfo(config_.filePath).fileName());
                break;
            case LogTailConfig::Source::Journalctl:
                sourceLabel_->setText(
                    config_.journalUnit.isEmpty()
                        ? "journalctl"
                        : QString("journalctl -u %1").arg(config_.journalUnit));
                break;
            default:
                sourceLabel_->setText("not configured");
                break;
        }
    }

    // ── File tail ─────────────────────────────────────────────────────────────
    void startFileTail() {
        QFile f(config_.filePath);
        if (!f.open(QFile::ReadOnly)) {
            appendLine(QString("Cannot open: %1").arg(config_.filePath), QColor("#ff5555"));
            filePos_ = 0;
            return;
        }

        // Seed with up to the last ~100 KB so we don't read gigabyte-sized files
        const qint64 fileSize = f.size();
        const qint64 seekTo   = qMax(qint64(0), fileSize - qint64(100 * 1024));
        f.seek(seekTo);

        QStringList lines;
        for (const QByteArray& raw : f.readAll().split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (!line.isEmpty()) lines.append(line);
        }

        // Keep only the last maxLines entries
        if (lines.size() > config_.maxLines)
            lines = lines.mid(lines.size() - config_.maxLines);

        appendLines(lines);
        filePos_ = fileSize;

        watcher_ = new QFileSystemWatcher(this);
        watcher_->addPath(config_.filePath);
        connect(watcher_, &QFileSystemWatcher::fileChanged,
                this, &LogTailDisplay::onFileChanged);
    }

    void onFileChanged(const QString& path) {
        QFile f(path);
        if (!f.open(QFile::ReadOnly)) return;

        const qint64 currentSize = f.size();
        if (currentSize < filePos_) {
            // Truncation / log rotation
            filePos_ = 0;
            logView_->clear();
            appendLine("─── log rotated ───", QColor("#6272a4"));
        }

        if (filePos_ == currentSize) return;

        f.seek(filePos_);
        const QByteArray newData = f.readAll();
        filePos_ = f.pos();

        QStringList lines;
        for (const QByteArray& raw : newData.split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (!line.isEmpty()) lines.append(line);
        }
        appendLines(lines);

        // QFileSystemWatcher may stop tracking after some editors replace files
        if (!watcher_->files().contains(path))
            watcher_->addPath(path);
    }

    // ── journalctl ────────────────────────────────────────────────────────────
    void startJournalctl() {
        QStringList args = {"-f", "-n", "50", "--no-pager", "--output=short-iso"};
        if (!config_.journalUnit.isEmpty())
            args << "-u" << config_.journalUnit;

        process_ = new QProcess(this);
        connect(process_, &QProcess::readyReadStandardOutput,
                this, &LogTailDisplay::onJournalOutput);
        connect(process_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError) {
                    appendLine("journalctl: failed to start — is systemd available?",
                               QColor("#ff5555"));
                });
        process_->start("journalctl", args);
    }

    void onJournalOutput() {
        QStringList lines;
        while (process_->canReadLine()) {
            const QString line = QString::fromUtf8(process_->readLine()).trimmed();
            if (!line.isEmpty()) lines.append(line);
        }
        appendLines(lines);
    }

    // ── Text insertion helpers ────────────────────────────────────────────────
    void appendLines(const QStringList& lines) {
        if (lines.isEmpty()) return;

        auto* sb = logView_->verticalScrollBar();
        const bool atBottom = sb->value() >= sb->maximum() - 4;

        logView_->setUpdatesEnabled(false);
        QTextCursor cursor(logView_->document());
        cursor.movePosition(QTextCursor::End);
        cursor.beginEditBlock();

        for (const QString& line : lines) {
            if (cursor.position() > 0) cursor.insertBlock();
            QTextCharFormat fmt;
            fmt.setForeground(colorForLine(line));
            cursor.setCharFormat(fmt);
            cursor.insertText(line);
        }

        cursor.endEditBlock();
        logView_->setUpdatesEnabled(true);

        if (atBottom) {
            QMetaObject::invokeMethod(this, [this, sb]() {
                sb->setValue(sb->maximum());
            }, Qt::QueuedConnection);
        }
    }

    void appendLine(const QString& line, QColor color) {
        appendLines({line});
        // Override color on the last block
        QTextCursor cursor(logView_->document());
        cursor.movePosition(QTextCursor::End);
        cursor.select(QTextCursor::BlockUnderCursor);
        QTextCharFormat fmt;
        fmt.setForeground(color);
        cursor.mergeCharFormat(fmt);
    }

    // ── Config dialog ─────────────────────────────────────────────────────────
    void openConfig() {
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle("Configure Log Source");
        dlg->setMinimumWidth(400);

        auto* vbox = new QVBoxLayout(dlg);
        vbox->setSpacing(8);

        // Source type radios
        auto* fileRadio    = new QRadioButton("File", dlg);
        auto* journalRadio = new QRadioButton("journalctl (systemd)", dlg);

        if (config_.source == LogTailConfig::Source::Journalctl)
            journalRadio->setChecked(true);
        else
            fileRadio->setChecked(true);

        // File path row
        auto* fileRow    = new QWidget(dlg);
        auto* fileLayout = new QHBoxLayout(fileRow);
        fileLayout->setContentsMargins(16, 0, 0, 0);
        auto* fileEdit  = new QLineEdit(config_.filePath, fileRow);
        auto* browseBtn = new QPushButton("Browse…", fileRow);
        fileLayout->addWidget(new QLabel("Path:", fileRow));
        fileLayout->addWidget(fileEdit, 1);
        fileLayout->addWidget(browseBtn);

        // journalctl unit row
        auto* journalRow    = new QWidget(dlg);
        auto* journalLayout = new QHBoxLayout(journalRow);
        journalLayout->setContentsMargins(16, 0, 0, 0);
        auto* unitEdit = new QLineEdit(config_.journalUnit, journalRow);
        unitEdit->setPlaceholderText("leave empty for all units");
        journalLayout->addWidget(new QLabel("Unit:", journalRow));
        journalLayout->addWidget(unitEdit, 1);

        // Buffer size
        auto* bufRow    = new QHBoxLayout();
        auto* spinBox   = new QSpinBox(dlg);
        spinBox->setRange(50, 5000);
        spinBox->setValue(config_.maxLines);
        spinBox->setSuffix(" lines");
        bufRow->addWidget(new QLabel("Buffer size:", dlg));
        bufRow->addWidget(spinBox);
        bufRow->addStretch();

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);

        vbox->addWidget(fileRadio);
        vbox->addWidget(fileRow);
        vbox->addWidget(journalRadio);
        vbox->addWidget(journalRow);
        vbox->addLayout(bufRow);
        vbox->addWidget(buttons);

        auto syncVisibility = [&]() {
            fileRow->setEnabled(fileRadio->isChecked());
            journalRow->setEnabled(journalRadio->isChecked());
        };
        syncVisibility();

        connect(fileRadio,    &QRadioButton::toggled, dlg, [syncVisibility](bool) { syncVisibility(); });
        connect(journalRadio, &QRadioButton::toggled, dlg, [syncVisibility](bool) { syncVisibility(); });
        connect(browseBtn, &QPushButton::clicked, dlg, [&]() {
            const QString p = QFileDialog::getOpenFileName(dlg, "Choose Log File");
            if (!p.isEmpty()) fileEdit->setText(p);
        });
        connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);

        if (dlg->exec() == QDialog::Accepted) {
            config_.source = fileRadio->isChecked()
                ? LogTailConfig::Source::File
                : LogTailConfig::Source::Journalctl;
            config_.filePath    = fileEdit->text().trimmed();
            config_.journalUnit = unitEdit->text().trimmed();
            config_.maxLines    = spinBox->value();
            applySource();
        }
        dlg->deleteLater();
    }

    // ── Members ───────────────────────────────────────────────────────────────
    LogTailConfig        config_;
    QLabel*              sourceLabel_ = nullptr;
    QPushButton*         configBtn_   = nullptr;
    QStackedWidget*      stack_       = nullptr;
    QPlainTextEdit*      logView_     = nullptr;
    QFileSystemWatcher*  watcher_     = nullptr;
    QProcess*            process_     = nullptr;
    qint64               filePos_     = 0;
};

#include "LogTailWidget.moc"

// ── LogTailWidget (IWidget plugin) ────────────────────────────────────────────

LogTailWidget::LogTailWidget(QObject* parent) : QObject(parent) {}

void LogTailWidget::initialize(dashboard::WidgetContext* /*context*/) {}

QWidget* LogTailWidget::createWidget(QWidget* parent) {
    display_ = new LogTailDisplay(parent);
    if (!pending_.isEmpty())
        display_->loadConfig(pending_);
    return display_;
}

QJsonObject LogTailWidget::serialize() const {
    return display_ ? display_->saveConfig() : pending_;
}

void LogTailWidget::deserialize(const QJsonObject& data) {
    pending_ = data;
    if (display_) display_->loadConfig(data);
}

dashboard::WidgetMetadata LogTailWidget::metadata() const {
    return {
        .name        = "logtail",
        .version     = "1.0.0",
        .author      = "Dashboard",
        .description = "Tail a log file or journalctl stream",
        .minSize     = QSize(300, 150),
        .maxSize     = QSize(1200, 900),
        .defaultSize = QSize(520, 300),
    };
}
