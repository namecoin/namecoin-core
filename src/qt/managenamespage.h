#ifndef MANAGENAMESPAGE_H
#define MANAGENAMESPAGE_H

#include <qt/platformstyle.h>

#include <QWidget>

class WalletModel;
class NameTableModel;

namespace Ui {
    class ManageNamesPage;
}

QT_BEGIN_NAMESPACE
class QTableView;
class QItemSelection;
class QSortFilterProxyModel;
class QMenu;
class QModelIndex;
QT_END_NAMESPACE

/** Page for managing names */
class ManageNamesPage : public QWidget
{
    Q_OBJECT

public:
    explicit ManageNamesPage(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~ManageNamesPage();

    void setModel(WalletModel *walletModel);

private:
    const PlatformStyle *platformStyle;
    Ui::ManageNamesPage *ui;
    NameTableModel *model;
    WalletModel *walletModel;
    QSortFilterProxyModel *proxyModel;
    QMenu *contextMenu;
    QAction *copyNameAction;
    QAction *copyValueAction;
    QAction *configureNameAction;
    QAction *renewNameAction;

public Q_SLOTS:
    void exportClicked();

private Q_SLOTS:
    bool eventFilter(QObject *object, QEvent *event);
    void selectionChanged();

    /** Spawn contextual menu (right mouse menu) for name table entry */
    void contextualMenu(const QPoint &point);

    void onCopyNameAction();
    void onCopyValueAction();
    void onConfigureNameAction();
    void onRenewNameAction();
};

#endif // MANAGENAMESPAGE_H
