#include "mpdrunner.h"

#include <KLocalizedString>
#include <KRunner/QueryMatch>
#include <KRunner/RunnerContext>

#include <QDebug>

#include <mpd/client.h>

#include <algorithm>
#include <memory>

K_PLUGIN_CLASS_WITH_JSON(MpdRunner, "mpdrunner.json")

namespace
{
constexpr int MaxResults = 20;
constexpr int IndexCheckIntervalMs = 5000;
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
}

MpdRunner::MpdRunner(QObject *parent, const KPluginMetaData &metaData)
    : KRunner::AbstractRunner(parent, metaData)
{
    setMinLetterCount(3);
    addSyntax(QStringLiteral(":q:"), i18n("Finds tracks in the MPD library matching :q:"));
    addSyntax(QStringLiteral("mpd :q:"), i18n("Same, but ranks MPD results above other runners"));
    addSyntax(QStringLiteral("q :q:"), i18n("Queues the track after the current song without skipping to it"));
}

QString MpdRunner::fold(const QString &s)
{
    // Case- and accent-insensitive: "Beyoncé" matches "beyonce".
    QString out;
    const QString decomposed = s.normalized(QString::NormalizationForm_KD);
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing) {
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
            const QString foldedArtist = fold(artist);
            const QString foldedTitle = fold(value);
            tracks.append({artist, album, value, foldedArtist, foldedTitle, foldedArtist + QLatin1Char(' ') + fold(album) + QLatin1Char(' ') + foldedTitle});
        }
        mpd_return_pair(conn, pair);
    }
    return check(conn, "list") && mpd_response_finish(conn);
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
    refreshIndex();
    for (qsizetype i = 0; i < m_tracks.size(); ++i) {
        if ((i & 0xfff) == 0 && !context.isValid()) {
            return;
        }
        const Track &t = m_tracks.at(i);
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

    QList<KRunner::QueryMatch> matches;
    for (auto it = hits.begin(); it != cut; ++it) {
        const Track &t = m_tracks.at(it->index);
        KRunner::QueryMatch match(this);
        match.setCategoryRelevance(prefixed ? KRunner::QueryMatch::CategoryRelevance::High : KRunner::QueryMatch::CategoryRelevance::Low);
        match.setRelevance(std::min<qreal>(it->score, 1.0));
        match.setIconName(queueOnly ? QStringLiteral("media-playlist-append") : QStringLiteral("audio-x-generic"));
        match.setText(t.artist.isEmpty() ? t.title : i18nc("track title - artist", "%1 — %2", t.title, t.artist));
        match.setSubtext(t.album);
        match.setData(QVariantList{t.artist, t.album, t.title, queueOnly});
        match.setId(t.artist + QLatin1Char('\x1f') + t.album + QLatin1Char('\x1f') + t.title);
        matches.append(match);
    }
    lock.unlock();

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
    if (!mpd_search_db_songs(c, true) || !mpd_search_add_expression(c, expression.toUtf8().constData()) || !mpd_search_add_window(c, 0, 1)
        || !mpd_search_commit(c)) {
        check(c, "find");
        return;
    }
    QByteArray uri;
    if (mpd_song *song = mpd_recv_song(c)) {
        uri = mpd_song_get_uri(song);
        mpd_song_free(song);
    }
    if (!check(c, "find") || !mpd_response_finish(c)) {
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
