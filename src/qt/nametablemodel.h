#ifndef NAMETABLEMODEL_H
#define NAMETABLEMODEL_H

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>

#include <QAbstractTableModel>
#include <QDateTime>
#include <QStringList>

#include <memory>
#include <optional>
#include <sync.h>

namespace interfaces {
class Handler;
}

class PlatformStyle;
class NameTablePriv;
class CWallet;
class WalletModel;

enum class SynchronizationState;

/**
   Qt model for "Manage Names" page.
 */
class NameTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit NameTableModel(const PlatformStyle *platformStyle, WalletModel *parent=nullptr);
    virtual ~NameTableModel();

    enum ColumnIndex {
        Name = 0,
        Value = 1,
        ExpiresIn = 2,
        NameStatus = 3
    };

    enum RoleIndex {
        /** Binary data corresponding to name/value */
        BinaryRole = Qt::UserRole,
    };

    /** @name Methods overridden from QAbstractTableModel
        @{*/
    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    int columnCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const;
    Qt::ItemFlags flags(const QModelIndex &index) const;
    /*@}*/

    QString update(const QString &name, const std::optional<QString> &value, const std::optional<QString> &transferTo) const;
    QString renew(const QString &name) const;

private:
    WalletModel *walletModel;
    std::unique_ptr<interfaces::Handler> m_handler_transaction_changed;
    //std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    QStringList columns;
    std::unique_ptr<NameTablePriv> priv;
    const PlatformStyle *platformStyle;
    int cachedNumBlocks;
    RecursiveMutex cs_model;
    bool initDone;

    /** Notify listeners that data changed. */
    void emitDataChanged(int index);

public Q_SLOTS:
    void init();
    void updateExpiration(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType header, SynchronizationState sync_state);
    void processNewTransaction(const QModelIndex& parent, int start, int /*end*/);

    friend class NameTablePriv;
};

struct NameTableEntry
{
    QString name;
    QString value;
    int nHeight;
    int expiresIn;
    QString nameStatus;

    static const int NAME_NEW = -1;             // Dummy nHeight value for not-yet-created names
    static const int NAME_NON_EXISTING = -2;    // Dummy nHeight value for unitinialized entries
    static const int NAME_UNCONFIRMED = -3;     // Dummy nHeight value for unconfirmed name transactions

    static const std::string NAME_STATUS_CONFIRMED;
    static const std::string NAME_STATUS_EXPIRED;
    static const std::string NAME_STATUS_TRANSFERRED_OUT;
    static const std::string NAME_STATUS_REGISTRATION_PENDING;
    static const std::string NAME_STATUS_INCOMING_TRANSFER_PENDING;
    static const std::string NAME_STATUS_OUTGOING_TRANSFER_PENDING;
    static const std::string NAME_STATUS_RENEWAL_PENDING;
    static const std::string NAME_STATUS_UPDATE_PENDING;

    // NOTE: making this const throws warning indicating it will not be const
    bool HeightValid() { return nHeight >= 0; }
    static bool CompareHeight(int nOldHeight, int nNewHeight);    // Returns true if new height is better

    NameTableEntry() : nHeight(NAME_NON_EXISTING) {}
    NameTableEntry(const QString &name, const QString &value, int nHeight, int expiresIn, const QString &nameStatus):
        name(name), value(value), nHeight(nHeight), expiresIn(expiresIn), nameStatus(nameStatus) {}
    NameTableEntry(const std::string &name, const std::string &value, int nHeight, int expiresIn, const std::string &nameStatus):
        name(QString::fromStdString(name)), value(QString::fromStdString(value)), nHeight(nHeight), expiresIn(expiresIn), nameStatus(QString::fromStdString(nameStatus)) {}
};

#endif // NAMETABLEMODEL_H
