#include <qt/managenamespage.h>
#include <qt/forms/ui_managenamespage.h>

#include <qt/configurenamedialog.h>
#include <qt/csvmodelwriter.h>
#include <qt/guiutil.h>
#include <qt/nametablemodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <node/interface_ui.h>

#include <QMessageBox>
#include <QMenu>
#include <QSortFilterProxyModel>

#include <optional>

ManageNamesPage::ManageNamesPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    platformStyle(platformStyle),
    ui(new Ui::ManageNamesPage),
    model(nullptr),
    walletModel(nullptr),
    proxyModel(nullptr)
{
    ui->setupUi(this);

    // Context menu actions
    copyNameAction = new QAction(tr("Copy &Name"), this);
    copyValueAction = new QAction(tr("Copy &Value"), this);
    configureNameAction = new QAction(tr("&Configure Name..."), this);
    renewNameAction = new QAction(tr("&Renew Names"), this);

    // Build context menu
    contextMenu = new QMenu();
    contextMenu->addAction(copyNameAction);
    contextMenu->addAction(copyValueAction);
    contextMenu->addAction(configureNameAction);
    contextMenu->addAction(renewNameAction);

    // Connect signals for context menu actions
    connect(copyNameAction, &QAction::triggered, this, &ManageNamesPage::onCopyNameAction);
    connect(copyValueAction, &QAction::triggered, this, &ManageNamesPage::onCopyValueAction);
    connect(configureNameAction, &QAction::triggered, this, &ManageNamesPage::onConfigureNameAction);
    connect(renewNameAction, &QAction::triggered, this, &ManageNamesPage::onRenewNameAction);

    connect(ui->configureNameButton, &QPushButton::clicked, this, &ManageNamesPage::onConfigureNameAction);
    connect(ui->renewNameButton, &QPushButton::clicked, this, &ManageNamesPage::onRenewNameAction);

    connect(ui->tableView, &QTableView::doubleClicked, this, &ManageNamesPage::onConfigureNameAction);

    connect(ui->tableView, &QTableView::customContextMenuRequested, this, &ManageNamesPage::contextualMenu);
    ui->tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    ui->tableView->installEventFilter(this);

    connect(ui->exportButton, &QPushButton::clicked, this, &ManageNamesPage::onExport);
}

ManageNamesPage::~ManageNamesPage()
{
    delete ui;
}

void ManageNamesPage::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
    model = walletModel->getNameTableModel();

    proxyModel = new QSortFilterProxyModel(this);
    proxyModel->setSourceModel(model);
    proxyModel->setDynamicSortFilter(true);
    proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

    ui->tableView->setModel(proxyModel);
    ui->tableView->sortByColumn(0, Qt::AscendingOrder);

    ui->tableView->horizontalHeader()->setHighlightSections(false);

    // Set column widths
    ui->tableView->horizontalHeader()->resizeSection(
            NameTableModel::Name, 320);
    ui->tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    connect(ui->tableView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &ManageNamesPage::selectionChanged);

    connect(model, &QAbstractItemModel::rowsInserted,
            this, &ManageNamesPage::rowCountChanged);
    connect(model, &QAbstractItemModel::rowsRemoved,
            this, &ManageNamesPage::rowCountChanged);

    selectionChanged();
    rowCountChanged();
}

bool ManageNamesPage::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FocusIn)
    {
        if (object == ui->tableView)
        {
            ui->configureNameButton->setDefault(true);
        }
    }
    return QWidget::eventFilter(object, event);
}

void ManageNamesPage::selectionChanged()
{
    // Enable/disable UI elements based on number of names selected.
    QTableView *table = ui->tableView;
    if (!table->selectionModel())
        return;

    QModelIndexList indexes = GUIUtil::getEntryData(ui->tableView, NameTableModel::Name);

    bool anyUnspendableSelected = false;
    for (const QModelIndex& index : indexes)
    {
        const std::string &status = index.sibling(index.row(), NameTableModel::NameStatus).data(Qt::EditRole).toString().toStdString();

        if (status == NameTableEntry::NAME_STATUS_EXPIRED || status == NameTableEntry::NAME_STATUS_TRANSFERRED_OUT)
        {
            anyUnspendableSelected = true;
            break;
        }
    }

    const bool singleNameSelected = indexes.size() == 1;
    const bool anyNamesSelected = indexes.size() >= 1;

    // Context menu
    copyNameAction->setEnabled(singleNameSelected);
    copyValueAction->setEnabled(singleNameSelected);
    configureNameAction->setEnabled(singleNameSelected && !anyUnspendableSelected);
    renewNameAction->setEnabled(anyNamesSelected && !anyUnspendableSelected);

    // Buttons
    ui->configureNameButton->setEnabled(configureNameAction->isEnabled());
    ui->renewNameButton->setEnabled(renewNameAction->isEnabled());
}

