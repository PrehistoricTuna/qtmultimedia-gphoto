#include <QCameraImageCapture>
#include <QThread>

#include "gphotocamera.h"

namespace {
    constexpr auto capturingFailLimit = 10;
    constexpr auto viewfinderParameter = "viewfinder";
}

using CameraWidgetPtr = std::unique_ptr<CameraWidget, int (*)(CameraWidget*)>;

QDebug operator<<(QDebug dbg, const CameraWidgetType &t)
{
    switch (t) {
    case GP_WIDGET_WINDOW:
        dbg.nospace() << "GP_WIDGET_WINDOW";
        break;
    case GP_WIDGET_SECTION:
        dbg.nospace() << "GP_WIDGET_SECTION";
        break;
    case GP_WIDGET_TEXT:
        dbg.nospace() << "GP_WIDGET_TEXT";
        break;
    case GP_WIDGET_RANGE:
        dbg.nospace() << "GP_WIDGET_RANGE";
        break;
    case GP_WIDGET_TOGGLE:
        dbg.nospace() << "GP_WIDGET_TOGGLE";
        break;
    case GP_WIDGET_RADIO:
        dbg.nospace() << "GP_WIDGET_RADIO";
        break;
    case GP_WIDGET_MENU:
        dbg.nospace() << "GP_WIDGET_MENU";
        break;
    case GP_WIDGET_BUTTON:
        dbg.nospace() << "GP_WIDGET_BUTTON";
        break;
    case GP_WIDGET_DATE:
        dbg.nospace() << "GP_WIDGET_DATE";
        break;
    }

    return dbg.space();
}

GPhotoCamera::GPhotoCamera(GPContext *context, const CameraAbilities &abilities,
                           const GPPortInfo &portInfo, QObject *parent)
    : QObject(parent)
    , m_context(context)
    , m_abilities(abilities)
    , m_portInfo(portInfo)
    , m_camera(nullptr, gp_camera_free)
    , m_file(nullptr, gp_file_free)
{
    connect(this, &GPhotoCamera::previewCaptured, this, &GPhotoCamera::capturePreview, Qt::QueuedConnection);
}

GPhotoCamera::~GPhotoCamera()
{
    closeCamera();
}

void GPhotoCamera::setState(QCamera::State state)
{
    if (m_state == state)
        return;

    auto previousState = m_state;

    if (QCamera::UnloadedState == previousState) {
        if (QCamera::LoadedState == state) {
            openCamera();
        } else if (QCamera::ActiveState == state) {
            startViewFinder();
        }
    } else if (QCamera::LoadedState == previousState) {
        if (QCamera::UnloadedState == state) {
            closeCamera();
        } else if (QCamera::ActiveState == state) {
            startViewFinder();
        }
    } else if (QCamera::ActiveState == previousState) {
        if (QCamera::UnloadedState == state) {
            closeCamera();
        } else if (QCamera::LoadedState == state) {
            stopViewFinder();
        }
    }
}

void GPhotoCamera::setCaptureMode(QCamera::CaptureModes captureMode)
{
    if (m_captureMode != captureMode) {
        m_captureMode = captureMode;
        emit captureModeChanged(captureMode);
    }
}

void GPhotoCamera::capturePhoto(int id, const QString &fileName)
{
    if (!isReadyForCapture()) {
        emit imageCaptureError(id, QCameraImageCapture::NotReadyError, "Camera is not ready");
        return;
    }

    setMirrorPosition(MirrorPosition::Down);

    // Capture the frame from camera
    CameraFilePath filePath;
    int ret = gp_camera_capture(m_camera.get(), GP_CAPTURE_IMAGE, &filePath, m_context);

    if (ret < GP_OK) {
        qWarning() << "Failed to capture frame:" << ret;
        emit imageCaptureError(id, QCameraImageCapture::ResourceError, "Failed to capture frame");
    } else {
        qDebug() << "Captured frame:" << filePath.folder << filePath.name;

        // Download the file
        CameraFile* file;
        gp_file_new(&file);

        // Unique pointer will free memory on exit
        auto filePtr = CameraFilePtr(file, gp_file_free);

        ret = gp_camera_file_get(m_camera.get(), filePath.folder, filePath.name, GP_FILE_TYPE_NORMAL, file, m_context);

        if (ret < GP_OK) {
            qWarning() << "Failed to get file from camera:" << ret;
            emit imageCaptureError(id, QCameraImageCapture::ResourceError, "Failed to download file from camera");
        } else {
            const char* data;
            unsigned long int size = 0;

            ret = gp_file_get_data_and_size(file, &data, &size);
            if (ret < GP_OK) {
                qWarning() << "Failed to get file data and size from camera:" << ret;
                emit imageCaptureError(id, QCameraImageCapture::ResourceError, "Failed to download file from camera");
            } else {
                emit imageCaptured(id, QByteArray(data, int(size)), fileName);
            }
        }

        waitForOperationCompleted();
    }

    setMirrorPosition(MirrorPosition::Up);
}

