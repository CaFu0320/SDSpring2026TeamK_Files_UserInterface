/*
 * Simple explanation of this file:
 * This Qt file creates the screen shown in the demo.
 *
 * The older C program, fac_subs_decode_with_logging, talks to the Commsignia
 * unit and prints simple JSON lines such as:
 *
 *   current light color
 *   next light color
 *   seconds remaining
 *   intersection name
 *   lane direction arrow
 *
 * This Qt program starts that C program in the background, reads those JSON
 * lines, and updates the screen.
 *
 * In short:
 *
 *   C decoder program  --->  JSON text  --->  Qt user interface
 *
 * That is how the traffic-light circle, countdown, intersection name,
 * and arrow appear in the window.
 */

#include "MainWindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// These names are used by Qt to remember saved settings
// For example, the app remembers the SDK folder and the OBU address
constexpr auto kSettingsOrg = "spat_viewer";
constexpr auto kSettingsApp = "SPATViewer";
constexpr auto kKeyExamplesPath = "backend/examplesWslPath";
constexpr auto kKeyLastAddress = "device/lastAddress";
constexpr auto kKeyAutoStart = "ui/autoStartOnLaunch";

/*
 * Make a small text label used for words like:
 * "Now approaching:", "Current:", "Change to", and "seconds"
 */
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color: #f0f0f0; font-size: 18px;"));
    return l;
}

/*
 * Clean up the intersection name before showing it
 * This makes names with "&" display normally on the screen
 */
