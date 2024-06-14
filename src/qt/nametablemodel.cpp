#include <qt/nametablemodel.h>

#include <interfaces/node.h>
#include <qt/clientmodel.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <names/encoding.h>
#include <univalue.h>
#include <wallet/wallet.h>

#include <QDebug>

// ExpiresIn column is right-aligned as it contains numbers
namespace {
    int column_alignments[] = {
        Qt::AlignLeft|Qt::AlignVCenter,     // Name
        Qt::AlignLeft|Qt::AlignVCenter,     // Value
        Qt::AlignRight|Qt::AlignVCenter,    // Expires in
        Qt::AlignRight|Qt::AlignVCenter,    // Name Status
    };
}

const std::string NameTableEntry::NAME_STATUS_CONFIRMED = "Confirmed";
const std::string NameTableEntry::NAME_STATUS_EXPIRED = "Expired";
const std::string NameTableEntry::NAME_STATUS_TRANSFERRED_OUT = "Transferred out";
const std::string NameTableEntry::NAME_STATUS_REGISTRATION_PENDING = "Registration pending";
const std::string NameTableEntry::NAME_STATUS_INCOMING_TRANSFER_PENDING = "Incoming transfer pending";
const std::string NameTableEntry::NAME_STATUS_OUTGOING_TRANSFER_PENDING = "Outgoing transfer pending";
const std::string NameTableEntry::NAME_STATUS_RENEWAL_PENDING = "Renewal pending";
const std::string NameTableEntry::NAME_STATUS_UPDATE_PENDING = "Update pending";

// Returns true if new height is better
bool NameTableEntry::CompareHeight(int nOldHeight, int nNewHeight)
{
    if(nOldHeight == NAME_NON_EXISTING)
        return true;

    // We use optimistic way, assuming that unconfirmed transaction will eventually become confirmed,
    // so we update the name in the table immediately. Ideally we need a separate way of displaying
    // unconfirmed names (e.g. grayed out)
    if(nNewHeight == NAME_UNCONFIRMED)
        return true;

    // Here we rely on the fact that dummy height values are always negative
    return nNewHeight > nOldHeight;
}

// Private implementation
class NameTablePriv
{
public:
    explicit NameTablePriv(NameTableModel& _parent) :
        parent(_parent)
    {
    }

    NameTableModel& parent;

    /* Local cache of name table.
     */
    QList<NameTableEntry> cachedNameTable;
    std::map<std::string, NameTableEntry> cachedNameMap;

