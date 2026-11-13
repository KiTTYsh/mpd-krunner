#include "mpdrunner.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KRunner/QueryMatch>
#include <KRunner/RunnerContext>

#include <QBuffer>
#include <QDebug>
#include <QGuiApplication>
#include <QIcon>
#include <QIconEngine>
#include <QImageReader>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>

#include <mpd/client.h>

#include <algorithm>
#include <memory>

K_PLUGIN_CLASS_WITH_JSON(MpdRunner, "mpdrunner.json")

namespace
{
constexpr int MaxResults = 20;
constexpr int IndexCheckIntervalMs = 5000;
// Without the index, at most this many songs are fetched per query and ranked.
constexpr unsigned SearchLimit = 500;
constexpr int ArtSize = 128;
constexpr int ArtChunkSize = 1 << 20;
constexpr qsizetype MaxArtBytes = 32 << 20;
constexpr qsizetype ArtCacheBytes = 32 << 20;
const QString UseIndexKey = QStringLiteral("cacheLibrary");
const QString Prefix = QStringLiteral("mpd ");
const QString QueuePrefix = QStringLiteral("q ");

struct ConnectionDeleter {
    void operator()(mpd_connection *c) const
    {
        mpd_connection_free(c);
    }
};
using Connection = std::unique_ptr<mpd_connection, ConnectionDeleter>;

// Honours MPD_HOST / MPD_PORT (including "password@host"), like mpc does.
Connection openConnection()
{
    Connection conn(mpd_connection_new(nullptr, 0, 3000));
    if (!conn) {
        qWarning() << "krunner_mpd: out of memory";
        return {};
    }
    if (mpd_connection_get_error(conn.get()) != MPD_ERROR_SUCCESS) {
        qWarning() << "krunner_mpd: cannot connect:" << mpd_connection_get_error_message(conn.get());
        return {};
    }
    return conn;
}

bool check(mpd_connection *conn, const char *what)
{
    if (mpd_connection_get_error(conn) == MPD_ERROR_SUCCESS) {
        return true;
    }
    qWarning() << "krunner_mpd:" << what << "failed:" << mpd_connection_get_error_message(conn);
    return false;
}

// Quotes a value for use inside an MPD filter expression.
QString quote(QString value)
{
    value.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    value.replace(QLatin1Char('\''), QLatin1String("\\'"));
    value.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

// Returns the URI of the first song matching the filter expression; empty if
// there is none or on error.
QByteArray findUri(mpd_connection *conn, const QString &expression)
{
    if (!mpd_search_db_songs(conn, true) || !mpd_search_add_expression(conn, expression.toUtf8().constData()) || !mpd_search_add_window(conn, 0, 1)
        || !mpd_search_commit(conn)) {
        check(conn, "find");
        return {};
    }
    QByteArray uri;
    if (mpd_song *song = mpd_recv_song(conn)) {
        uri = mpd_song_get_uri(song);
        mpd_song_free(song);
    }
    if (!check(conn, "find") || !mpd_response_finish(conn)) {
        return {};
    }
    return uri;
}

// Fetches an album's cover through "albumart", scaled down to ArtSize. Returns
// a null image if the album has none; sets *failed if the connection broke.
QImage fetchAlbumArt(mpd_connection *conn, const QString &artist, const QString &album, bool *failed)
{
    const QByteArray uri = findUri(conn, QStringLiteral("((artist == %1) AND (album == %2))").arg(quote(artist), quote(album)));
    if (uri.isEmpty()) {
        *failed = mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS;
        return {};
    }

    QByteArray data;
    QByteArray chunk(ArtChunkSize, Qt::Uninitialized);
    for (;;) {
        const int n = mpd_run_albumart(conn, uri.constData(), data.size(), chunk.data(), chunk.size());
        if (n < 0) {
            // A missing cover is a server error, which leaves the connection usable.
            *failed = !mpd_connection_clear_error(conn);
            return {};
        }
        if (n == 0) {
            break;
        }
        data.append(chunk.constData(), n);
        if (data.size() > MaxArtBytes) {
            return {};
        }
    }

    QBuffer buffer(&data);
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    if (size.width() > ArtSize || size.height() > ArtSize) {
        reader.setScaledSize(size.scaled(ArtSize, ArtSize, Qt::KeepAspectRatio));
    }
    return reader.read();
}

// Draws a cover image, optionally with a theme icon as an emblem in the corner.
// match() runs in a worker thread where QPixmap must not be used, so the
// pixmap is only made when the icon is painted (in the GUI thread).
class CoverIconEngine : public QIconEngine
{
public:
    CoverIconEngine(const QImage &image, const QString &emblem)
        : m_image(image)
        , m_emblem(emblem)
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode, QIcon::State) override
    {
        QRect target(QPoint(), m_image.size().scaled(rect.size(), Qt::KeepAspectRatio));
        target.moveCenter(rect.center());
        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(target, m_image);
        painter->restore();
        if (!m_emblem.isEmpty()) {
            // On a backdrop in the theme's window colour, so it stands out from the cover.
            const int side = std::min(rect.width(), rect.height()) / 2;
            const QRect corner(rect.right() - side + 1, rect.bottom() - side + 1, side, side);
            QColor backdrop = QGuiApplication::palette().color(QPalette::Window);
            backdrop.setAlpha(220);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(backdrop);
            painter->drawRoundedRect(corner, side / 5.0, side / 5.0);
            painter->restore();
            QIcon::fromTheme(m_emblem).paint(painter, corner.adjusted(side / 8, side / 8, -side / 8, -side / 8));
        }
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QImage image(size * scale, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            paint(&painter, image.rect(), mode, state);
        }
        image.setDevicePixelRatio(scale);
        return QPixmap::fromImage(image);
    }

    QIconEngine *clone() const override
    {
        return new CoverIconEngine(m_image, m_emblem);
    }

private:
    QImage m_image;
    QString m_emblem;
};
}

