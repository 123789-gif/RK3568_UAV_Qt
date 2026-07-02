#include "ptz/ptz_capture_worker.h"

#include <QDebug>
#include <QFile>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <setjmp.h>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <jpeglib.h>
#include <linux/videodev2.h>

namespace {

constexpr int kRequestBuffers = 4;

uint64_t nowNs()
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ULL + uint64_t(ts.tv_nsec);
}

bool xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret == 0;
}

struct CaptureBuffer {
    void *addr = nullptr;
    size_t length = 0;
};

struct CaptureState {
    int fd = -1;
    std::vector<CaptureBuffer> buffers;
};

void cleanupCapture(CaptureState &state)
{
    if (state.fd >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)ioctl(state.fd, VIDIOC_STREAMOFF, &type);
    }

    for (CaptureBuffer &buffer : state.buffers) {
        if (buffer.addr && buffer.addr != MAP_FAILED) {
            munmap(buffer.addr, buffer.length);
        }
    }
    state.buffers.clear();

    if (state.fd >= 0) {
        close(state.fd);
        state.fd = -1;
    }
}

bool initCapture(CaptureState &state,
                 const PtzResolvedDevice &config,
                 QString *error)
{
    const QByteArray encodedDevice = QFile::encodeName(config.videoDevice);
    const char *device = encodedDevice.constData();
    state.fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (state.fd < 0) {
        *error = QString("open %1 failed: %2")
                .arg(QString::fromLocal8Bit(device),
                     QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    v4l2_capability cap {};
    if (!xioctl(state.fd, VIDIOC_QUERYCAP, &cap)) {
        *error = QString("VIDIOC_QUERYCAP failed: %1").arg(strerror(errno));
        return false;
    }

    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        *error = QString("%1 does not support capture + streaming").arg(device);
        return false;
    }

    v4l2_format fmt {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<uint32_t>(config.profile.captureWidth);
    fmt.fmt.pix.height = static_cast<uint32_t>(config.profile.captureHeight);
    fmt.fmt.pix.pixelformat = config.profile.pixelFormat;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (!xioctl(state.fd, VIDIOC_S_FMT, &fmt)) {
        *error = QString("VIDIOC_S_FMT failed: %1").arg(strerror(errno));
        return false;
    }

    if (fmt.fmt.pix.pixelformat != config.profile.pixelFormat ||
        int(fmt.fmt.pix.width) != config.profile.captureWidth ||
        int(fmt.fmt.pix.height) != config.profile.captureHeight) {
        *error = QString("%1 rejected %2 %3x%4, actual=%5 %6x%7")
                .arg(config.videoDevice,
                     v4l2PixelFormatName(config.profile.pixelFormat))
                .arg(config.profile.captureWidth)
                .arg(config.profile.captureHeight)
                .arg(v4l2PixelFormatName(fmt.fmt.pix.pixelformat))
                .arg(fmt.fmt.pix.width)
                .arg(fmt.fmt.pix.height);
        return false;
    }

    v4l2_streamparm parm {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = config.profile.captureFps;
    if (!xioctl(state.fd, VIDIOC_S_PARM, &parm)) {
        *error = QString("VIDIOC_S_PARM failed: %1").arg(strerror(errno));
        return false;
    }
    const uint32_t fpsNumerator = parm.parm.capture.timeperframe.numerator;
    const uint32_t fpsDenominator = parm.parm.capture.timeperframe.denominator;
    if (fpsNumerator > 0
            && fpsDenominator != static_cast<uint32_t>(config.profile.captureFps) * fpsNumerator) {
        *error = QString("%1 rejected %2 fps, actual=%3/%4")
                .arg(config.videoDevice)
                .arg(config.profile.captureFps)
                .arg(fpsDenominator)
                .arg(fpsNumerator);
        return false;
    }

    v4l2_requestbuffers req {};
    req.count = kRequestBuffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (!xioctl(state.fd, VIDIOC_REQBUFS, &req)) {
        *error = QString("VIDIOC_REQBUFS failed: %1").arg(strerror(errno));
        return false;
    }
    if (req.count < 3) {
        *error = QString("insufficient V4L2 buffers: %1").arg(req.count);
        return false;
    }

    state.buffers.resize(req.count);
    for (uint32_t index = 0; index < req.count; ++index) {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;

        if (!xioctl(state.fd, VIDIOC_QUERYBUF, &buffer)) {
            *error = QString("VIDIOC_QUERYBUF[%1] failed: %2")
                    .arg(index).arg(strerror(errno));
            return false;
        }

        void *mapped = mmap(nullptr,
                            buffer.length,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            state.fd,
                            buffer.m.offset);
        if (mapped == MAP_FAILED) {
            *error = QString("mmap V4L2 buffer[%1] failed: %2")
                    .arg(index).arg(strerror(errno));
            return false;
        }

        state.buffers[index].addr = mapped;
        state.buffers[index].length = buffer.length;

        if (!xioctl(state.fd, VIDIOC_QBUF, &buffer)) {
            *error = QString("VIDIOC_QBUF[%1] failed: %2")
                    .arg(index).arg(strerror(errno));
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (!xioctl(state.fd, VIDIOC_STREAMON, &type)) {
        *error = QString("VIDIOC_STREAMON failed: %1").arg(strerror(errno));
        return false;
    }

    return true;
}

struct JpegErrorManager {
    jpeg_error_mgr pub {};
    jmp_buf jumpBuffer {};
    char message[JMSG_LENGTH_MAX] {};
};

void jpegErrorExit(j_common_ptr cinfo)
{
    JpegErrorManager *error = reinterpret_cast<JpegErrorManager *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, error->message);
    longjmp(error->jumpBuffer, 1);
}

bool decodeJpegPreview(const uint8_t *data,
                    size_t size,
                    QImage *image,
                    QString *errorText)
{
    if (!data || size < 4 || !image || !errorText) {
        return false;
    }

    jpeg_decompress_struct cinfo {};
    JpegErrorManager error {};
    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = jpegErrorExit;

    if (setjmp(error.jumpBuffer)) {
        jpeg_destroy_decompress(&cinfo);
        *errorText = QString::fromLocal8Bit(error.message);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo,
                 const_cast<unsigned char *>(data),
                 static_cast<unsigned long>(size));

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        *errorText = "jpeg_read_header failed";
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    // 两套PTZ均直接按Profile指定尺寸解码，不在采集线程中做二次缩放。
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    cinfo.out_color_space = JCS_RGB;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.dct_method = JDCT_FASTEST;

    if (!jpeg_start_decompress(&cinfo)) {
        *errorText = "jpeg_start_decompress failed";
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    if (cinfo.output_components != 3) {
        *errorText = QString("unexpected JPEG components: %1")
                .arg(cinfo.output_components);
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    QImage decoded(int(cinfo.output_width),
                   int(cinfo.output_height),
                   QImage::Format_RGB888);
    if (decoded.isNull()) {
        *errorText = "QImage allocation failed";
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = decoded.scanLine(int(cinfo.output_scanline));
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
            *errorText = QString("jpeg_read_scanlines failed at line %1")
                    .arg(cinfo.output_scanline);
            jpeg_abort_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            return false;
        }
    }

    if (!jpeg_finish_decompress(&cinfo)) {
        *errorText = "jpeg_finish_decompress failed";
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_destroy_decompress(&cinfo);
    *image = decoded;
    errorText->clear();
    return true;
}

} // namespace

PtzCaptureWorker::PtzCaptureWorker(QObject *parent)
    : QObject(parent)
{
}

PtzCaptureWorker::~PtzCaptureWorker()
{
    stopCapture();
}

bool PtzCaptureWorker::isRunning() const
{
    return m_running.load();
}

bool PtzCaptureWorker::setCaptureConfig(const PtzResolvedDevice &config,
                                        QString *errorMessage)
{
    if (m_running.load()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("采集运行中，必须先停止再切换相机配置");
        }
        return false;
    }
    if (!config.valid || config.videoDevice.isEmpty() || config.profile.id.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无效的PTZ视频配置");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = config;
    if (errorMessage) {
        errorMessage->clear();
    }
    return true;
}

PtzResolvedDevice PtzCaptureWorker::captureConfig() const
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
}

bool PtzCaptureWorker::takeLatestFrame(QImage *frame)
{
    if (!frame) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (!m_framePending || m_latestFrame.isNull()) {
        return false;
    }

    *frame = m_latestFrame;
    m_framePending = false;
    return true;
}

void PtzCaptureWorker::clearLatestFrame()
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    m_latestFrame = QImage();
    m_framePending = false;
}

void PtzCaptureWorker::startCapture()
{
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        if (!m_config.valid) {
            emit stateChanged(false, QStringLiteral("PTZ视频未配置，请先应用相机配置"));
            return;
        }
    }

    if (m_running.load()) {
        emit stateChanged(true, "PTZ视频已在运行");
        return;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_latestFrame = QImage();
        m_framePending = false;
    }

    m_stopReq.store(false);
    m_running.store(true);
    m_thread = std::thread(&PtzCaptureWorker::workerMain, this);
}

void PtzCaptureWorker::stopCapture()
{
    m_stopReq.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
    clearLatestFrame();
}

void PtzCaptureWorker::workerMain()
{
    PtzResolvedDevice config;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        config = m_config;
    }

    CaptureState capture;
    QString initError;
    if (!initCapture(capture, config, &initError)) {
        cleanupCapture(capture);
        m_running.store(false);
        emit stateChanged(false, initError);
        return;
    }

    emit stateChanged(true,
                      QStringLiteral("%1：%2，%3 %4x%5@%6")
                      .arg(config.profile.displayName,
                           config.videoDevice,
                           v4l2PixelFormatName(config.profile.pixelFormat))
                      .arg(config.profile.captureWidth)
                      .arg(config.profile.captureHeight)
                      .arg(config.profile.captureFps));

    QString finalMessage = "PTZ视频已停止";
    bool lostDevice = false;
    uint64_t statStart = nowNs();
    uint64_t captureCount = 0;
    uint64_t decodeCount = 0;
    uint64_t decodeFail = 0;
    uint64_t decodeNsSum = 0;

    while (!m_stopReq.load()) {
        pollfd pfd {};
        pfd.fd = capture.fd;
        pfd.events = POLLIN;

        const int pollResult = poll(&pfd, 1, 250);
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            finalMessage = QString("poll %1 failed: %2")
                    .arg(config.videoDevice,
                         QString::fromLocal8Bit(strerror(errno)));
            break;
        }
        if (pollResult == 0) {
            const QByteArray encodedPath = QFile::encodeName(config.videoDevice);
            if (::access(encodedPath.constData(), F_OK) != 0) {
                finalMessage = QStringLiteral("PTZ视频设备已拔出：%1")
                        .arg(config.videoDevice);
                lostDevice = true;
                break;
            }
            continue;
        }

        const bool hardPollFailure = pfd.revents & (POLLHUP | POLLNVAL);
        const bool pollErrorWithoutData = (pfd.revents & POLLERR)
                && !(pfd.revents & POLLIN);
        if (hardPollFailure || pollErrorWithoutData) {
            finalMessage = QStringLiteral("PTZ视频设备失效：%1，poll revents=0x%2")
                    .arg(config.videoDevice)
                    .arg(static_cast<unsigned int>(pfd.revents), 0, 16);
            lostDevice = true;
            break;
        }

        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;

        if (!xioctl(capture.fd, VIDIOC_DQBUF, &buffer)) {
            if (errno == EAGAIN) {
                continue;
            }
            finalMessage = QString("VIDIOC_DQBUF failed on %1: %2")
                    .arg(config.videoDevice,
                         QString::fromLocal8Bit(strerror(errno)));
            lostDevice = (errno == ENODEV || errno == EIO || errno == EPIPE);
            break;
        }

        ++captureCount;

        if (buffer.index < capture.buffers.size() && buffer.bytesused > 0) {
            QImage decoded;
            QString decodeError;
            const uint64_t begin = nowNs();
            const bool ok = decodeJpegPreview(
                        static_cast<const uint8_t *>(capture.buffers[buffer.index].addr),
                        size_t(buffer.bytesused),
                        &decoded,
                        &decodeError);
            const uint64_t end = nowNs();
            decodeNsSum += end - begin;

            if (ok) {
                ++decodeCount;
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_latestFrame = decoded;
                m_framePending = true;
            } else {
                ++decodeFail;
                if (decodeFail <= 5) {
                    qWarning() << "PTZ JPEG decode failed:" << decodeError;
                }
            }
        }

        if (!xioctl(capture.fd, VIDIOC_QBUF, &buffer)) {
            finalMessage = QString("VIDIOC_QBUF failed on %1: %2")
                    .arg(config.videoDevice,
                         QString::fromLocal8Bit(strerror(errno)));
            lostDevice = (errno == ENODEV || errno == EIO || errno == EPIPE);
            break;
        }

        const uint64_t now = nowNs();
        if (now - statStart >= 1000000000ULL) {
            const double seconds = double(now - statStart) / 1e9;
            const double captureFps = captureCount / seconds;
            const double decodeFps = decodeCount / seconds;
            const double decodeAvgUs = (decodeCount + decodeFail) > 0
                    ? double(decodeNsSum) / double(decodeCount + decodeFail) / 1000.0
                    : 0.0;

            emit statsUpdated(captureFps,
                              decodeFps,
                              decodeAvgUs,
                              int(decodeFail));

            captureCount = 0;
            decodeCount = 0;
            decodeFail = 0;
            decodeNsSum = 0;
            statStart = now;
        }
    }

    cleanupCapture(capture);
    m_running.store(false);
    if (lostDevice) {
        clearLatestFrame();
    }
    emit stateChanged(false, finalMessage);
    if (lostDevice && !m_stopReq.load()) {
        emit deviceLost(config.videoDevice, finalMessage);
    }
}

