#include <qt/nametablemodel.h>

#include <interfaces/node.h>
#include <qt/clientmodel.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>
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

    /* Query entire name table anew from core.
     */
    void refreshNameTable(interfaces::Wallet& wallet)
    {
        LOCK(parent.cs_model);

        qDebug() << "NameTablePriv::refreshNameTable";
        std::map< std::string, NameTableEntry > vNamesO;

        // confirmed names (name_list)
        // TODO: Add unconfirmed names once support for this is added to
        // name_list.
        // TODO: Filter out expired=true and ismine=false once support for this
        // is added to name_list.
        // TODO: Set name and value encoding to hex, so that nonstandard
        // encodings don't cause errors.

        std::string walletURI = ("/wallet/" + parent.walletModel->getWalletName()).toStdString();

        UniValue confirmedNames;
        try {
            confirmedNames = parent.walletModel->node().executeRpc("name_list", NullUniValue, walletURI);
        } catch (const UniValue& e) {
            // although we shouldn't typically encounter error here, we
            // should continue and try to add confirmed names and
            // pending names. show error to user in case something
            // actually went wrong so they can potentially recover
            UniValue message = find_value( e, "message");
            LogPrintf ("name_list lookup error: %s\n", message.get_str());
        }

        // will be an object if name_list command isn't available/other error
        if(confirmedNames.isArray())
        {
            for (const auto& v : confirmedNames.getValues())
            {
                UniValue maybeName = find_value ( v, "name");
                UniValue maybeData = find_value ( v, "value");
                if (!maybeName.isStr() || !maybeData.isStr())
                {
                    continue;
                }

                std::string name = maybeName.get_str();
                std::string data = maybeData.get_str();
                int height = find_value ( v, "height").get_int();
                int expiresIn = find_value ( v, "expires_in").get_int();
                vNamesO[name] = NameTableEntry(name, data, height, expiresIn, "confirmed");
            }
        }

        // TODO: use beginInsertRows/nop/beginRemoveRows instead
        parent.beginResetModel();

        // TODO: edit existing cached table instead of clearing it
        cachedNameTable.clear();

        // Add existing names
        for (const auto& item : vNamesO)
            cachedNameTable.append(item.second);

        // TODO: use endInsertRows/dataChanged/endRemoveRows instead
        parent.endResetModel();
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
};

// TODO: figure out which of these members are actually still necessary for name_list
NameTableModel::NameTableModel(const PlatformStyle *platformStyle, WalletModel *parent):
        QAbstractTableModel(parent),
        walletModel(parent),
        priv(new NameTablePriv(*this)),
        platformStyle(platformStyle)
{
    columns << tr("Name") << tr("Value") << tr("Expires In") << tr("Status");
    priv->refreshNameTable(walletModel->wallet());

    connect(&walletModel->clientModel(), &ClientModel::numBlocksChanged, this, &NameTableModel::updateExpiration);

    connect(walletModel->getTransactionTableModel(), &TransactionTableModel::rowsInserted, this, &NameTableModel::processNewTransaction);
}

NameTableModel::~NameTableModel()
{
}

void NameTableModel::updateExpiration(int count, const QDateTime& blockDate, double nVerificationProgress, bool header, SynchronizationState sync_state)
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

QString NameTableModel::update(const QString &name, const Optional<QString> &value, const Optional<QString> &transferTo) const
{
    std::string strName = name.toStdString();
    LogPrintf ("wallet attempting name_update: name=%s\n", strName);

    UniValue params(UniValue::VOBJ);
    params.pushKV ("name", strName);

    if (value)
    {
        params.pushKV ("value", value.value().toStdString());
    }

    if (transferTo)
    {
        UniValue options(UniValue::VOBJ);
        options.pushKV ("destAddress", transferTo.value().toStdString());
        params.pushKV ("options", options);
    }

    std::string walletURI = ("/wallet/" + walletModel->getWalletName()).toStdString();

    UniValue res;
    try {
       res = walletModel->node().executeRpc("name_update", params, walletURI);
    }
    catch (const UniValue& e) {
        UniValue message = find_value(e, "message");
        std::string errorStr = message.get_str();
        LogPrintf ("name_update error: %s\n", errorStr);
        return QString::fromStdString(errorStr);
    }
    return tr ("");
}

QString NameTableModel::renew(const QString &name) const
{
    return update(name, {}, {});
}