QVariant GPhotoCamera::parameter(const QString &name)
{
    CameraWidget *root;
    auto ret = gp_camera_get_config(m_camera.get(), &root, m_context);
    if (ret < GP_OK) {
        qWarning() << "Unable to get root option from gphoto while getting parameter" << qPrintable(name);
        return QVariant();
    }

    CameraWidget *option;
    ret = gp_widget_get_child_by_name(root, qPrintable(name), &option);
    if (ret < GP_OK) {
        qWarning() << "Unable to get config widget from gphoto";
        return QVariant();
    }

    CameraWidgetType type;
    ret = gp_widget_get_type(option, &type);
    if (ret < GP_OK) {
        qWarning() << "Unable to get config widget type from gphoto";
        return QVariant();
    }

    if (type == GP_WIDGET_RADIO) {
        char *value;
        ret = gp_widget_get_value(option, &value);
        if (ret < GP_OK) {
            qWarning() << "Unable to get value for option" << qPrintable(name) << "from gphoto";
            return QVariant();
        }
        return QString::fromLocal8Bit(value);
    }

    if (type == GP_WIDGET_TOGGLE) {
        auto value = 0;
        ret = gp_widget_get_value(option, &value);
        if (ret < GP_OK) {
            qWarning() << "Unable to get value for option" << qPrintable(name) << "from gphoto";
            return QVariant();
        }
        return value != 0;
    }

    qWarning() << "Options of type" << type << "are currently not supported";
    return QVariant();
}

