#include "StartupWindow.hpp"

#include "AccountService.hpp"
#include "Theme.hpp"

#include <algorithm>
#include <QApplication>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kStartupWidth = 460;
constexpr int kCompactHeight = 350;
constexpr int kLoginHeight = 470;

} // namespace

StartupWindow::StartupWindow(account::Service* service, QWidget* parent)
    : QDialog(parent), m_service(service),
      m_russian(QLocale::system().language() == QLocale::Russian) {
    setObjectName(QStringLiteral("StartupWindow"));
    setWindowTitle(QStringLiteral("VLT Studio Pro"));
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                   Qt::WindowCloseButtonHint);
    setModal(true);
    setFixedSize(kStartupWidth, kCompactHeight);

    m_logo = new QLabel(this);
    m_logo->setObjectName(QStringLiteral("StartupLogo"));
    m_logo->setFixedSize(76, 76);
    m_logo->setAlignment(Qt::AlignCenter);
    m_logo->setAccessibleName(
        local("Логотип VLT Studio Pro", "VLT Studio Pro logo"));
    const QPixmap logo(QStringLiteral(":/vlt/icon-1024.png"));
    if (!logo.isNull()) {
        m_logo->setPixmap(logo.scaled(QSize(60, 60), Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
    }

    auto* title = new QLabel(QStringLiteral("VLT STUDIO PRO"), this);
    title->setObjectName(QStringLiteral("StartupTitle"));
    title->setAlignment(Qt::AlignCenter);
    QFont titleFont = title->font();
    titleFont.setPixelSize(20);
    titleFont.setBold(true);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    title->setFont(titleFont);

    auto* product = new QLabel(
        local("Цифровая звуковая рабочая станция",
              "Digital audio workstation"), this);
    product->setObjectName(QStringLiteral("StartupProduct"));
    product->setAlignment(Qt::AlignCenter);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("StartupStatus"));
    m_status->setAlignment(Qt::AlignCenter);
    QFont statusFont = m_status->font();
    statusFont.setPixelSize(14);
    statusFont.setBold(true);
    m_status->setFont(statusFont);

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("StartupDetail"));
    m_detail->setAlignment(Qt::AlignCenter);
    m_detail->setWordWrap(true);
    m_detail->setMinimumHeight(32);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("StartupProgress"));
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->setAccessibleName(
        local("Ход запуска приложения", "Application startup progress"));

    m_loginPanel = new QFrame(this);
    m_loginPanel->setObjectName(QStringLiteral("StartupLoginPanel"));
    auto* loginColumn = new QVBoxLayout(m_loginPanel);
    loginColumn->setContentsMargins(16, 14, 16, 14);
    loginColumn->setSpacing(10);

    m_email = new QLineEdit(m_loginPanel);
    m_email->setObjectName(QStringLiteral("StartupEmail"));
    m_email->setInputMethodHints(Qt::ImhEmailCharactersOnly);
    m_email->setClearButtonEnabled(true);
    m_email->setAccessibleName(local("Почта", "Email"));
    m_password = new QLineEdit(m_loginPanel);
    m_password->setObjectName(QStringLiteral("StartupPassword"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setAccessibleName(local("Пароль", "Password"));
    connect(m_password, &QLineEdit::returnPressed, this, &StartupWindow::submit);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    form->addRow(local("Почта", "Email"), m_email);
    form->addRow(local("Пароль", "Password"), m_password);
    loginColumn->addLayout(form);

    m_login = new QPushButton(local("Войти", "Sign in"), m_loginPanel);
    m_login->setObjectName(QStringLiteral("StartupLoginButton"));
    m_login->setDefault(true);
    connect(m_login, &QPushButton::clicked, this, &StartupWindow::submit);
    loginColumn->addWidget(m_login);

    auto* links = new QHBoxLayout;
    links->setContentsMargins(0, 0, 0, 0);
    auto* registerButton = new QPushButton(
        local("Создать аккаунт", "Create account"), m_loginPanel);
    registerButton->setFlat(true);
    connect(registerButton, &QPushButton::clicked, this,
            [this] { openSite(QStringLiteral("register")); });
    auto* resetButton = new QPushButton(
        local("Забыли пароль?", "Forgot password?"), m_loginPanel);
    resetButton->setFlat(true);
    connect(resetButton, &QPushButton::clicked, this,
            [this] { openSite(QStringLiteral("forgot-password")); });
    links->addWidget(registerButton);
    links->addStretch(1);
    links->addWidget(resetButton);
    loginColumn->addLayout(links);
    m_loginPanel->hide();

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(32, 22, 32, 24);
    column->setSpacing(8);
    column->addStretch(1);
    column->addWidget(m_logo, 0, Qt::AlignHCenter);
    column->addWidget(title);
    column->addWidget(product);
    column->addSpacing(8);
    column->addWidget(m_status);
    column->addWidget(m_detail);
    column->addWidget(m_progress);
    column->addSpacing(2);
    column->addWidget(m_loginPanel);
    column->addStretch(1);

    connect(service, &account::Service::busyChanged, this, [this](bool busy) {
        m_login->setDisabled(busy);
        m_email->setDisabled(busy);
        m_password->setDisabled(busy);
        if (busy) {
            setStage(local("Проверяем лицензию…", "Checking license…"),
                     local("Восстанавливаем сохранённую сессию аккаунта",
                           "Restoring the saved account session"), true);
        }
    });
    connect(service, &account::Service::authenticatedChanged, this,
            [this](bool authenticated) {
                if (!authenticated) return;
                m_loginPanel->hide();
                syncWindowSize();
                setStage(local("Лицензия подтверждена", "License confirmed"),
                         m_service->snapshot().email.isEmpty()
                             ? local("Аккаунт готов", "Account ready")
                             : m_service->snapshot().email,
                         false);
            });
    connect(service, &account::Service::authenticationRequired, this,
            [this](const QString& reason, bool) {
                revealLogin(reason, reason != tr("Sign in to continue."));
            });
    connect(service, &account::Service::errorOccurred, this,
            [this](const QString&, const QString& message) {
                revealLogin(message, true);
            });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &StartupWindow::applyTheme);

    setStage(local("Подготовка запуска…", "Preparing startup…"),
             local("Проверяем сохранённую лицензию", "Checking saved license"),
             true);
    syncWindowSize();
    applyTheme();
}