QString normalizedIntersectionTitle(QString s)
{
    s.replace(QStringLiteral("&#38;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return s;
}

#ifdef Q_OS_WIN
/*
 * On Windows, the backend runs inside WSL
 * This helper turns a Windows path like C:/Users/... into a WSL path like /mnt/c/Users/...
 */
QString toWslPath(QString in)
{
    QString path = in.trimmed();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if(path.size() >= 2 && path.at(1) == QLatin1Char(':')) {
        const QChar drive = path.at(0).toLower();
        QString tail = path.mid(2);
        while(tail.startsWith(QLatin1Char('/'))) {
            tail.remove(0, 1);
        }
        path = QStringLiteral("/mnt/%1/%2").arg(drive, tail);
    }
    return QDir::cleanPath(path);
}
#endif
} 

/*
 * This is the main window constructor
 * It builds the user interface seen in the screenshot
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_proc(nullptr)
    , m_autoStartAct(nullptr)
    , m_statusLine(nullptr)
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QCoreApplication::setOrganizationName(QString::fromLatin1(kSettingsOrg));
    QCoreApplication::setApplicationName(QString::fromLatin1(kSettingsApp));

    // Set the window name and starting size
    setWindowTitle(QStringLiteral("SPAT Viewer"));
    resize(680, 520);

    // Hide the normal menu/status bars so the UI looks clean and simple
    menuBar()->hide();
    statusBar()->hide();

    // This option lets the program connect automatically when it opens
    m_autoStartAct = new QAction(QStringLiteral("Connect automatically on startup"), this);
    m_autoStartAct->setCheckable(true);
    {
        QSettings s;
        m_autoStartAct->setChecked(s.value(QString::fromLatin1(kKeyAutoStart), true).toBool());
    }
    connect(m_autoStartAct, &QAction::triggered, this, [this](bool checked) {
        QSettings s;
        s.setValue(QString::fromLatin1(kKeyAutoStart), checked);
    });

    // Create the main background area of the window
    auto *central = new QWidget(this);
    central->setStyleSheet(QStringLiteral("background-color: #1a1a1e;"));
    setCentralWidget(central);
    // Right-clicking the window opens settings such as Start, Stop, SDK folder, and device IP
    central->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(central, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    auto *root = new QVBoxLayout(central);
    root->setSpacing(12);
    root->setContentsMargins(24, 20, 24, 20);

    /*
     * Top row of the UI:
     * Shows "Now approaching:" followed by the intersection name
     * In the screenshot, this is where "Tamarus St & Flamingo Rd" appears
     */
    auto *approachRow = new QHBoxLayout();
    approachRow->addWidget(makeCaption(QStringLiteral("Now approaching:"), this));
    m_intersectionTitle = new QLabel(QStringLiteral("—"), this);
    m_intersectionTitle->setTextFormat(Qt::PlainText);
    m_intersectionTitle->setStyleSheet(
        QStringLiteral("color: #5dade2; font-size: 20px; font-weight: 600;"));
    m_intersectionTitle->setWordWrap(true);
    approachRow->addWidget(m_intersectionTitle, 1);
    root->addLayout(approachRow);

    // Add the "Current:" text above the big traffic-light circle
    root->addSpacing(8);
    root->addWidget(makeCaption(QStringLiteral("Current:"), this));

    /*
     * Big traffic-light circle
     * Its color changes to green, yellow, red, or gray based on the live SPaT data
     */
    m_curDot = new QLabel(QStringLiteral("●"), this);
    m_curDot->setAlignment(Qt::AlignCenter);
    m_curDot->setMinimumHeight(220);
    QFont dotFont = m_curDot->font();
    dotFont.setPointSize(140);
    m_curDot->setFont(dotFont);
    m_curDot->setStyleSheet(colorStyle(QStringLiteral("UNKNOWN"), true));

    /*
     * Lane direction arrow
     * This shows whether the lane movement is straight, left, right, or U-turn
     */
    m_laneArrow = new QLabel(laneArrowGlyph(QString()), this);
    m_laneArrow->setAlignment(Qt::AlignCenter);
    m_laneArrow->setMinimumWidth(120);
    QFont arrowFont = m_laneArrow->font();
    arrowFont.setPointSize(96);
    m_laneArrow->setFont(arrowFont);
    m_laneArrow->setStyleSheet(QStringLiteral("color: #ecf0f1;"));

    /*
     * Put the big light and arrow next to each other
     * This is the middle part of the screenshot
     */
    auto *signalRow = new QHBoxLayout();
    signalRow->setSpacing(0);
    signalRow->addStretch(1);
    signalRow->addWidget(m_curDot, 0, Qt::AlignVCenter);
    signalRow->addStretch(1);
    signalRow->addWidget(m_laneArrow, 0, Qt::AlignVCenter);
    signalRow->addStretch(1);
    root->addLayout(signalRow);

    root->addStretch(1);

    /*
     * Bottom row of the UI:
     * Shows the next light color and how many seconds are left
     * In the screenshot, this is the "Change to yellow in 5 seconds" section
     */
    auto *changeRow = new QHBoxLayout();
    changeRow->setSpacing(10);
    changeRow->addWidget(makeCaption(QStringLiteral("Change to"), this));
    m_nextDot = new QLabel(QStringLiteral("●"), this);
    m_nextDot->setAlignment(Qt::AlignCenter);
    QFont nextFont = m_nextDot->font();
    nextFont.setPointSize(32);
    m_nextDot->setFont(nextFont);
    m_nextDot->setStyleSheet(colorStyle(QStringLiteral("UNKNOWN"), false));
    changeRow->addWidget(m_nextDot, 0, Qt::AlignVCenter);
    changeRow->addWidget(makeCaption(QStringLiteral("in"), this));
    m_secValueLabel = new QLabel(QStringLiteral("—"), this);
    m_secValueLabel->setStyleSheet(
        QStringLiteral("color: #5dade2; font-size: 22px; font-weight: 700;"));
    changeRow->addWidget(m_secValueLabel, 0, Qt::AlignVCenter);
    changeRow->addWidget(makeCaption(QStringLiteral("seconds"), this));
    changeRow->addStretch(1);
    root->addLayout(changeRow);

    /*
     * Small status message at the bottom
     * This tells the user if the backend is connected, waiting, stopped, or has an error
     */
    m_statusLine = new QLabel(QStringLiteral(
        "Status: Right-click → SDK examples folder & device address, then Start connection "
        "(or save both to auto-connect on launch)."), this);
    m_statusLine->setWordWrap(true);
    m_statusLine->setStyleSheet(QStringLiteral("color: #8a8a95; font-size: 13px;"));
    root->addWidget(m_statusLine);

    // After the window opens, try to connect automatically if the user saved that option
    QTimer::singleShot(0, this, &MainWindow::tryAutoStart);
}