bool GPhotoCamera::setParameter(const QString &name, const QVariant &value)
{
    CameraWidget *root;
    auto ret = gp_camera_get_config(m_camera.get(), &root, m_context);
    if (ret < GP_OK) {
        qWarning() << "Unable to get root option from gphoto while setting parameter" << qPrintable(name);
        return false;
    }

    // Get widget pointer
    CameraWidget *option;
    ret = gp_widget_get_child_by_name(root, qPrintable(name), &option);
    if (ret < GP_OK) {
        qWarning() << "Unable to get option" << qPrintable(name) << "from gphoto";
        return false;
    }

    // Unique pointer will free memory on exit
    auto optionPtr = CameraWidgetPtr(option, gp_widget_free);

    // Get option type
    CameraWidgetType type;
    ret = gp_widget_get_type(option, &type);
    if (ret < GP_OK) {
        qWarning() << "Unable to get option type from gphoto";
        return false;
    }

    if (type == GP_WIDGET_RADIO) {
        if (value.type() == QVariant::String) {
            // String, need no conversion
            ret = gp_widget_set_value(option, qPrintable(value.toString()));

            if (ret < GP_OK) {
                qWarning() << "Failed to set value" << value << "to" << name << "option:" << ret;
                return false;
            }

            ret = gp_camera_set_config(m_camera.get(), root, m_context);

            if (ret < GP_OK) {
                qWarning() << "Failed to set config to camera";
                return false;
            }

            waitForOperationCompleted();
            return true;
        }

        if (value.type() == QVariant::Double) {
            // Trying to find nearest possible value (with the distance of 0.1) and set it to property
            auto v = value.toDouble();

            auto count = gp_widget_count_choices(option);
            for (auto i = 0; i < count; ++i) {
                const char* choice;
                gp_widget_get_choice(option, i, &choice);

                // We use a workaround for flawed russian i18n of gphoto2 strings
                auto ok = false;
                auto choiceValue = QString::fromLocal8Bit(choice).replace(',', '.').toDouble(&ok);
                if (!ok) {
                    qDebug() << "Failed to convert value" << choice << "to double";
                    continue;
                }

                if (qAbs(choiceValue - v) < 0.1) {
                    ret = gp_widget_set_value(option, choice);
                    if (ret < GP_OK) {
                        qWarning() << "Failed to set value" << choice << "to" << name << "option:" << ret;
                        return false;
                    }

                    ret = gp_camera_set_config(m_camera.get(), root, m_context);
                    if (ret < GP_OK) {
                        qWarning() << "Failed to set config to camera";
                        return false;
                    }

                    waitForOperationCompleted();
                    return true;
                }
            }

            qWarning() << "Can't find value matching to" << v << "for option" << name;
            return false;
        }

        if (value.type() == QVariant::Int) {
            // Little hacks for 'ISO' option: if the value is -1, we pick the first non-integer value
            // we found and set it as a parameter
            auto v = value.toInt();


            auto count = gp_widget_count_choices(option);
            for (auto i = 0; i < count; ++i) {
                const char* choice;
                gp_widget_get_choice(option, i, &choice);

                auto ok = false;
                auto choiceValue = QString::fromLocal8Bit(choice).toInt(&ok);

                if ((ok && choiceValue == v) || (!ok && v == -1)) {
                    ret = gp_widget_set_value(option, choice);
                    if (ret < GP_OK) {
                        qWarning() << "Failed to set value" << choice << "to" << name << "option:" << ret;
                        return false;
                    }

                    ret = gp_camera_set_config(m_camera.get(), root, m_context);
                    if (ret < GP_OK) {
                        qWarning() << "Failed to set config to camera";
                        return false;
                    }

                    waitForOperationCompleted();
                    return true;
                }
            }

            qWarning() << "Can't find value matching to" << v << "for option" << name;
            return false;
        }

        qWarning() << "Failed to set value" << value << "to" << name << "option. Type" << value.type()
                   << "is not supported";
        return false;
    }

    if (type == GP_WIDGET_TOGGLE) {
        auto v = 0;
        if (value.canConvert<int>()) {
            v = value.toInt();
        } else {
            qWarning() << "Failed to set value" << value << "to" << name << "option. Type" << value.type()
                       << "is not supported";
            return false;
        }

        ret = gp_widget_set_value(option, &v);
        if (ret < GP_OK) {
          qWarning() << "Failed to set value" << v << "to" << name << "option:" << ret;
          return false;
        }

        ret = gp_camera_set_config(m_camera.get(), root, m_context);
        if (ret < GP_OK) {
          qWarning() << "Failed to set config to camera";
          return false;
        }

        waitForOperationCompleted();
        return true;
    }

    qWarning() << "Options of type" << type << "are currently not supported";
    return false;
}

void GPhotoCamera::capturePreview()
{
    if (m_status != QCamera::ActiveStatus)
        return;

    gp_file_clean(m_file.get());

    auto ret = gp_camera_capture_preview(m_camera.get(), m_file.get(), m_context);
    if (GP_OK == ret) {
        const char* data = nullptr;
        unsigned long int size = 0;
        ret = gp_file_get_data_and_size(m_file.get(), &data, &size);
        if (GP_OK == ret) {
            m_capturingFailCount = 0;
            if (!QThread::currentThread()->isInterruptionRequested()) {
                auto image = QImage::fromData(QByteArray(data, int(size)));
                emit previewCaptured(image);
            }
            return;
        }
    }

    qWarning() << "Failed retrieving preview" << ret;
    ++m_capturingFailCount;

    if (capturingFailLimit < m_capturingFailCount) {
        qWarning() << "Closing camera because of capturing fail";
        emit error(QCamera::CameraError, tr("Unable to capture frame"));
        closeCamera();
    }
}

void GPhotoCamera::openCamera()
{
    // Camera is already open
    if (m_camera)
        return;

    setStatus(QCamera::LoadingStatus);

    // Create camera object
    Camera *camera;
    auto ret = gp_camera_new(&camera);
    if (ret != GP_OK) {
        openCameraErrorHandle("Unable to open camera");
        return;
    }

    auto cameraPtr = CameraPtr(camera, gp_camera_free);

    ret = gp_camera_set_abilities(camera, m_abilities);
    if (ret < GP_OK) {
        openCameraErrorHandle("Unable to set abilities for camera");
        return;
    }

    ret = gp_camera_set_port_info(camera, m_portInfo);
    if (ret < GP_OK) {
        openCameraErrorHandle("Unable to set port info for camera");
        return;
    }

    CameraFile *file;
    gp_file_new(&file);
    m_file.reset(file);
    m_camera = std::move(cameraPtr);
    m_capturingFailCount = 0;

    setStatus(QCamera::LoadedStatus);
}