bool StartupWindow::runAuthentication() {
    if (!m_service) return false;
    m_cancelled = false;
    show();
    raise();
    activateWindow();

    if (m_service->authenticated()) return true;
    QEventLoop wait;
    const QMetaObject::Connection authenticated = connect(
        m_service, &account::Service::authenticatedChanged, &wait,
        [&wait](bool ready) { if (ready) wait.quit(); });
    const QMetaObject::Connection rejected = connect(
        this, &QDialog::rejected, &wait, &QEventLoop::quit);
    if (!m_restoreStarted) {
        m_restoreStarted = true;
        QTimer::singleShot(0, m_service, &account::Service::beginRestore);
    }
    wait.exec();
    disconnect(authenticated);
    disconnect(rejected);
    return !m_cancelled && m_service->authenticated();
}

void StartupWindow::showSystemLoading() {
    m_loginPanel->hide();
    syncWindowSize();
    setStage(local("Загружаем систему…", "Loading system…"),
             local("Подготавливаем аудиодвижок и интерфейс",
                   "Preparing the audio engine and interface"), true);
}

void StartupWindow::showPluginScan(std::uint32_t done, std::uint32_t total,
                                   const QString& currentPath) {
    m_loginPanel->hide();
    syncWindowSize();
    const QString file = currentPath.isEmpty()
                             ? QString()
                             : QFileInfo(currentPath).fileName();
    m_status->setText(local("Проверяем плагины…", "Checking plugins…"));
    if (total == 0) {
        m_progress->setRange(0, 0);
        m_detail->setText(local("Ищем установленные плагины",
                                "Looking for installed plugins"));
        return;
    }
    m_progress->setRange(0, int(total));
    m_progress->setValue(int(std::min(done, total)));
    m_detail->setText(
        local("%1 из %2%3", "%1 of %2%3")
            .arg(done).arg(total)
            .arg(file.isEmpty() ? QString() : QStringLiteral("  ·  ") + file));
    m_detail->setToolTip(currentPath);
}

void StartupWindow::showReady(int pluginCount) {
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    setStage(local("Готово", "Ready"),
             local("Доступно плагинов: %1", "%1 plugins available")
                 .arg(pluginCount), false);
}

void StartupWindow::reject() {
    m_cancelled = true;
    QDialog::reject();
}

void StartupWindow::submit() {
    if (!m_service) return;
    if (m_email->text().trimmed().isEmpty() || m_password->text().size() < 12) {
        setStatusError(local("Введите почту и пароль не короче 12 символов.",
                             "Enter your email and a password of at least 12 characters."));
        return;
    }
    m_service->login(m_email->text(), m_password->text());
}

void StartupWindow::revealLogin(const QString& reason, bool error) {
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_status->setText(local("Требуется вход", "Sign-in required"));
    m_detail->setText(reason);
    m_detail->setProperty("error", error);
    m_detail->style()->unpolish(m_detail);
    m_detail->style()->polish(m_detail);
    m_loginPanel->show();
    syncWindowSize();
    m_email->setFocus(Qt::OtherFocusReason);
}

