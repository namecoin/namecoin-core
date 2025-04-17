#include <qt/buynamespage.h>
#include <qt/forms/ui_buynamespage.h>

#include <interfaces/node.h>
#include <logging.h>
#include <qt/configurenamedialog.h>
#include <qt/guiutil.h>
#include <qt/nametablemodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <rpc/protocol.h>

#include <names/encoding.h>
#include <univalue.h>

#include <QMessageBox>

BuyNamesPage::BuyNamesPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    platformStyle(platformStyle),
    ui(new Ui::BuyNamesPage),
    walletModel(nullptr)
{
    ui->setupUi(this);

    ui->preRegisterNameButton->hide();
    ui->registerNameButton->hide();

    connect(ui->registerName, &QLineEdit::textEdited, this, &BuyNamesPage::onNameEdited);
    connect(ui->preRegisterNameButton, &QPushButton::clicked, this, &BuyNamesPage::onPreRegisterNameAction);
    connect(ui->registerNameButton, &QPushButton::clicked, this, &BuyNamesPage::onRegisterNameAction);

    ui->registerName->installEventFilter(this);
}

BuyNamesPage::~BuyNamesPage()
{
    delete ui;
}

void BuyNamesPage::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
}

bool BuyNamesPage::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FocusIn)
    {
        if (object == ui->registerName)
        {
            ui->registerNameButton->setDefault(true);
        }
    }
    return QWidget::eventFilter(object, event);
}

void BuyNamesPage::onNameEdited(const QString &name)
{
    if (!walletModel)
        return;

    const QString availableError = name_available(name);

    if (availableError == "")
    {
        ui->statusLabel->setText(tr("%1 is available to register!").arg(name));
        ui->preRegisterNameButton->show();
        ui->registerNameButton->show();
    }
    else
    {
        ui->statusLabel->setText(availableError);
        ui->preRegisterNameButton->hide();
        ui->registerNameButton->hide();
    }
}

void BuyNamesPage::onPreRegisterNameAction()
{
    if (!walletModel)
        return;

    QString name = ui->registerName->text();

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    const QString err_msg = this->name_new(name);
    if (!err_msg.isEmpty() && err_msg != "ABORTED")
    {
        QMessageBox::critical(this, tr("Name pre-registration error"), err_msg);
        return;
    }

    // reset UI text
    ui->registerName->setText("d/");
    ui->registerNameButton->setDefault(true);
}

void BuyNamesPage::onRegisterNameAction()
{
    if (!walletModel)
        return;

    QString name = ui->registerName->text();

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    ConfigureNameDialog dlg(platformStyle, name, "", this);
    dlg.setModel(walletModel);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString &newValue = dlg.getReturnData();
    const std::optional<QString> transferToAddress = dlg.getTransferTo();

    const QString err_msg = this->firstupdate(name, newValue, transferToAddress);
    if (!err_msg.isEmpty() && err_msg != "ABORTED")
    {
        QMessageBox::critical(this, tr("Name registration error"), err_msg);
        return;
    }

    // reset UI text
    ui->registerName->setText("d/");
    ui->registerNameButton->setDefault(true);
}

// Returns empty string if available, otherwise a description of why it is not
// available.
QString BuyNamesPage::name_available(const QString &name) const
{
    const std::string strName = name.toStdString();
    LogDebug(BCLog::QT, "wallet attempting name_show: name=%s\n", strName);

    UniValue params(UniValue::VOBJ);

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
    params.pushKV ("options", options);

    const std::string walletURI = "/wallet/" + walletModel->getWalletName().toStdString();

    try
    {
        walletModel->node().executeRpc("name_show", params, walletURI);
    }
    catch (const UniValue& e)
    {
        const UniValue code = e.find_value("code");
        const int codeInt = code.getInt<int>();
        if (codeInt == RPC_WALLET_ERROR)
        {
            // Name doesn't exist, so it's available.
            return QString("");
        }

        const UniValue message = e.find_value("message");
        const std::string errorStr = message.get_str();
        LogDebug(BCLog::QT, "name_show error: %s\n", errorStr);
        return QString::fromStdString(errorStr);
    }

    return tr("%1 is already registered, sorry!").arg(name);
}

QString BuyNamesPage::name_new(const QString &name) const
{
    const std::string strName = name.toStdString();
    LogPrint(BCLog::QT, "wallet attempting name_new: name=%s\n", strName);

    UniValue params(UniValue::VOBJ);
    params.pushKV ("name", strName);

    const std::string walletURI = "/wallet/" + walletModel->getWalletName().toStdString();

    try {
        walletModel->node().executeRpc("name_new", params, walletURI);
    }
    catch (const UniValue& e) {
        const UniValue message = find_value(e, "message");
        const std::string errorStr = message.get_str();
        LogPrint(BCLog::QT, "name_new error: %s\n", errorStr);
        return QString::fromStdString(errorStr);
    }
    return tr ("");
}

QString BuyNamesPage::firstupdate(const QString &name, const std::optional<QString> &value, const std::optional<QString> &transferTo) const
{
    const std::string strName = name.toStdString();
    LogDebug(BCLog::QT, "wallet attempting name_firstupdate: name=%s\n", strName);

    UniValue params(UniValue::VOBJ);

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
        walletModel->node().executeRpc("name_firstupdate", params, walletURI);
    }
    catch (const UniValue& e) {
        const UniValue message = e.find_value("message");
        const std::string errorStr = message.get_str();
        LogDebug(BCLog::QT, "name_firstupdate error: %s\n", errorStr);
        return QString::fromStdString(errorStr);
    }
    return tr ("");
}
