#include "Profile.h"

#include "AppPaths.h"

#include <QDateTime>
#include <QImageReader>
#include <QJSEngine>

namespace {

constexpr int kPictureSize = 512;

QString simplified(const QString &text, int limit, bool singleLine)
{
    QString t = singleLine ? text.simplified() : text.trimmed();
    if (!singleLine) {
        // Keep the bio's line breaks, but no more than one empty line in a row.
        t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        while (t.contains(QStringLiteral("\n\n\n")))
            t.replace(QStringLiteral("\n\n\n"), QStringLiteral("\n\n"));
    }
    return t.left(limit);
}

} // namespace

Profile *Profile::instance()
{
    static Profile *inst = new Profile;
    return inst;
}

Profile *Profile::create(QQmlEngine *, QJSEngine *)
{
    Profile *p = instance();
    QJSEngine::setObjectOwnership(p, QJSEngine::CppOwnership);
    return p;
}

Profile::Profile(QObject *parent)
    : QObject(parent)
{
    auto settings = AppPaths::settings();
    m_name = settings->value("profile/name").toString();
    m_bio = settings->value("profile/bio").toString();
    m_pictureFile = settings->value("profile/picture").toString();
    if (!m_pictureFile.isEmpty() && !QFileInfo::exists(AppPaths::dataDir() + QLatin1Char('/') + m_pictureFile))
        m_pictureFile.clear();
}

void Profile::setName(const QString &name)
{
    const QString value = simplified(name, nameLimit(), true);
    if (value == m_name)
        return;
    m_name = value;
    AppPaths::settings()->setValue("profile/name", m_name);
    emit changed();
}

void Profile::setBio(const QString &bio)
{
    const QString value = simplified(bio, bioLimit(), false);
    if (value == m_bio)
        return;
    m_bio = value;
    AppPaths::settings()->setValue("profile/bio", m_bio);
    emit changed();
}

QUrl Profile::picture() const
{
    if (m_pictureFile.isEmpty())
        return {};
    return QUrl::fromLocalFile(AppPaths::dataDir() + QLatin1Char('/') + m_pictureFile);
}

bool Profile::setPicture(const QUrl &file)
{
    QImageReader reader(file.isLocalFile() ? file.toLocalFile() : file.toString());
    reader.setAutoTransform(true);   // photos taken sideways keep their orientation
    QImage image = reader.read();
    if (image.isNull())
        return false;

    // The middle square, scaled down: the picture is shown in a circle.
    const int side = qMin(image.width(), image.height());
    image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    if (side > kPictureSize)
        image = image.scaled(kPictureSize, kPictureSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // A new name each time, so the interface doesn't show the previous picture from its image cache.
    const QString name = QStringLiteral("profile-%1.png").arg(QDateTime::currentMSecsSinceEpoch());
    if (!image.save(AppPaths::dataDir() + QLatin1Char('/') + name, "PNG"))
        return false;
    removePicture();
    m_pictureFile = name;
    AppPaths::settings()->setValue("profile/picture", m_pictureFile);
    emit changed();
    return true;
}

void Profile::removePicture()
{
    if (m_pictureFile.isEmpty())
        return;
    QFile::remove(AppPaths::dataDir() + QLatin1Char('/') + m_pictureFile);
    m_pictureFile.clear();
    AppPaths::settings()->remove("profile/picture");
    emit changed();
}