/*
 * This function creates the right-click menu
 * The user can start/stop the connection and set the SDK folder or device address
 */
void MainWindow::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    QAction *startAct = menu.addAction(QStringLiteral("Start connection"));
    QAction *stopAct = menu.addAction(QStringLiteral("Stop connection"));
    startAct->setEnabled(m_proc == nullptr);
    stopAct->setEnabled(m_proc != nullptr);
    menu.addSeparator();
    menu.addAction(QStringLiteral("SDK examples folder…"), this, &MainWindow::openExamplesFolderSettings);
    menu.addAction(QStringLiteral("Device address…"), this, &MainWindow::openDeviceAddressSettings);
    menu.addSeparator();
    menu.addAction(m_autoStartAct);

    QAction *chosen = menu.exec(centralWidget()->mapToGlobal(pos));
    if(chosen == startAct)
        startBackend();
    else if(chosen == stopAct)
        stopBackend();
}

/*
 * Update the small status text at the bottom of the screen
 * It also updates the window title with the latest status
 */
void MainWindow::setStatus(const QString &text)
{
    if(m_statusLine) {
        m_statusLine->setText(text);
    }
    if(text.isEmpty()) {
        setWindowTitle(QStringLiteral("SPAT Viewer"));
        return;
    }
    constexpr int kMaxTitle = 72;
    const QString tail = text.size() <= kMaxTitle ? text : (text.left(kMaxTitle) + QChar(0x2026));
    setWindowTitle(QStringLiteral("SPAT Viewer — %1").arg(tail));
}

/*
 * Read the saved folder path where the backend C decoder program is located
 */
QString MainWindow::examplesWslPath() const
{
    QSettings s;
    return s.value(QString::fromLatin1(kKeyExamplesPath)).toString().trimmed();
}

/*
 * Read the saved IP address or hostname of the Commsignia device
 */
QString MainWindow::deviceAddress() const
{
    QSettings s;
    return s.value(QString::fromLatin1(kKeyLastAddress)).toString().trimmed();
}

/*
 * If the user enabled auto-start and saved the needed settings,
 * start the backend automatically
 */
void MainWindow::tryAutoStart()
{
    QSettings s;
    if(!s.value(QString::fromLatin1(kKeyAutoStart), true).toBool()) {
        return;
    }
    if(examplesWslPath().isEmpty() || deviceAddress().isEmpty()) {
        return;
    }
    startBackend();
}

/*
 * Ask the user where the SDK examples folder is
 * This is needed so Qt can find and run fac_subs_decode_with_logging
 */
void MainWindow::openExamplesFolderSettings()
{
    const QString current = examplesWslPath();
    bool ok = false;
#ifdef Q_OS_WIN
    const QString hint = QStringLiteral(
        "WSL path to the SDK examples folder (where fac_subs_decode_with_logging was built),\n"
        "e.g. /mnt/c/Users/you/.../examples");
#else
    const QString hint = QStringLiteral(
        "Full path to the SDK examples folder (where fac_subs_decode_with_logging was built),\n"
        "e.g. /home/you/.../examples");
#endif
    const QString text = QInputDialog::getText(this, QStringLiteral("SDK examples folder"), hint,
                                               QLineEdit::Normal, current, &ok);
    if(ok) {
        QSettings s;
        s.setValue(QString::fromLatin1(kKeyExamplesPath), text.trimmed());
        setStatus(QStringLiteral("SDK folder saved."));
    }
}