    /* Query entire name table anew from core.
     */
    void refreshNameTable(interfaces::Wallet& wallet)
    {
        LOCK(parent.cs_model);

        qDebug() << "NameTablePriv::refreshNameTable";
        std::map<std::string, NameTableEntry> vNamesO;

        // confirmed names (name_list)

        UniValue params(UniValue::VOBJ);
        UniValue options(UniValue::VOBJ);
        options.pushKV ("nameEncoding", "hex");
        options.pushKV ("valueEncoding", "hex");
        params.pushKV ("options", options);

        const std::string walletURI = "/wallet/" + parent.walletModel->getWalletName().toStdString();

        UniValue confirmedNames;
        try {
            confirmedNames = parent.walletModel->node().executeRpc("name_list", params, walletURI);
        } catch (const UniValue& e) {
            // although we shouldn't typically encounter error here, we
            // should continue and try to add confirmed names and
            // pending names. show error to user in case something
            // actually went wrong so they can potentially recover
            const UniValue message = e.find_value( "message");
            LogPrint(BCLog::QT, "name_list lookup error: %s\n", message.get_str());
        }

        // will be an object if name_list command isn't available/other error
        if(confirmedNames.isArray())
        {
            for (const auto& v : confirmedNames.getValues())
            {
                UniValue maybeName = v.find_value ( "name");
                UniValue maybeData = v.find_value ( "value");
                if (!maybeName.isStr() || !maybeData.isStr())
                {
                    continue;
                }

                // TODO: Properly handle non-ASCII names/data.

                const std::string hexName = maybeName.get_str();
                std::string name;

                const std::string hexData = maybeData.get_str();
                std::string data;

                try
                {
                    name = NameTableModel::hexToAscii(QString::fromStdString(hexName)).toStdString();
                    data = NameTableModel::hexToAscii(QString::fromStdString(hexData)).toStdString();
                }
                catch (const InvalidNameString& exc)
                {
                    continue;
                }

                const int height = v.find_value ( "height").getInt<int>();
                const int expiresIn = v.find_value ( "expires_in").getInt<int>();

                const bool isMine = v.find_value ( "ismine").get_bool();
                const bool isExpired = v.find_value ( "expired").get_bool();
                // TODO: Check "op" field

                std::string status = NameTableEntry::NAME_STATUS_CONFIRMED;
                if (isExpired)
                {
                    status = NameTableEntry::NAME_STATUS_EXPIRED;
                }
                if (!isMine)
                {
                    status = NameTableEntry::NAME_STATUS_TRANSFERRED_OUT;
                }

                vNamesO[name] = NameTableEntry(name, data, height, expiresIn, status);
            }
        }

        // unconfirmed names (name_pending)

        UniValue pendingNames;
        try {
            pendingNames = parent.walletModel->node().executeRpc("name_pending", params, walletURI);
        } catch (const UniValue& e) {
            // although we shouldn't typically encounter error here, we
            // should continue and try to add confirmed names and
            // pending names. show error to user in case something
            // actually went wrong so they can potentially recover
            const UniValue message = e.find_value( "message");
            LogPrint(BCLog::QT, "name_pending lookup error: %s\n", message.get_str());
        }

        // will be an object if name_pending command isn't available/other error
        if(pendingNames.isArray())
        {
            for (const auto& v : pendingNames.getValues())
            {
                UniValue maybeName = v.find_value ( "name");
                UniValue maybeData = v.find_value ( "value");
                if (!maybeName.isStr() || !maybeData.isStr())
                {
                    continue;
                }

                // TODO: Properly handle non-ASCII names/data.

                const std::string hexName = maybeName.get_str();
                std::string name;

                const std::string hexData = maybeData.get_str();
                std::string data;

                try
                {
                    name = NameTableModel::hexToAscii(QString::fromStdString(hexName)).toStdString();
                    data = NameTableModel::hexToAscii(QString::fromStdString(hexData)).toStdString();
                }
                catch (const InvalidNameString& exc)
                {
                    continue;
                }

                const bool isMine = v.find_value ( "ismine").get_bool();
                const std::string op = v.find_value ( "op").get_str();

                const bool confirmedEntryExists = vNamesO.count(name);

                int height = 0;
                int expiresIn = 0;
                std::string status;

                if (confirmedEntryExists)
                {
                    // A confirmed entry exists.
                    auto confirmedEntry = vNamesO[name];
                    std::string confirmedData = confirmedEntry.value.toStdString();
                    std::string confirmedStatus = confirmedEntry.nameStatus.toStdString();
                    height = confirmedEntry.nHeight;
                    expiresIn = confirmedEntry.expiresIn;

                    if (confirmedStatus == NameTableEntry::NAME_STATUS_EXPIRED || confirmedStatus == NameTableEntry::NAME_STATUS_TRANSFERRED_OUT)
                    {
                        if (!isMine)
                        {
                            // This name isn't ours, it's just some random name
                            // that another user has pending.
                            continue;
                        }

                        // The name will be ours when it confirms, but it
                        // wasn't ours before.

                        if (op == "name_firstupdate")
                        {
                            // This is a registration, so the reason it wasn't
                            // ours before is that we hadn't registered it yet.
                            status = NameTableEntry::NAME_STATUS_REGISTRATION_PENDING;
                        }
                        else if (op == "name_update")
                        {
                            // This is an update, so we weren't the one to
                            // register it.  (If we were, there would be a
                            // confirmed entry.)  So this is an incoming
                            // transfer.
                            status = NameTableEntry::NAME_STATUS_INCOMING_TRANSFER_PENDING;
                        }
                    }
                    else if (confirmedStatus == NameTableEntry::NAME_STATUS_CONFIRMED)
                    {
                        if (!isMine)
                        {
                            // This name was ours, but won't be after this
                            // transaction confirms.  So this is an outgoing
                            // transfer.
                            status = NameTableEntry::NAME_STATUS_OUTGOING_TRANSFER_PENDING;
                        }
                        else
                        {
                            // This name was ours, and still will be.  So this
                            // is a pending update.

                            if (data == confirmedData)
                            {
                                // Data is unchanged, so this is a renewal.
                                status = NameTableEntry::NAME_STATUS_RENEWAL_PENDING;
                            }
                            else
                            {
                                // Data is changed.
                                status = NameTableEntry::NAME_STATUS_UPDATE_PENDING;
                            }
                        }
                    }
                }
                else
                {
                    // A confirmed entry doesn't exist.

                    if (!isMine)
                    {
                        // This name isn't ours, it's just some random name
                        // that another user has pending.
                        continue;
                    }

                    // The name will be ours when it confirms, but it wasn't
                    // ours before.

                    if (op == "name_firstupdate")
                    {
                        // This is a registration, so the reason it wasn't ours
                        // before is that we hadn't registered it yet.
                        status = NameTableEntry::NAME_STATUS_REGISTRATION_PENDING;
                    }
                    else if (op == "name_update")
                    {
                        // This is an update, so we weren't the one to register
                        // it.  (If we were, there would be a confirmed entry.)
                        // So this is an incoming transfer.
                        status = NameTableEntry::NAME_STATUS_INCOMING_TRANSFER_PENDING;
                    }
                }

                vNamesO[name] = NameTableEntry(name, data, height, expiresIn, status);
            }
        }

        for (const auto& item : vNamesO)
        {
            if (cachedNameMap.count(item.first))
            {
                // Name is already present, update its data.
                const int editIndex = indexOfName(item.first);

                cachedNameTable[editIndex] = item.second;

                parent.emitDataChanged(editIndex);
            }
            else
            {
                // Name is not already present, insert it.
                parent.beginInsertRows(QModelIndex(), cachedNameTable.size(), cachedNameTable.size());

                cachedNameTable.append(item.second);

                parent.endInsertRows();
            }
        }

        for (const auto& item : cachedNameMap)
        {
            if (!vNamesO.count(item.first))
            {
                // Name needs to be deleted.
                const int removeIndex = indexOfName(item.first);

                parent.beginRemoveRows(QModelIndex(), removeIndex, removeIndex);

                cachedNameTable.removeAt(removeIndex);

                parent.endRemoveRows();
            }
        }

        cachedNameMap = vNamesO;
    }