void GPhotoCamera::closeCamera()
{
    // Camera is already closed
    if (!m_camera)
        return;

    if (QCamera::ActiveStatus == m_status)
        stopViewFinder();

    setStatus(QCamera::UnloadingStatus);

    gp_file_clean(m_file.get());
    m_file.reset();

    gp_camera_exit(m_camera.get(), m_context);
    m_camera.reset();

    setStatus(QCamera::UnloadedStatus);
}

void GPhotoCamera::startViewFinder()
{
    openCamera();

    if (QCamera::ActiveStatus == m_status)
        return;

    setStatus(QCamera::StartingStatus);
    setMirrorPosition(MirrorPosition::Up);
    setStatus(QCamera::ActiveStatus);

    capturePreview();
}

void GPhotoCamera::stopViewFinder()
{
    if (m_status == QCamera::LoadedStatus)
        return;

    setStatus(QCamera::StoppingStatus);
    setMirrorPosition(MirrorPosition::Down);
    setStatus(QCamera::LoadedStatus);
}

void GPhotoCamera::setMirrorPosition(MirrorPosition pos)
{
    if (parameter(QLatin1Literal(viewfinderParameter)).isValid()) {
        auto up = (MirrorPosition::Up == pos);
        if (!setParameter(QLatin1Literal(viewfinderParameter), up))
            qWarning() << "Failed to flap" << (up ? "up" : "down") << "camera mirror";
    }
}

bool GPhotoCamera::isReadyForCapture() const
{
    if (m_captureMode & QCamera::CaptureStillImage)
        return (QCamera::ActiveStatus == m_status || QCamera::LoadedStatus == m_status);

    return false;
}

void GPhotoCamera::openCameraErrorHandle(const QString &errorText)
{
    qWarning() << qPrintable(errorText);
    setStatus(QCamera::UnavailableStatus);
    emit error(QCamera::CameraError, tr("Unable to open camera"));
}

void GPhotoCamera::logOption(const char *name)
{
    CameraWidget *root;
    auto ret = gp_camera_get_config(m_camera.get(), &root, m_context);
    if (ret < GP_OK) {
        qWarning() << "Unable to get root option from gphoto";
        return;
    }

    CameraWidget *option;
    ret = gp_widget_get_child_by_name(root, name, &option);
    if (ret < GP_OK)
        qWarning() << "Unable to get config widget from gphoto";

    // Unique pointer will free memory on exit
    auto optionPtr = CameraWidgetPtr(option, gp_widget_free);

    CameraWidgetType type;
    ret = gp_widget_get_type(option, &type);
    if (ret < GP_OK)
        qWarning() << "Unable to get config widget type from gphoto";

    char *value;
    ret = gp_widget_get_value(option, &value);
    if (ret < GP_OK)
        qWarning() << "Unable to get widget value from gphoto";

    qDebug() << "Option" << type << name << value;
    if (type == GP_WIDGET_RADIO) {
        auto count = gp_widget_count_choices(option);
        qDebug() << "Choices count:" << count;

        for (auto i = 0; i < count; ++i) {
            const char* choice;
            gp_widget_get_choice(option, i, &choice);
            qDebug() << "  value:" << choice;
        }
    }
}

void GPhotoCamera::waitForOperationCompleted()
{
    CameraEventType type;
    auto ret = GP_OK;
    do {
        void *data = nullptr;
        ret = gp_camera_wait_for_event(m_camera.get(), 10, &type, &data, m_context);
        free(data);
    } while ((ret == GP_OK) && (type != GP_EVENT_TIMEOUT) && m_camera);
}

void GPhotoCamera::setStatus(QCamera::Status status)
{
    if (m_status != status) {
        m_status = status;

        if (QCamera::LoadedStatus == m_status) {
            m_state = QCamera::LoadedState;
            emit stateChanged(m_state);
            emit readyForCaptureChanged(isReadyForCapture());
        } else if (QCamera::ActiveStatus == m_status) {
            m_state = QCamera::ActiveState;
            emit stateChanged(m_state);
            emit readyForCaptureChanged(isReadyForCapture());
        } else if (QCamera::UnloadedStatus == m_status || QCamera::UnavailableStatus == m_status) {
            m_state = QCamera::UnloadedState;
            emit stateChanged(m_state);
            emit readyForCaptureChanged(isReadyForCapture());
        }

        emit statusChanged(status);
    }
}