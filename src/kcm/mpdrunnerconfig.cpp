#include <KConfigGroup>
#include <KCModule>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>

#include <QCheckBox>
#include <QLabel>
#include <QVBoxLayout>

namespace
{
// Must match the runner's plugin id and key (see mpdrunner.cpp).
const QString RunnerId = QStringLiteral("krunner_mpd");
const QString UseIndexKey = QStringLiteral("cacheLibrary");
}

class MpdRunnerConfig : public KCModule
{
    Q_OBJECT

public:
    MpdRunnerConfig(QObject *parent, const KPluginMetaData &data)
        : KCModule(parent, data)
        , m_useIndex(new QCheckBox(i18n("Cache the library in memory"), widget()))
    {
        auto description = new QLabel(i18n("Matches against an in-memory copy of the library's artists, albums and titles, "
                                           "which is accent-insensitive and usually much faster. It is reloaded when MPD's "
                                           "database changes. When disabled, every query is sent to MPD as a search."),
                                      widget());
        description->setWordWrap(true);

        auto layout = new QVBoxLayout(widget());
        layout->addWidget(m_useIndex);
        layout->addWidget(description);
        layout->addStretch();

        connect(m_useIndex, &QCheckBox::toggled, this, [this] {
            setNeedsSave(m_useIndex->isChecked() != config().readEntry(UseIndexKey, true));
            setRepresentsDefaults(m_useIndex->isChecked());
        });
    }

    void load() override
    {
        m_useIndex->setChecked(config().readEntry(UseIndexKey, true));
        KCModule::load();
    }

    void save() override
    {
        // Notify makes the runner manager call reloadConfiguration().
        config().writeEntry(UseIndexKey, m_useIndex->isChecked(), KConfig::Notify);
        config().sync();
        KCModule::save();
    }

    void defaults() override
    {
        m_useIndex->setChecked(true);
        KCModule::defaults();
    }

private:
    static KConfigGroup config()
    {
        return KSharedConfig::openConfig(QStringLiteral("krunnerrc"))->group(QStringLiteral("Runners")).group(RunnerId);
    }

    QCheckBox *const m_useIndex;
};

K_PLUGIN_CLASS(MpdRunnerConfig)

#include "mpdrunnerconfig.moc"
