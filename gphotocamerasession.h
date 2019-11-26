#ifndef GPHOTOCAMERASESSION_H
#define GPHOTOCAMERASESSION_H

#include <memory>

#include <QAbstractVideoSurface>
#include <QCamera>
#include <QCameraImageCapture>
#include <QMap>
#include <QPointer>

using CaptureDestinations = QCameraImageCapture::CaptureDestinations;

class GPhotoCameraWorker;
class GPhotoFactory;

class GPhotoCameraSession final : public QObject
{
    Q_OBJECT
public:
    explicit GPhotoCameraSession(GPhotoFactory *factory, QObject *parent = nullptr);
    ~GPhotoCameraSession();

    GPhotoCameraSession(GPhotoCameraSession&&) = delete;
    GPhotoCameraSession& operator=(GPhotoCameraSession&&) = delete;

    // camera control
    QCamera::State state() const;
    void setState(QCamera::State state);
    QCamera::Status status() const;

    bool isCaptureModeSupported(QCamera::CaptureModes mode) const;
    QCamera::CaptureModes captureMode() const;
    void setCaptureMode(QCamera::CaptureModes captureMode);

    // destination control
    bool isCaptureDestinationSupported(CaptureDestinations destination) const;
    QCameraImageCapture::CaptureDestinations captureDestination() const;
    void setCaptureDestination(QCameraImageCapture::CaptureDestinations destination);

    // capture control
    bool isReadyForCapture() const;
    int capture(const QString &fileName);

    // video renderer control
    QAbstractVideoSurface* surface() const;
    void setSurface(QAbstractVideoSurface *surface);

    // options control
    QVariant parameter(const QString &name);
    bool setParameter(const QString &name, const QVariant &value);

    void setCamera(int cameraIndex);

signals:
    // camera control
    void statusChanged(QCamera::Status);
    void stateChanged(QCamera::State);
    void error(int error, const QString &errorString);
    void captureModeChanged(QCamera::CaptureModes);

    // capture destination control
    void captureDestinationChanged(QCameraImageCapture::CaptureDestinations destination);

    // image capture control
    void readyForCaptureChanged(bool);
    void imageCaptured(int id, const QImage &preview);
    void imageAvailable(int id, const QVideoFrame &buffer);
    void imageSaved(int id, const QString &fileName);
    void imageCaptureError(int id, int error, const QString &errorString);

    // video probe control
    void videoFrameProbed(const QVideoFrame &frame);

private slots:
    void onPreviewCaptured(const QImage &image);
    void onImageDataCaptured(int id, const QByteArray &imageData, const QString &fileName);
    void onWorkerStateChanged(QCamera::State);
    void onWorkerStatusChanged(QCamera::Status);
    void onCaptureModeChanged(QCamera::CaptureMode);
    void onWorkerReadyForCaptureChanged(bool);

private:
    Q_DISABLE_COPY(GPhotoCameraSession)

    std::shared_ptr<GPhotoCameraWorker> getWorker(int cameraIndex);

    GPhotoFactory *const m_factory;
    std::unique_ptr<QThread> m_workerThread;
    QCamera::State m_state = QCamera::UnloadedState;
    QCamera::Status m_status = QCamera::UnloadedStatus;
    QCamera::CaptureModes m_captureMode = QCamera::CaptureStillImage;
    CaptureDestinations m_captureDestination = QCameraImageCapture::CaptureToBuffer | QCameraImageCapture::CaptureToFile;
    QPointer<QAbstractVideoSurface> m_surface;
    QMap<int, std::shared_ptr<GPhotoCameraWorker>> m_workers;
    std::shared_ptr<GPhotoCameraWorker> m_currentWorker;
    int m_lastImageCaptureId = 0;
    bool m_readyForCapture = false;
};

#endif // GPHOTOCAMERASESSION_H
