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

#include <string>
#include <algorithm>

#include <QMessageBox>

//ascii -> old domain system
//domain -> domain from scratch (without .bit)
//hex -> hex
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
    }
    return QWidget::eventFilter(object, event);
}

void BuyNamesPage::onAsciiNameEdited(const QString &name)
{
    if (!walletModel)
        return;

    QString availableError = name_available(name);

    if (availableError == "")
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

void BuyNamesPage::onHexNameEdited(const QString &name)
{

    if (!walletModel)
        return;

    QString availableError;
    //check if it's even a valid hexdomain
    std::string hex = name.toStdString();
    if(!std::all_of(hex.begin(), hex.end(), ::isxdigit))
    {
        ui->statusLabel->setText(tr("%1 is not a valid hexadecimal entry!").arg(name));
    } 
    else 
    {
        std::string domain = ConvertDomainForms("0x" + hex);
        availableError = name_available(QString::fromStdString(domain));
    }
    
    if (availableError == "")
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

void BuyNamesPage::onDomainNameEdited(const QString &name){
    
    if (!walletModel)
        return;

    QString availableError;
    std::string domain = name.toStdString();
    //check if it even ends with .bit
    if(!name.toStdString().ends_with(".bit"))
    {
        ui->statusLabel->setText(tr("%1 does not end with .bit!").arg(name));
    } else {
        domain = ConvertDomainForms(domain);
        availableError = name_available(QString::fromStdString(domain));
    }

    if (availableError == "")
    {

        const std::string domain = ConvertDomainForms(name.toStdString());
        ui->statusLabel->setText(tr("%1 is available to register!").arg(name));
        ui->registerNameButton->show();
    }
    else
    {
        ui->statusLabel->setText(availableError);
        ui->registerNameButton->hide();
    }
}

void BuyNamesPage::onRegisterNameAction()
{
    if (!walletModel)
        return;

    //check which tab we're on
    int currentTab = ui->tabWidget->currentIndex();

    QString input, name;
    QMessageBox::StandardButton ErrorBox;

    //just do the conversion here...
    switch(currentTab) {
        case 0:
            {
                //strip off .bit
                input = ui->registerNameDomain->text();
                std::string domain = input.toStdString();
                if(!domain.ends_with(".bit")){
                    ErrorBox = QMessageBox::critical(this, tr("Invalid Namecoin Domain"),
                                tr("The inputted domain does not end with .bit."), QMessageBox::Cancel);
                    return;
                } else {
                    name = QString::fromStdString(ConvertDomainForms(domain));
                }
            }

            break;

        case 1:
            {
                //no changes needed
                input = ui->registerNameAscii->text();
                name = input;
                break;
            }
        case 2:
            {
                //check if valid hex
                input = ui->registerNameHex->text();
                std::string hex = input.toStdString();

                if(!std::all_of(hex.begin(), hex.end(), ::isxdigit)){
                    ErrorBox = QMessageBox::critical(this, tr("Invalid Hex Value"),
                                tr("The inputted hex value is invalid."), QMessageBox::Cancel);
     
                    return;
                } else {
                    std::string domain = ConvertDomainForms("0x" + input.toStdString());
                    name = QString::fromStdString(domain);
                }
            }

            break;

        default:
            //how did we get here?
            break;
    }

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
    ui->registerNameDomain->setText("");
    ui->registerNameAscii->setText("d/");
    ui->registerNameHex->setText("642f");
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