    int size()
    {
        return cachedNameTable.size();
    }

    NameTableEntry *index(int idx)
    {
        if(idx >= 0 && idx < cachedNameTable.size())
        {
            return &cachedNameTable[idx];
        }
        else
        {
            return nullptr;
        }
    }

    int indexOfName(std::string name)
    {
        if (cachedNameTable.isEmpty())
        {
            return -1;
        }

        for (int i = 0; i < cachedNameTable.size(); i++)
        {
            if (cachedNameTable.at(i).name.toStdString() == name)
            {
                return i;
            }
        }

        return -1;
    }
};

// TODO: figure out which of these members are actually still necessary for name_list
NameTableModel::NameTableModel(const PlatformStyle *platformStyle, WalletModel *parent):
        QAbstractTableModel(parent),
        walletModel(parent),
        priv(new NameTablePriv(*this)),
        platformStyle(platformStyle),
        initDone(false)
{
    columns << tr("Name") << tr("Value") << tr("Expires In") << tr("Status");

    // We can't init the name table from the constructor because executeRpc
    // locks context.wallets_mutex, which is already taken if we're creating a
    // new wallet.  So we use this stupid timer signal trick to delay the init.
    connect(walletModel, &WalletModel::timerTimeout, this, &NameTableModel::init);

    connect(&walletModel->clientModel(), &ClientModel::numBlocksChanged, this, &NameTableModel::updateExpiration);

    connect(walletModel->getTransactionTableModel(), &TransactionTableModel::rowsInserted, this, &NameTableModel::processNewTransaction);
}

NameTableModel::~NameTableModel()
{
}

void NameTableModel::init()
{
    if (initDone)
        return;

    priv->refreshNameTable(walletModel->wallet());

    initDone = true;
}

void NameTableModel::updateExpiration(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType header, SynchronizationState sync_state)
{
    // ClientModel already throttles this for us.

    priv->refreshNameTable(walletModel->wallet());
}

void NameTableModel::processNewTransaction(const QModelIndex& parent, int start, int /*end*/)
{
    // TransactionTableModel doesn't throttle this for us, so we have here a
    // copy of the throttling code from WalletView::processNewTransaction.

    // Prevent balloon-spam when initial block download is in progress
    if (!walletModel || walletModel->clientModel().node().isInitialBlockDownload())
        return;

    TransactionTableModel *ttm = walletModel->getTransactionTableModel();
    if (!ttm || ttm->processingQueuedTransactions())
        return;

    priv->refreshNameTable(walletModel->wallet());
}

