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
#include <names/applications.h>
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

    ui->registerNameButton->hide();

    connect(ui->registerNameDomain, &QLineEdit::textEdited, this, &BuyNamesPage::onDomainNameEdited);
    connect(ui->registerNameAscii, &QLineEdit::textEdited, this, &BuyNamesPage::onAsciiNameEdited);
    connect(ui->registerNameHex, &QLineEdit::textEdited, this, &BuyNamesPage::onHexNameEdited);
    connect(ui->registerNameButton, &QPushButton::clicked, this, &BuyNamesPage::onRegisterNameAction);

    ui->registerNameDomain->installEventFilter(this);
    ui->registerNameAscii->installEventFilter(this);
    ui->registerNameHex->installEventFilter(this);
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
        if (object == ui->registerNameAscii)
        {
            ui->registerNameButton->setDefault(true);
        }

        if (object == ui->registerNameHex)
        {
            ui->registerNameButton->setDefault(true);
        }

        if (object == ui->registerNameDomain)
        {
            ui->registerNameButton->setDefault(true);
        }

    }
    return QWidget::eventFilter(object, event);
}


QString BuyNamesPage::DomainToASCII(const QString &name) {
    if(IsPurportedNamecoinDomain(name.toStdString()))
    {
        return QString::fromStdString(ASCIIFromDomain(name.toStdString()));
    }
    return QString("");
}


QString BuyNamesPage::ASCIIToDomain(const QString &name) {
    if(NamespaceFromName(name.toStdString()) == NameNamespace::Domain)
    {
        return QString::fromStdString(DescFromName(DecodeName(name.toStdString(), NameEncoding::ASCII), NameNamespace::Domain));
    }
    return QString("");
}


QString BuyNamesPage::HexToASCII(const QString &name) {
    return NameTableModel::hexToAscii(name);
}


QString BuyNamesPage::ASCIIToHex(const QString &name) {
    return NameTableModel::asciiToHex(name);
}

void BuyNamesPage::RefreshAvailableError()
{
    QString name = ui->registerNameAscii->text();
    QString availableError = name_available(name);

    if (availableError.isEmpty())
    {
        ui->statusLabel->setText(tr("%1 is available to register!").arg(name));
        ui->registerNameButton->show();
    }
    else
    {
        ui->statusLabel->setText(availableError);
        ui->registerNameButton->hide();
    }
}

void BuyNamesPage::onAsciiNameEdited(const QString &name) {
    if (!walletModel)
        return;

    try
    {
        const QString hexName = ASCIIToHex(name);
        const QString domainName = ASCIIToDomain(name);

        ui->registerNameHex->setText(hexName);
        ui->registerNameDomain->setText(domainName);

        RefreshAvailableError();
    }
    catch(InvalidNameString &e)
    {
        ui->statusLabel->setText(tr("Not a valid ASCII entry!"));
    }

}

void BuyNamesPage::onHexNameEdited(const QString &name) {
    if (!walletModel)
        return;

    try {
        const QString asciiName = HexToASCII(name);
        const QString domainName = ASCIIToDomain(asciiName);

        ui->registerNameAscii->setText(asciiName);
        ui->registerNameDomain->setText(domainName);

        RefreshAvailableError();
    }
    catch(InvalidNameString &e)
    {
        ui->statusLabel->setText(tr("Not a valid hex entry!"));
    }

}

void BuyNamesPage::onDomainNameEdited(const QString &name) {
    if (!walletModel)
        return;

    try
    {
        const QString asciiName = DomainToASCII(name);
        const QString hexName = ASCIIToHex(asciiName);

        ui->registerNameAscii->setText(asciiName);
        ui->registerNameHex->setText(hexName);

        if(IsPurportedNamecoinDomain(name.toStdString()))
        {
            RefreshAvailableError();
        }
        else
        {
            ui->statusLabel->setText(tr("\"%1\" requires .bit at the end to be a valid Namecoin domain!").arg(name));
            ui->registerNameButton->hide();
        }
    }
    catch(InvalidNameString &e)
    {
        ui->statusLabel->setText(tr("Not a valid entry!"));
    }


}

void BuyNamesPage::onRegisterNameAction()
{
    if (!walletModel)
        return;

    QString name = ui->registerNameAscii->text();

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

    ui->registerNameDomain->setText("");
    ui->registerNameAscii->setText("d/");
    ui->registerNameHex->setText(HexToASCII(QString("d/")));
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