MpdRunner::MpdRunner(QObject *parent, const KPluginMetaData &metaData)
    : KRunner::AbstractRunner(parent, metaData)
{
    setMinLetterCount(3);
    addSyntax(QStringLiteral(":q:"), i18n("Finds tracks in the MPD library matching :q:"));
    addSyntax(QStringLiteral("mpd :q:"), i18n("Same, but ranks MPD results above other runners"));
    addSyntax(QStringLiteral("q :q:"), i18n("Queues the track after the current song without skipping to it"));
    m_art.setMaxCost(ArtCacheBytes);
}

void MpdRunner::reloadConfiguration()
{
    QMutexLocker lock(&m_mutex);
    m_useIndex = config().readEntry(UseIndexKey, true);
    if (!m_useIndex) {
        m_tracks = {};
        m_dbUpdate = 0;
    }
}

QString MpdRunner::fold(const QString &s)
{
    // Case- and accent-insensitive: "Beyoncé" matches "beyonce". Typographic
    // apostrophes become ASCII ones: "why's" matches "Why’s".
    QString out;
    const QString decomposed = s.normalized(QString::NormalizationForm_KD);
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c == u'\u2018' || c == u'\u2019' || c == u'\u02BC') {
            out.append(QLatin1Char('\''));
        } else if (c.category() != QChar::Mark_NonSpacing) {
            out.append(c);
        }
    }
    return out.toCaseFolded();
}

bool MpdRunner::loadIndex(mpd_connection *conn, QList<Track> &tracks)
{
    // "list title group album group artist" is far cheaper than fetching every
    // song's full metadata; the file URI is looked up on demand in run().
    if (!mpd_search_db_tags(conn, MPD_TAG_TITLE) || !mpd_search_add_group_tag(conn, MPD_TAG_ALBUM)
        || !mpd_search_add_group_tag(conn, MPD_TAG_ARTIST) || !mpd_search_commit(conn)) {
        return check(conn, "list");
    }

    const char *artistName = mpd_tag_name(MPD_TAG_ARTIST);
    const char *albumName = mpd_tag_name(MPD_TAG_ALBUM);
    const char *titleName = mpd_tag_name(MPD_TAG_TITLE);
    QString artist;
    QString album;
    while (mpd_pair *pair = mpd_recv_pair(conn)) {
        const QString value = QString::fromUtf8(pair->value);
        if (qstrcmp(pair->name, artistName) == 0) {
            artist = value;
        } else if (qstrcmp(pair->name, albumName) == 0) {
            album = value;
        } else if (qstrcmp(pair->name, titleName) == 0 && !value.isEmpty()) {
            tracks.append(makeTrack(artist, album, value));
        }
        mpd_return_pair(conn, pair);
    }
    return check(conn, "list") && mpd_response_finish(conn);
}

bool MpdRunner::searchTracks(mpd_connection *conn, const QStringList &words, QList<Track> &tracks)
{
    // "any" also matches other tags and the URI; match() filters those out again.
    QStringList terms;
    for (const QString &w : words) {
        terms.append(QStringLiteral("(any contains %1)").arg(quote(w)));
    }
    const QString expression = terms.size() == 1 ? terms.first() : QLatin1Char('(') + terms.join(QLatin1String(" AND ")) + QLatin1Char(')');
    if (!mpd_search_db_songs(conn, false) || !mpd_search_add_expression(conn, expression.toUtf8().constData())
        || !mpd_search_add_window(conn, 0, SearchLimit) || !mpd_search_commit(conn)) {
        return check(conn, "search");
    }

    QSet<QString> seen;
    while (mpd_song *song = mpd_recv_song(conn)) {
        const auto tag = [song](mpd_tag_type type) {
            return QString::fromUtf8(mpd_song_get_tag(song, type, 0));
        };
        const QString title = tag(MPD_TAG_TITLE);
        const QString artist = tag(MPD_TAG_ARTIST);
        const QString album = tag(MPD_TAG_ALBUM);
        mpd_song_free(song);
        // Like the index, which is grouped by artist/album/title.
        const QString id = artist + QLatin1Char('\x1f') + album + QLatin1Char('\x1f') + title;
        if (!title.isEmpty() && !seen.contains(id)) {
            seen.insert(id);
            tracks.append(makeTrack(artist, album, title));
        }
    }
    return check(conn, "search") && mpd_response_finish(conn);
}