/*
 * Ask the user for the Commsignia OBU/RSU address
 * Example: 192.168.1.54
 */
void MainWindow::openDeviceAddressSettings()
{
    const QString current = deviceAddress();
    bool ok = false;
    const QString text = QInputDialog::getText(this, QStringLiteral("Device address"),
                                               QStringLiteral("IP address or hostname of the onboard unit (OBU), "
                                                              "e.g. 192.168.1.54"),
                                               QLineEdit::Normal, current, &ok);
    if(ok) {
        QSettings s;
        s.setValue(QString::fromLatin1(kKeyLastAddress), text.trimmed());
        setStatus(QStringLiteral("Device address saved."));
    }
}

/*
 * When the window closes, stop the backend program too
 */
MainWindow::~MainWindow()
{
    stopBackend();
}

/*
 * Start the C decoder program in the background
 *
 * This is the bridge between the old terminal program and the new GUI:
 * - the C program receives V2X data
 * - the C program prints JSON lines
 * - this Qt program reads those JSON lines
 * - the UI updates the circle, arrow, countdown, and intersection name
 */
void MainWindow::startBackend()
{
    if(m_proc)
        return;

    QString examples = examplesWslPath();
#ifdef Q_OS_WIN
    examples = toWslPath(examples);
#endif
    if(examples.isEmpty()) {
#ifdef Q_OS_WIN
        const QString msg = QStringLiteral(
            "Right-click this window, choose SDK examples folder, and enter the WSL path to your SDK "
            "examples directory (where you ran make).");
#else
        const QString msg = QStringLiteral(
            "Right-click this window, choose SDK examples folder, and enter the full path to your SDK "
            "examples directory (where you built fac_subs_decode_with_logging).");
#endif
        QMessageBox::information(this, QStringLiteral("SDK folder"), msg);
        openExamplesFolderSettings();
        return;
    }

    const QString ip = deviceAddress();
    if(ip.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Address"),
                             QStringLiteral("Right-click this window and choose Device address."));
        openDeviceAddressSettings();
        return;
    }

    QSettings s;
    s.setValue(QString::fromLatin1(kKeyLastAddress), ip);

    // QProcess lets this Qt app run the C decoder program in the background
    m_proc = new QProcess(this);
    m_errBuf.clear();
#ifdef Q_OS_WIN
    QString escaped = examples;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    const QString inner = QStringLiteral(
                              "cd '%1' && test -x ./fac_subs_decode_with_logging && "
                              "./fac_subs_decode_with_logging %2 --json")
                              .arg(escaped, ip);
    m_proc->setProgram(QStringLiteral("wsl"));
    // -ilc: login + interactive bash so ~/.bashrc (e.g. LD_LIBRARY_PATH) matches a normal terminal.
    m_proc->setArguments({QStringLiteral("bash"), QStringLiteral("-ilc"), inner});
#else
    const QString decoder =
        QDir::cleanPath(QDir(examples).filePath(QStringLiteral("fac_subs_decode_with_logging")));
    m_proc->setProgram(decoder);
    m_proc->setArguments({ip, QStringLiteral("--json")});
    m_proc->setWorkingDirectory(examples);
#endif

    // When the backend prints normal JSON output, onStdout() reads it
    // When the backend prints status/error text, onStderr() reads it
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &MainWindow::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this, &MainWindow::onStderr);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &MainWindow::onProcessFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &MainWindow::onProcessError);

    // Start the backend program
    m_buf.clear();
    m_proc->start();
    if(!m_proc->waitForStarted(5000)) {
#ifdef Q_OS_WIN
        const QString err = QStringLiteral(
            "The link to the decoder could not be started. Check that WSL is installed and available.");
#else
        const QString err = QStringLiteral(
            "The decoder could not be started. Check the examples path, that the binary exists, "
            "and that it is executable (chmod +x).");
#endif
        QMessageBox::critical(this, QStringLiteral("Could not start"), err);
        delete m_proc;
        m_proc = nullptr;
        return;
    }

    setStatus(QStringLiteral("Connecting…"));
}