int NameTableModel::rowCount(const QModelIndex &parent /*= QModelIndex()*/) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int NameTableModel::columnCount(const QModelIndex &parent /*= QModelIndex()*/) const
{
    Q_UNUSED(parent);
    return columns.length();
}

QVariant NameTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid())
        return QVariant();

    NameTableEntry *rec = static_cast<NameTableEntry*>(index.internalPointer());

    // TODO: implement Qt::ForegroudRole for font color styling for states?
    // TODO: implement Qt::ToolTipRole show name status on tooltip
    if(role == Qt::DisplayRole || role == Qt::EditRole)
    {
        switch(index.column())
        {
            case Name:
                return rec->name;
            case Value:
                return rec->value;
            case ExpiresIn:
                return rec->expiresIn;
            case NameStatus:
                return rec->nameStatus;
        }
    }

    if(role == Qt::TextAlignmentRole)
        return column_alignments[index.column()];

    return QVariant();
}

QVariant NameTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation != Qt::Horizontal)
        return QVariant();

    if(role == Qt::DisplayRole)
        return columns[section];

    if(role == Qt::TextAlignmentRole)
        return column_alignments[section];

    if(role == Qt::ToolTipRole)
    {
        switch(section)
        {
            case Name:
                return tr("Name registered using Namecoin.");

            case Value:
                return tr("Data associated with the name.");

            case ExpiresIn:
                return tr("Number of blocks, after which the name will expire. Update name to renew it.\nEmpty cell means pending(awaiting automatic name_firstupdate or awaiting network confirmation).");
        }
    }
    return QVariant();
}

Qt::ItemFlags NameTableModel::flags(const QModelIndex &index) const
{
    if(!index.isValid())
        return 0;

    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

QModelIndex NameTableModel::index(int row, int column, const QModelIndex &parent /* = QModelIndex()*/) const
{
    Q_UNUSED(parent);
    NameTableEntry *data = priv->index(row);
    if(data)
    {
        return createIndex(row, column, priv->index(row));
    }
    return QModelIndex();
}

void
NameTableModel::emitDataChanged(int idx)
{
    //emit
    dataChanged(index(idx, 0), index(idx, columns.length()-1));
}

QString NameTableModel::asciiToHex(const QString &ascii)
{
    const std::string strAscii = ascii.toStdString();
    const valtype vt = DecodeName (strAscii, NameEncoding::ASCII);

    const std::string strHex = EncodeName (vt, NameEncoding::HEX);
    const QString hex = QString::fromStdString(strHex);

    return hex;
}

QString NameTableModel::hexToAscii(const QString &hex)
{
    const std::string strHex = hex.toStdString();
    const valtype vt = DecodeName (strHex, NameEncoding::HEX);

    const std::string strAscii = EncodeName (vt, NameEncoding::ASCII);
    const QString ascii = QString::fromStdString(strAscii);

    return ascii;
}

QString NameTableModel::update(const QString &name, const std::optional<QString> &value, const std::optional<QString> &transferTo) const
{
    LogPrint(BCLog::QT, "wallet attempting name_update: name=%s\n", name.toStdString());

    UniValue params(UniValue::VOBJ);

    // TODO: Properly handle non-ASCII names/data.

    try
    {
        const QString hexName = NameTableModel::asciiToHex(name);
        params.pushKV ("name", hexName.toStdString());
    }
    catch (const InvalidNameString& exc)
    {
        return tr ("Name was invalid ASCII.");
    }

    UniValue options(UniValue::VOBJ);
    options.pushKV ("nameEncoding", "hex");

    if (value)
    {
        try
        {
            const QString hexValue = NameTableModel::asciiToHex(value.value());
            params.pushKV ("value", hexValue.toStdString());
        }
        catch (const InvalidNameString& exc)
        {
            return tr ("Value was invalid ASCII.");
        }

        options.pushKV ("valueEncoding", "hex");
    }

    if (transferTo)
    {
        options.pushKV ("destAddress", transferTo.value().toStdString());
    }

    params.pushKV ("options", options);

    const std::string walletURI = "/wallet/" + walletModel->getWalletName().toStdString();

    try {
        walletModel->node().executeRpc("name_update", params, walletURI);
    }
    catch (const UniValue& e) {
        const UniValue message = e.find_value("message");
        const std::string errorStr = message.get_str();
        LogPrint(BCLog::QT, "name_update error: %s\n", errorStr);
        return QString::fromStdString(errorStr);
    }
    return tr ("");
}

QString NameTableModel::renew(const QString &name) const
{
    return update(name, {}, {});
}