MpdRunner::Track MpdRunner::makeTrack(const QString &artist, const QString &album, const QString &title)
{
    const QString foldedArtist = fold(artist);
    const QString foldedTitle = fold(title);
    return {artist, album, title, foldedArtist, foldedTitle, foldedArtist + QLatin1Char(' ') + fold(album) + QLatin1Char(' ') + foldedTitle};
}

void MpdRunner::refreshIndex()
{
    if (!m_tracks.isEmpty() && !m_nextCheck.hasExpired()) {
        return;
    }
    m_nextCheck.setRemainingTime(IndexCheckIntervalMs);

    Connection conn = openConnection();
    if (!conn) {
        return;
    }
    mpd_stats *stats = mpd_run_stats(conn.get());
    if (!stats) {
        check(conn.get(), "stats");
        return;
    }
    const unsigned long dbUpdate = mpd_stats_get_db_update_time(stats);
    mpd_stats_free(stats);
    if (!m_tracks.isEmpty() && dbUpdate == m_dbUpdate) {
        return;
    }

    QList<Track> tracks;
    tracks.reserve(m_tracks.size());
    if (loadIndex(conn.get(), tracks)) {
        m_tracks = std::move(tracks);
        m_dbUpdate = dbUpdate;
    }
}

qreal MpdRunner::score(const Track &t, const QStringList &words)
{
    // Whatever isn't the artist is probably (part of) the title: "beatles yesterday"
    // should rank the song "Yesterday" above other tracks on "Yesterday and Today".
    QStringList titleWords;
    for (const QString &w : words) {
        if (!t.foldedArtist.contains(w)) {
            titleWords.append(w);
        }
    }
    const QString titleQuery = titleWords.join(QLatin1Char(' '));

    qreal score = 0.5;
    if (titleQuery.isEmpty()) {
        score += 0.1;
    } else if (t.foldedTitle == titleQuery) {
        score += 0.3;
    } else if (t.foldedTitle.startsWith(titleQuery)) {
        score += 0.2;
    } else if (t.foldedTitle.contains(titleQuery)) {
        score += 0.1;
    }

    // Prefer words matched at the start of a word: "halo" in "Halo" over "FitzcarrHalo".
    for (const QString &w : words) {
        for (qsizetype pos = t.haystack.indexOf(w); pos >= 0; pos = t.haystack.indexOf(w, pos + 1)) {
            if (pos == 0 || !t.haystack.at(pos - 1).isLetterOrNumber()) {
                score += 0.15 / words.size();
                break;
            }
        }
    }
    return score;
}