/*
 * Stop the backend C decoder program
 */
void MainWindow::stopBackend()
{
    if(!m_proc)
        return;
    m_proc->kill();
    m_proc->waitForFinished(3000);
    m_proc->deleteLater();
    m_proc = nullptr;
    m_errBuf.clear();
    setStatus(QStringLiteral("Stopped."));
}

/*
 * Read error/status messages from the backend
 * These are shown in the small status line at the bottom of the GUI
 */
void MainWindow::onStderr()
{
    if(!m_proc)
        return;
    m_errBuf += m_proc->readAllStandardError();
    while(true) {
        int nl = m_errBuf.indexOf('\n');
        if(nl < 0)
            break;
        QByteArray line = m_errBuf.left(nl);
        m_errBuf = m_errBuf.mid(nl + 1);
        const QString t = QString::fromUtf8(line.trimmed());
        if(!t.isEmpty())
            setStatus(QStringLiteral("Decoder: %1").arg(t));
    }
}

/*
 * Read normal output from the backend
 *
 * The backend sends JSON lines
 * Each JSON line describes one update, such as:
 *   current color
 *   next color
 *   seconds remaining
 *   intersection name
 *   arrow direction
 *
 * This function reads those lines and decides how to update the GUI
 */
void MainWindow::onStdout()
{
    if(!m_proc)
        return;
    m_buf += m_proc->readAllStandardOutput();
    while(true) {
        int nl = m_buf.indexOf('\n');
        if(nl < 0)
            break;
        QByteArray line = m_buf.left(nl);
        m_buf = m_buf.mid(nl + 1);
        line = line.trimmed();
        if(line.isEmpty())
            continue;
        // Only handle JSON lines. JSON lines start with "{". Other text is ignored
        if(!line.startsWith('{'))
            continue;
        // Turn the JSON text into a Qt JSON object
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
        if(pe.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        QJsonObject o = doc.object();
        const QString t = o.value(QStringLiteral("type")).toString();
        // The backend is running and ready to receive signal updates
        if(t == QStringLiteral("ready")) {
            setStatus(QStringLiteral("Connected — waiting for signal updates."));
            continue;
        }
        // MAP update: use it to show the intersection ID if a name is not available yet
        if(t == QStringLiteral("map")) {
            const int iid = o.value(QStringLiteral("intersectionId")).toInt(-1);
            if(iid >= 0)
                m_intersectionTitle->setText(QStringLiteral("Intersection %1").arg(iid));
            else
                m_intersectionTitle->setText(QStringLiteral("—"));
            continue;
        }
        if(t == QStringLiteral("position")) {
            continue;
        }
        // Signal-group update: this contains the live light state shown on the UI
        if(o.contains(QStringLiteral("sg")))
            applyState(o);
    }
}

/*
 * This runs when the backend program stops
 * It shows a message so the user knows the connection ended
 */
void MainWindow::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);
    if(m_proc) {
        const QByteArray restOut = m_proc->readAllStandardOutput();
        const QByteArray restErr = m_proc->readAllStandardError();
        if(!restErr.trimmed().isEmpty()) {
            setStatus(QStringLiteral("Ended (%1): %2")
                          .arg(exitCode)
                          .arg(QString::fromUtf8(restErr.trimmed()).left(200)));
        } else if(!restOut.trimmed().isEmpty()) {
            setStatus(QStringLiteral("Ended (%1): %2")
                          .arg(exitCode)
                          .arg(QString::fromUtf8(restOut.trimmed()).left(200)));
        } else {
            setStatus(QStringLiteral("Session ended (code %1).").arg(exitCode));
        }
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_errBuf.clear();
}

/*
 * This runs if Qt cannot start or communicate with the backend
 */
