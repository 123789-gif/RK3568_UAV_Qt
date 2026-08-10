#include "system/process_signal_bridge.h"

#include <QSocketNotifier>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

volatile sig_atomic_t ProcessSignalBridge::s_writeFd = -1;

namespace {

bool setFdFlags(int fd, int command, int flag)
{
    const int oldFlags = ::fcntl(fd, command);
    return oldFlags >= 0 && ::fcntl(fd, command == F_GETFL ? F_SETFL : F_SETFD,
                                    oldFlags | flag) == 0;
}

} // namespace

ProcessSignalBridge::ProcessSignalBridge(QObject *parent)
    : QObject(parent)
{
    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        m_error = QStringLiteral("pipe failed: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }

    m_readFd = pipeFds[0];
    m_writeFd = pipeFds[1];
    if (!setFdFlags(m_readFd, F_GETFL, O_NONBLOCK) ||
        !setFdFlags(m_writeFd, F_GETFL, O_NONBLOCK) ||
        !setFdFlags(m_readFd, F_GETFD, FD_CLOEXEC) ||
        !setFdFlags(m_writeFd, F_GETFD, FD_CLOEXEC)) {
        m_error = QStringLiteral("fcntl on signal pipe failed: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(m_readFd);
        ::close(m_writeFd);
        m_readFd = -1;
        m_writeFd = -1;
        return;
    }

    struct sigaction action {};
    action.sa_handler = &ProcessSignalBridge::signalHandler;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    s_writeFd = static_cast<sig_atomic_t>(m_writeFd);
    if (::sigaction(SIGTERM, &action, &m_oldSigterm) != 0) {
        m_error = QStringLiteral("sigaction(SIGTERM) failed: %1")
                .arg(QString::fromLocal8Bit(std::strerror(errno)));
        s_writeFd = -1;
        ::close(m_readFd);
        ::close(m_writeFd);
        m_readFd = -1;
        m_writeFd = -1;
        return;
    }

    if (::sigaction(SIGINT, &action, &m_oldSigint) != 0) {
        const int savedErrno = errno;
        ::sigaction(SIGTERM, &m_oldSigterm, nullptr);
        s_writeFd = -1;
        ::close(m_readFd);
        ::close(m_writeFd);
        m_readFd = -1;
        m_writeFd = -1;
        m_error = QStringLiteral("sigaction(SIGINT) failed: %1")
                .arg(QString::fromLocal8Bit(std::strerror(savedErrno)));
        return;
    }

    m_notifier = new QSocketNotifier(m_readFd, QSocketNotifier::Read, this);
    connect(m_notifier,
            static_cast<void (QSocketNotifier::*)(int)>(&QSocketNotifier::activated),
            this,
            [this](int) { drainSignals(); });
    m_installed = true;
}

ProcessSignalBridge::~ProcessSignalBridge()
{
    if (m_notifier != nullptr) {
        m_notifier->setEnabled(false);
    }

    if (m_installed) {
        ::sigaction(SIGTERM, &m_oldSigterm, nullptr);
        ::sigaction(SIGINT, &m_oldSigint, nullptr);
    }

    s_writeFd = -1;
    if (m_readFd >= 0) {
        ::close(m_readFd);
    }
    if (m_writeFd >= 0) {
        ::close(m_writeFd);
    }
}

bool ProcessSignalBridge::isInstalled() const
{
    return m_installed;
}

QString ProcessSignalBridge::errorString() const
{
    return m_error;
}

void ProcessSignalBridge::signalHandler(int signalNumber)
{
    const int savedErrno = errno;
    const int fd = static_cast<int>(s_writeFd);
    if (fd >= 0) {
        const unsigned char value = static_cast<unsigned char>(signalNumber);
        (void)::write(fd, &value, sizeof(value));
    }
    errno = savedErrno;
}

void ProcessSignalBridge::drainSignals()
{
    unsigned char signals[32];
    for (;;) {
        const ssize_t count = ::read(m_readFd, signals, sizeof(signals));
        if (count > 0) {
            for (ssize_t i = 0; i < count; ++i) {
                emit terminationRequested(static_cast<int>(signals[i]));
            }
            continue;
        }

        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}