void MpdRunner::match(KRunner::RunnerContext &context)
{
    QString query = context.query().trimmed();
    const bool queueOnly = query.startsWith(QueuePrefix, Qt::CaseInsensitive);
    const bool prefixed = queueOnly || query.startsWith(Prefix, Qt::CaseInsensitive);
    if (prefixed) {
        query = query.mid(queueOnly ? QueuePrefix.size() : Prefix.size()).trimmed();
    }
    // mpd's own search is only case-insensitive, so it gets the words unfolded:
    // "beyoncé" finds "Beyoncé" either way, "beyonce" reliably only with the index.
    // Apostrophes split words, as tags may use a different one: "why's" searches
    // for "why" and "s", and the folded words below still require the apostrophe.
    static const QRegularExpression searchSeparators(QStringLiteral("[ '\u2018\u2019\u02BC]+"));
    const QStringList searchWords = query.toCaseFolded().split(searchSeparators, Qt::SkipEmptyParts);
    query = fold(query);
    const QStringList words = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty() || query.size() < 3) {
        return;
    }

    struct Hit {
        qsizetype index;
        qreal score;
    };
    QList<Hit> hits;

    QMutexLocker lock(&m_mutex);
    const bool useIndex = m_useIndex;
    QList<Track> found;
    if (useIndex) {
        refreshIndex();
    } else {
        lock.unlock();
        if (searchWords.isEmpty()) {
            return;
        }
        Connection conn = openConnection();
        if (!conn || !searchTracks(conn.get(), searchWords, found)) {
            return;
        }
    }
    const QList<Track> &tracks = useIndex ? m_tracks : found;
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        if ((i & 0xfff) == 0 && !context.isValid()) {
            return;
        }
        const Track &t = tracks.at(i);
        if (!std::all_of(words.cbegin(), words.cend(), [&](const QString &w) {
                return t.haystack.contains(w);
            })) {
            continue;
        }
        hits.append({i, score(t, words)});
    }

    const auto cut = hits.begin() + std::min<qsizetype>(hits.size(), MaxResults);
    std::partial_sort(hits.begin(), cut, hits.end(), [](const Hit &a, const Hit &b) {
        return a.score > b.score;
    });

    QList<std::pair<Track, qreal>> results;
    for (auto it = hits.begin(); it != cut; ++it) {
        results.append({tracks.at(it->index), it->score});
    }
    if (useIndex) {
        lock.unlock();
    }

    QMutexLocker artLock(&m_artMutex);
    Connection artConn;
    bool artFailed = false;
    QList<KRunner::QueryMatch> matches;
    for (const auto &[t, score] : results) {
        if (!context.isValid()) {
            return;
        }
        QImage cover;
        const QString artKey = t.artist + QLatin1Char('\x1f') + t.album;
        if (t.album.isEmpty()) {
            // No album to show the cover of.
        } else if (const QImage *cached = m_art.object(artKey)) {
            cover = *cached;
        } else if (!artFailed) {
            if (!artConn) {
                artConn = openConnection();
                // Fetch covers in one piece; older servers just keep sending 8 KiB chunks.
                if (artConn && !mpd_run_binarylimit(artConn.get(), ArtChunkSize)) {
                    mpd_connection_clear_error(artConn.get());
                }
            }
            artFailed = !artConn;
            if (artConn) {
                cover = fetchAlbumArt(artConn.get(), t.artist, t.album, &artFailed);
            }
            if (!artFailed) {
                m_art.insert(artKey, new QImage(cover), cover.sizeInBytes() + 1);
            }
        }

        KRunner::QueryMatch match(this);
        match.setCategoryRelevance(prefixed ? KRunner::QueryMatch::CategoryRelevance::High : KRunner::QueryMatch::CategoryRelevance::Low);
        match.setRelevance(std::min<qreal>(score, 1.0));
        const QString iconName = queueOnly ? QStringLiteral("media-playlist-append") : QStringLiteral("audio-x-generic");
        if (cover.isNull()) {
            match.setIconName(iconName);
        } else {
            match.setIcon(QIcon(new CoverIconEngine(cover, queueOnly ? iconName : QString())));
        }
        match.setText(t.artist.isEmpty() ? t.title : i18nc("track title - artist", "%1 — %2", t.title, t.artist));
        match.setSubtext(t.album);
        match.setData(QVariantList{t.artist, t.album, t.title, queueOnly});
        match.setId(t.artist + QLatin1Char('\x1f') + t.album + QLatin1Char('\x1f') + t.title);
        matches.append(match);
    }
    artLock.unlock();

    context.addMatches(matches);
}

void MpdRunner::run(const KRunner::RunnerContext &, const KRunner::QueryMatch &match)
{
    const QVariantList data = match.data().toList();
    if (data.size() != 4) {
        return;
    }
    const QStringList tags{data[0].toString(), data[1].toString(), data[2].toString()};
    const bool queueOnly = data[3].toBool();

    Connection conn = openConnection();
    if (!conn) {
        return;
    }
    mpd_connection *c = conn.get();

    const QString expression = QStringLiteral("((artist == %1) AND (album == %2) AND (title == %3))").arg(quote(tags[0]), quote(tags[1]), quote(tags[2]));
    const QByteArray uri = findUri(c, expression);
    if (mpd_connection_get_error(c) != MPD_ERROR_SUCCESS) {
        return;
    }
    if (uri.isEmpty()) {
        qWarning() << "krunner_mpd: track no longer in database:" << tags;
        return;
    }

    mpd_status *status = mpd_run_status(c);
    if (!status) {
        check(c, "status");
        return;
    }
    const bool hasCurrent = mpd_status_get_song_pos(status) >= 0;
    mpd_status_free(status);

    // Insert right after the current song (or append if nothing is queued as
    // current), then jump to it unless only queueing.
    const int id = hasCurrent ? mpd_run_add_id_whence(c, uri.constData(), 0, MPD_POSITION_AFTER_CURRENT) : mpd_run_add_id(c, uri.constData());
    if (id < 0) {
        check(c, "add");
        return;
    }
    if (!queueOnly && !mpd_run_play_id(c, id)) {
        check(c, "play");
    }
}

#include "mpdrunner.moc"
