#pragma once

#include <KRunner/AbstractRunner>

#include <QCache>
#include <QDeadlineTimer>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QString>

struct mpd_connection;

class MpdRunner : public KRunner::AbstractRunner
{
    Q_OBJECT

public:
    MpdRunner(QObject *parent, const KPluginMetaData &metaData);

    void match(KRunner::RunnerContext &context) override;
    void run(const KRunner::RunnerContext &context, const KRunner::QueryMatch &match) override;
    void reloadConfiguration() override;

private:
    struct Track {
        QString artist;
        QString album;
        QString title;
        QString foldedArtist;
        QString foldedTitle;
        QString haystack; // folded "artist album title", used for matching
    };

    // Reloads the track index if mpd's database changed. Caller must hold m_mutex.
    void refreshIndex();
    static bool loadIndex(mpd_connection *conn, QList<Track> &tracks);
    // Asks mpd directly for tracks matching every word, without the index.
    static bool searchTracks(mpd_connection *conn, const QStringList &words, QList<Track> &tracks);
    static Track makeTrack(const QString &artist, const QString &album, const QString &title);
    static QString fold(const QString &s);
    static qreal score(const Track &t, const QStringList &words);

    QMutex m_mutex;
    QList<Track> m_tracks;
    unsigned long m_dbUpdate = 0;
    QDeadlineTimer m_nextCheck;
    bool m_useIndex = true;

    QMutex m_artMutex;
    QCache<QString, QImage> m_art; // keyed by artist + '\x1f' + album; null image = no cover
};
