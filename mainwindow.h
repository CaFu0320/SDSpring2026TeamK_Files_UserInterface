//tells Qt what the window contains
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>

class QAction;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void startBackend();
    void stopBackend();
    void onStdout();
    void onStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError err);
    void showContextMenu(const QPoint &pos);

private:
    void applyState(const QJsonObject &o);
    static QString colorStyle(const QString &name, bool largeDot);
    static QString laneArrowGlyph(const QString &arrowKey);
    void openExamplesFolderSettings();
    void openDeviceAddressSettings();
    void tryAutoStart();
    QString examplesWslPath() const;
    QString deviceAddress() const;
    void setStatus(const QString &text);

    QAction *m_autoStartAct;
    QLabel *m_intersectionTitle;
    QLabel *m_curDot;
    QLabel *m_laneArrow;
    QLabel *m_nextDot;
    QLabel *m_secValueLabel;
    QLabel *m_statusLine;

    QProcess *m_proc;
    QByteArray m_buf;
    QByteArray m_errBuf;
};
