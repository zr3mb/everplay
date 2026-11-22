#pragma once

#include <QObject>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// The user's own profile on this device: a name, a picture and a short bio, kept in settings.ini and the data
// folder. With an online account the name and the picture also go to the server (OnlineService::syncProfile),
// where the people in your rooms and shared playlists see them; the bio stays here.
class Profile : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY changed)
    Q_PROPERTY(QString bio READ bio WRITE setBio NOTIFY changed)
    Q_PROPERTY(QUrl picture READ picture NOTIFY changed)   // empty: no picture chosen
    Q_PROPERTY(int nameLimit READ nameLimit CONSTANT)
    Q_PROPERTY(int bioLimit READ bioLimit CONSTANT)

public:
    static Profile *instance();
    static Profile *create(QQmlEngine *, QJSEngine *);

    QString name() const { return m_name; }
    void setName(const QString &name);
    QString bio() const { return m_bio; }
    void setBio(const QString &bio);
    QUrl picture() const;
    int nameLimit() const { return 40; }
    int bioLimit() const { return 300; }

    // Copies the image into the data folder (a square, at most 512 px), so the original may move or go.
    // False when the file isn't an image Everplay can read.
    Q_INVOKABLE bool setPicture(const QUrl &file);
    Q_INVOKABLE void removePicture();

signals:
    void changed();

private:
    explicit Profile(QObject *parent = nullptr);

    QString m_name;
    QString m_bio;
    QString m_pictureFile;   // file name in the data folder
};