void ManageNamesPage::rowCountChanged()
{
    if (!model)
    {
        return;
    }

    ui->countLabel->setText(tr("%1 names.").arg(model->rowCount()));
}

void ManageNamesPage::contextualMenu(const QPoint &point)
{
    QModelIndex index = ui->tableView->indexAt(point);
    if (index.isValid())
        contextMenu->exec(QCursor::pos());
}

void ManageNamesPage::onCopyNameAction()
{
    GUIUtil::copyEntryData(ui->tableView, NameTableModel::Name, NameTableModel::BinaryRole);
}

void ManageNamesPage::onCopyValueAction()
{
    GUIUtil::copyEntryData(ui->tableView, NameTableModel::Value, NameTableModel::BinaryRole);
}

void ManageNamesPage::onConfigureNameAction()
{
    QModelIndexList indexes = GUIUtil::getEntryData(ui->tableView, NameTableModel::Name);

    if (indexes.isEmpty())
        return;

    if (indexes.size() != 1)
        return;

    WalletModel::UnlockContext ctx(walletModel->requestUnlock ());
    if (!ctx.isValid ())
        return;

    const QModelIndex &index = indexes.at(0);
    const QString name = index.data(NameTableModel::BinaryRole).toString();
    const std::string strName = name.toStdString();
    const QString initValue = index.sibling(index.row(), NameTableModel::Value).data(NameTableModel::BinaryRole).toString();

    ConfigureNameDialog dlg(platformStyle, name, initValue, this);
    dlg.setModel(walletModel);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString &newValue = dlg.getReturnData();
    const std::optional<QString> transferToAddress = dlg.getTransferTo();

    const QString err_msg = model->update(name, newValue, transferToAddress);
    if (!err_msg.isEmpty() && err_msg != "ABORTED")
    {
        QMessageBox::critical(this, tr("Name update error"), err_msg);
        return;
    }
}

void ManageNamesPage::onRenewNameAction()
{
    QModelIndexList indexes = GUIUtil::getEntryData(ui->tableView, NameTableModel::Name);

    if (indexes.isEmpty())
        return;

    QString msg;
    QString title;

    if (indexes.size() == 1)
    {
        const QString name = indexes.at(0).data(Qt::EditRole).toString();

        msg = tr ("Are you sure you want to renew the <b>%1</b>?")
            .arg (GUIUtil::HtmlEscape (name));
        title = tr ("Confirm name renewal");
    }
    else
    {
        msg = tr ("Are you sure you want to renew multiple names simultaneously?  This will reveal common ownership of the renewed names (bad for anonymity).");
        title = tr ("Confirm multiple name renewal");
    }

    QMessageBox::StandardButton res;
    res = QMessageBox::question (this, title, msg,
                                 QMessageBox::Yes | QMessageBox::Cancel,
                                 QMessageBox::Cancel);
    if (res != QMessageBox::Yes)
        return;

    WalletModel::UnlockContext ctx(walletModel->requestUnlock ());
    if (!ctx.isValid ())
        return;

    for (const QModelIndex& index : indexes)
    {
        const QString name = index.data(NameTableModel::BinaryRole).toString();

        const QString err_msg = model->renew(name);
        if (!err_msg.isEmpty() && err_msg != "ABORTED")
        {
            QMessageBox::critical(this, tr("Name renew error"), err_msg);
            return;
        }
    }
}

void ManageNamesPage::onExport()
{
    if (!model) {
        return;
    }

    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(this,
        tr("Export Name Inventory"), QString(),
        /*: Expanded name of the CSV file format.
            See https://en.wikipedia.org/wiki/Comma-separated_values */
        tr("Comma separated file") + QLatin1String(" (*.csv)"), nullptr);

    if (filename.isNull())
        return;

    CSVModelWriter writer(filename);

    // name, column, role
    writer.setModel(proxyModel);
    writer.addColumn(tr("Name"), NameTableModel::Name, Qt::EditRole);
    writer.addColumn(tr("Value"), NameTableModel::Value, Qt::EditRole);
    writer.addColumn(tr("Expires In"), NameTableModel::ExpiresIn, Qt::EditRole);
    writer.addColumn(tr("Name Status"), NameTableModel::NameStatus, Qt::EditRole);

    if(!writer.write()) {
        Q_EMIT message(tr("Exporting Failed"), tr("There was an error trying to save the name inventory to %1.").arg(filename),
            CClientUIInterface::MSG_ERROR);
    }
    else {
        Q_EMIT message(tr("Exporting Successful"), tr("The name inventory was successfully saved to %1.").arg(filename),
            CClientUIInterface::MSG_INFORMATION);
    }
}
