#include <qt/configurenamedialog.h>
#include <qt/forms/ui_configurenamedialog.h>

// TODO: How many of these are actually still needed?
#include <names/main.h>
#include <qt/addressbookpage.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <wallet/wallet.h>

#include <QMessageBox>
#include <QClipboard>

ConfigureNameDialog::ConfigureNameDialog(const PlatformStyle *platformStyle,
                                         const QString &_name, const QString &data,
                                         QWidget *parent) :
    QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint),
    ui(new Ui::ConfigureNameDialog),
    platformStyle(platformStyle),
    name(_name)
{
    ui->setupUi(this);

    if (platformStyle->getUseExtraSpacing())
        ui->transferToLayout->setSpacing(4);

    GUIUtil::setupAddressWidget(ui->transferTo, this);

    ui->labelName->setText(name);
    ui->dataEdit->setText(data);

    returnData = data;

    ui->labelSubmitHint->setText(tr("name_update transaction will be issued immediately"));
    setWindowTitle(tr("Update Name"));
}


ConfigureNameDialog::~ConfigureNameDialog()
{
    delete ui;
}

void ConfigureNameDialog::accept()
{
    if (!walletModel)
        return;

    QString addr = ui->transferTo->text();

    if (addr != "" && !walletModel->validateAddress(addr))
    {
        ui->transferTo->setValid(false);
        return;
    }

    returnData = ui->dataEdit->text();
    returnTransferTo = ui->transferTo->text();

    QDialog::accept();
}

void ConfigureNameDialog::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
}

void ConfigureNameDialog::on_pasteButton_clicked()
{
    // Paste text from clipboard into recipient field
    ui->transferTo->setText(QApplication::clipboard()->text());
}

void ConfigureNameDialog::on_addressBookButton_clicked()
{
    if (!walletModel)
        return;

    AddressBookPage dlg(
        // platformStyle
        platformStyle,
        // mode
        AddressBookPage::ForSelection,
        // tab
        AddressBookPage::SendingTab,
        // *parent
        this);
    dlg.setModel(walletModel->getAddressTableModel());
    if (dlg.exec())
        ui->transferTo->setText(dlg.getReturnValue());
}