void StartupWindow::setStage(const QString& status, const QString& detail,
                             bool indeterminate) {
    m_status->setText(status);
    m_detail->setText(detail);
    m_detail->setToolTip(QString());
    m_detail->setProperty("error", false);
    m_detail->style()->unpolish(m_detail);
    m_detail->style()->polish(m_detail);
    m_progress->setRange(0, indeterminate ? 0 : 1);
    if (!indeterminate) m_progress->setValue(status == local("Готово", "Ready") ? 1 : 0);
}

void StartupWindow::setStatusError(const QString& message) {
    m_detail->setText(message);
    m_detail->setProperty("error", true);
    m_detail->style()->unpolish(m_detail);
    m_detail->style()->polish(m_detail);
}

void StartupWindow::openSite(const QString& path) {
    QString origin = qEnvironmentVariable(
        "VLT_PUBLIC_ORIGIN", QStringLiteral("https://vltstudio.ru"));
    while (origin.endsWith('/')) origin.chop(1);
    QDesktopServices::openUrl(
        QUrl(origin + QLatin1Char('/') +
             (m_russian ? QStringLiteral("ru") : QStringLiteral("en")) +
             QLatin1Char('/') + path));
}

void StartupWindow::syncWindowSize() {
    const QSize target(kStartupWidth,
                       m_loginPanel && !m_loginPanel->isHidden()
                           ? kLoginHeight
                           : kCompactHeight);
    if (size() == target) return;

    // Expanding the sign-in form should not make the dialog jump down and to
    // the right. Preserve the visual centre once the window is on screen.
    const QPoint centre = frameGeometry().center();
    setFixedSize(target);
    if (isVisible()) move(centre.x() - width() / 2, centre.y() - height() / 2);
}

QString StartupWindow::local(const char* russian, const char* english) const {
    return QString::fromUtf8(m_russian ? russian : english);
}

void StartupWindow::applyTheme() {
    const Theme& t = th();
    const QColor border = mixColors(t.separator(), t.textPrimary, 0.10);
    setStyleSheet(QString(R"(
#StartupWindow { background: %1; }
#StartupLogo { background: %5; border: 1px solid %6; border-radius: 15px; }
#StartupTitle { color: %2; }
#StartupProduct, #StartupDetail { color: %3; }
#StartupStatus { color: %2; }
#StartupDetail[error="true"] { color: %4; }
#StartupLoginPanel { background: %5; border: 1px solid %6; border-radius: 10px; }
#StartupLoginPanel QLabel { color: %3; }
#StartupLoginPanel QLineEdit {
    min-height: 28px; color: %2; background: %7;
    border: 1px solid %6; border-radius: 7px; padding: 0 8px;
}
#StartupLoginPanel QLineEdit:focus { border-color: %8; }
#StartupLoginButton {
    min-height: 30px; color: white; background: %8;
    border: 1px solid %8; border-radius: 8px; font-weight: 600;
}
#StartupLoginButton:disabled { color: %3; background: %7; border-color: %6; }
#StartupProgress { background: %7; border: 0; border-radius: 3px; }
#StartupProgress::chunk { background: %8; border-radius: 3px; }
)")
        .arg(t.headerBackground.name(), t.textPrimary.name(),
             t.textSecondary.name(), Theme::record().name(),
             t.surfaceElevated.name(), border.name(), t.well().name(),
             t.accent.name()));
}

bool StartupWindow::checkForTest() {
    const QPixmap logo = m_logo
                             ? m_logo->pixmap(Qt::ReturnByValue)
                             : QPixmap();
    const bool hierarchy = m_logo && m_status && m_detail && m_progress &&
                           m_loginPanel && !logo.isNull() &&
                           !m_logo->accessibleName().isEmpty() &&
                           !m_status->text().isEmpty() &&
                           !m_progress->accessibleName().isEmpty();
    const bool compact = hierarchy &&
                         size() == QSize(kStartupWidth, kCompactHeight) &&
                         m_logo->size() == QSize(76, 76) &&
                         m_loginPanel->isHidden();

    // The same smoke check covers the only taller state: the sign-in panel
    // must expand the dialog, then return cleanly to the loading footprint.
    m_loginPanel->show();
    syncWindowSize();
    if (layout()) layout()->activate();
    const bool loginFits = size() == QSize(kStartupWidth, kLoginHeight) &&
                           layout() && layout()->minimumSize().height() <= height();
    m_loginPanel->hide();
    syncWindowSize();
    return compact && loginFits &&
           size() == QSize(kStartupWidth, kCompactHeight);
}