void MainWindow::onProcessError(QProcess::ProcessError err)
{
#ifdef Q_OS_WIN
    const QString hint = QStringLiteral("Check WSL and try again.");
#else
    const QString hint = QStringLiteral("Check the decoder path and device address.");
#endif
    setStatus(QStringLiteral("Connection error (%1). %2").arg(static_cast<int>(err)).arg(hint));
}

/*
 * Convert a simple direction word into an arrow symbol
 *
 * Example:
 *   "left"     ->
 *   "right"    <-
 *   "straight" ↑
 */
QString MainWindow::laneArrowGlyph(const QString &arrowKey)
{
    const QString k = arrowKey.trimmed().toLower();
    if(k == QStringLiteral("left") || k == QStringLiteral("l"))
        return QStringLiteral("←");
    if(k == QStringLiteral("right") || k == QStringLiteral("r"))
        return QStringLiteral("→");
    if(k == QStringLiteral("uturn") || k == QStringLiteral("u") || k == QStringLiteral("u-turn"))
        return QStringLiteral("\u21B6");
    if(k == QStringLiteral("straight") || k == QStringLiteral("s") || k.isEmpty())
        return QStringLiteral("↑");
    return QStringLiteral("↑");
}

/*
 * Convert a color name into the style used by the circle on the screen
 *
 * Example:
 *   "GREEN"  makes the circle green
 *   "YELLOW" makes the circle yellow
 *   "RED"    makes the circle red
 */
QString MainWindow::colorStyle(const QString &name, bool largeDot)
{
    if(largeDot) {
        if(name == QStringLiteral("GREEN"))
            return QStringLiteral("color: #2ecc71;");
        if(name == QStringLiteral("YELLOW"))
            return QStringLiteral("color: #f1c40f;");
        if(name == QStringLiteral("RED"))
            return QStringLiteral("color: #e74c3c;");
        return QStringLiteral("color: #7f8c8d;");
    }
    if(name == QStringLiteral("GREEN"))
        return QStringLiteral("color: #2ecc71;");
    if(name == QStringLiteral("YELLOW"))
        return QStringLiteral("color: #f1c40f;");
    if(name == QStringLiteral("RED"))
        return QStringLiteral("color: #e74c3c;");
    return QStringLiteral("color: #95a5a6;");
}

/*
 * Apply one live signal update to the screen
 *
 * This is the main function that makes the screenshot happen
 * It takes the JSON data from the backend and updates:
 *   - intersection name
 *   - current light circle
 *   - next light circle
 *   - countdown seconds
 *   - lane arrow
 */
void MainWindow::applyState(const QJsonObject &o)
{
    // Get the intersection name or ID from the backend
    const QString ixName = o.value(QStringLiteral("intersection_name")).toString().trimmed();
    const int ixId = o.value(QStringLiteral("intersection_id")).toInt(-99999);
    if(!ixName.isEmpty() && ixName != QStringLiteral("Unknown intersection"))
        m_intersectionTitle->setText(normalizedIntersectionTitle(ixName));
    else if(ixId >= 0)
        m_intersectionTitle->setText(QStringLiteral("Intersection %1").arg(ixId));
    /* else keep previous title until a line includes a name or id */

    // Get the live values that control the screen
    QString cur = o.value(QStringLiteral("current")).toString();
    QString next = o.value(QStringLiteral("next")).toString();
    int sec = o.value(QStringLiteral("sec")).toInt(-999);
    QString arrow = o.value(QStringLiteral("arrow")).toString();
    if(arrow.isEmpty())
        arrow = o.value(QStringLiteral("laneArrow")).toString();

    // Update the large current light, the small next light, and the arrow
    m_curDot->setStyleSheet(colorStyle(cur, true));
    m_nextDot->setStyleSheet(colorStyle(next, false));
    m_laneArrow->setText(laneArrowGlyph(arrow));

    // Update the countdown number
    if(sec >= 0)
        m_secValueLabel->setText(QString::number(sec));
    else
        m_secValueLabel->setText(QStringLiteral("—"));
}
