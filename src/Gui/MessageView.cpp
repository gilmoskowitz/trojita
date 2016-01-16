/* Copyright (C) 2006 - 2014 Jan Kundrát <jkt@flaska.net>
   Copyright (C) 2014        Luke Dashjr <luke+trojita@dashjr.org>

   This file is part of the Trojita Qt IMAP e-mail client,
   http://trojita.flaska.net/

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License or (at your option) version 3 or any later version
   accepted by the membership of KDE e.V. (or its successor approved
   by the membership of KDE e.V.), which shall act as a proxy
   defined in Section 14 of version 3 of the license.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <QDebug>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebFrame>
#include <QWebHistory>
#include <QWebHitTestResult>
#include <QWebPage>

#include "MessageView.h"
#include "AbstractPartWidget.h"
#include "ComposeWidget.h"
#include "EmbeddedWebView.h"
#include "EnvelopeView.h"
#include "ExternalElementsWidget.h"
#include "OverlayWidget.h"
#include "PartWidgetFactoryVisitor.h"
#include "SimplePartWidget.h"
#include "Spinner.h"
#include "TagListWidget.h"
#include "UserAgentWebPage.h"
#include "Window.h"
#include "Common/InvokeMethod.h"
#include "Common/MetaTypes.h"
#include "Common/SettingsNames.h"
#include "Composer/QuoteText.h"
#include "Composer/SubjectMangling.h"
#include "Imap/Model/MailboxTree.h"
#include "Imap/Model/MsgListModel.h"
#include "Imap/Model/NetworkWatcher.h"
#include "Imap/Model/Utils.h"
#include "Imap/Network/MsgPartNetAccessManager.h"

namespace Gui
{

MessageView::MessageView(QWidget *parent, QSettings *settings, Plugins::PluginManager *pluginManager): QWidget(parent), m_settings(settings), m_pluginManager(pluginManager)
{
    QPalette pal = palette();
    pal.setColor(backgroundRole(), palette().color(QPalette::Active, QPalette::Base));
    pal.setColor(foregroundRole(), palette().color(QPalette::Active, QPalette::Text));
    setPalette(pal);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus); // not by the wheel
    netAccess = new Imap::Network::MsgPartNetAccessManager(this);
    connect(netAccess, &Imap::Network::MsgPartNetAccessManager::requestingExternal, this, &MessageView::externalsRequested);
    factory = new PartWidgetFactory(netAccess, this,
                                    std::unique_ptr<PartWidgetFactoryVisitor>(new PartWidgetFactoryVisitor()));

    emptyView = new EmbeddedWebView(this, new QNetworkAccessManager(this));
    emptyView->setFixedSize(450,300);
    CALL_LATER_NOARG(emptyView, handlePageLoadFinished);
    emptyView->setPage(new UserAgentWebPage(emptyView));
    emptyView->installEventFilter(this);
    emptyView->setAutoFillBackground(false);

    viewer = emptyView;

    //BEGIN create header section

    headerSection = new QWidget(this);

    // we create a dummy header, pass it through the style and the use it's color roles so we
    // know what headers in general look like in the system
    QHeaderView helpingHeader(Qt::Horizontal);
    helpingHeader.ensurePolished();
    pal = headerSection->palette();
    pal.setColor(headerSection->backgroundRole(), palette().color(QPalette::Active, helpingHeader.backgroundRole()));
    pal.setColor(headerSection->foregroundRole(), palette().color(QPalette::Active, helpingHeader.foregroundRole()));
    headerSection->setPalette(pal);
    headerSection->setAutoFillBackground(true);

    // the actual mail header
    m_envelope = new EnvelopeView(headerSection, this);

    // the tag bar
    tags = new TagListWidget(headerSection);
    tags->hide();
    connect(tags, &TagListWidget::tagAdded, this, &MessageView::newLabelAction);
    connect(tags, &TagListWidget::tagRemoved, this, &MessageView::deleteLabelAction);

    // whether we allow to load external elements
    externalElements = new ExternalElementsWidget(this);
    externalElements->hide();
    connect(externalElements, &ExternalElementsWidget::loadingEnabled, this, &MessageView::externalsEnabled);

    // layout the header
    layout = new QVBoxLayout(headerSection);
    layout->setSpacing(0);
    layout->addWidget(m_envelope, 1);
    layout->addWidget(tags, 3);
    layout->addWidget(externalElements, 1);

    //END create header section

    //BEGIN layout the message

    layout = new QVBoxLayout(this);
    layout->setSpacing(0);
    layout->setContentsMargins(0,0,0,0);

    layout->addWidget(headerSection, 1);

    headerSection->hide();

    // put the actual messages into an extra horizontal view
    // this allows us easy usage of the trailing stretch and also to indent the message a bit
    QHBoxLayout *hLayout = new QHBoxLayout;
    hLayout->setContentsMargins(6,6,6,0);
    hLayout->addWidget(viewer);
    static_cast<QVBoxLayout*>(layout)->addLayout(hLayout, 1);
    // add a strong stretch to squeeze header and message to the top
    // possibly passing a large stretch factor to the message could be enough...
    layout->addStretch(1000);

    //END layout the message

    // make the layout used to add messages our new horizontal layout
    layout = hLayout;

    markAsReadTimer = new QTimer(this);
    markAsReadTimer->setSingleShot(true);
    connect(markAsReadTimer, &QTimer::timeout, this, &MessageView::markAsRead);

    m_loadingSpinner = new Spinner(this);
    m_loadingSpinner->setText(tr("Fetching\nMessage"));
    m_loadingSpinner->setType(Spinner::Sun);
}

MessageView::~MessageView()
{
    // Redmine #496 -- the default order of destruction starts with our QNAM subclass which in turn takes care of all pending
    // QNetworkReply instances created by that manager. When the destruction goes to the WebKit objects, they try to disconnect
    // from the network replies which are however gone already. We can mitigate that by simply making sure that the destruction
    // starts with the QWebView subclasses and only after that proceeds to the QNAM. Qt's default order leads to segfaults here.
    if (viewer != emptyView) {
        delete viewer;
    }
    delete emptyView;

    delete factory;
}

void MessageView::setEmpty()
{
    markAsReadTimer->stop();
    m_envelope->setMessage(QModelIndex());
    headerSection->hide();
    if (message.isValid()) {
        disconnect(message.model(), &QAbstractItemModel::dataChanged, this, &MessageView::handleDataChanged);
    }
    message = QModelIndex();
    tags->hide();
    if (viewer != emptyView) {
        layout->removeWidget(viewer);
        viewer->deleteLater();
        viewer = emptyView;
        viewer->show();
        layout->addWidget(viewer);
        emit messageChanged();
        m_loadingItems.clear();
        m_loadingSpinner->stop();
    }
}

void MessageView::setMessage(const QModelIndex &index)
{
    Q_ASSERT(index.isValid());
    QModelIndex messageIndex = Imap::deproxifiedIndex(index);
    Q_ASSERT(messageIndex.isValid());

    // The data might be available from the local cache, so let's try to save a possible roundtrip here
    // by explicitly requesting the data
    messageIndex.data(Imap::Mailbox::RolePartData);

    if (!messageIndex.data(Imap::Mailbox::RoleIsFetched).toBool()) {
        // This happens when the message placeholder is already available in the GUI, but the actual message data haven't been
        // loaded yet. This is especially common with the threading model.
        // Note that the data might be already available in the cache, it's just that it isn't in the mailbox tree yet.
        setEmpty();
        connect(messageIndex.model(), &QAbstractItemModel::dataChanged, this, &MessageView::handleDataChanged);
        message = messageIndex;
        return;
    }

    QModelIndex rootPartIndex = messageIndex.child(0, 0);

    headerSection->show();
    if (message != messageIndex) {
        emptyView->hide();
        layout->removeWidget(viewer);
        if (viewer != emptyView) {
            viewer->setParent(0);
            viewer->deleteLater();
        }

        if (message.isValid()) {
            disconnect(message.model(), &QAbstractItemModel::dataChanged, this, &MessageView::handleDataChanged);
        }

        message = messageIndex;
        netAccess->setExternalsEnabled(false);
        externalElements->hide();

        netAccess->setModelMessage(message);

        m_loadingItems.clear();
        m_loadingSpinner->stop();

        UiUtils::PartLoadingOptions loadingMode;
        if (m_settings->value(Common::SettingsNames::guiPreferPlaintextRendering, QVariant(true)).toBool())
            loadingMode |= UiUtils::PART_PREFER_PLAINTEXT_OVER_HTML;
        viewer = factory->walk(rootPartIndex, 0, loadingMode);
        viewer->setParent(this);
        layout->addWidget(viewer);
        layout->setAlignment(viewer, Qt::AlignTop|Qt::AlignLeft);
        viewer->show();
        m_envelope->setMessage(message);

        tags->show();
        tags->setTagList(messageIndex.data(Imap::Mailbox::RoleMessageFlags).toStringList());
        connect(messageIndex.model(), &QAbstractItemModel::dataChanged, this, &MessageView::handleDataChanged);

        emit messageChanged();

        // We want to propagate the QWheelEvent to upper layers
        viewer->installEventFilter(this);
    }

    if (m_netWatcher && m_netWatcher->effectiveNetworkPolicy() != Imap::Mailbox::NETWORK_OFFLINE
            && m_settings->value(Common::SettingsNames::autoMarkReadEnabled, QVariant(true)).toBool()) {
        // No additional delay is needed here because the MsgListView won't open a message while the user keeps scrolling,
        // which was AFAIK the original intention
        markAsReadTimer->start(m_settings->value(Common::SettingsNames::autoMarkReadSeconds, QVariant(0)).toUInt() * 1000);
    }
}

void MessageView::markAsRead()
{
    if (!message.isValid())
        return;
    Imap::Mailbox::Model *model = const_cast<Imap::Mailbox::Model *>(dynamic_cast<const Imap::Mailbox::Model *>(message.model()));
    Q_ASSERT(model);
    if (!model->isNetworkAvailable())
        return;
    if (!message.data(Imap::Mailbox::RoleMessageIsMarkedRead).toBool())
        model->markMessagesRead(QModelIndexList() << message, Imap::Mailbox::FLAG_ADD);
}

/** @short Inhibit the automatic marking of the current message as already read

The user might have e.g. explicitly marked a previously read message as unread again immediately after navigating back to it
in the message listing. In that situation, the message viewer shall respect this decision and inhibit the helper which would
otherwise mark the current message as read after a short timeout.
*/
void MessageView::stopAutoMarkAsRead()
{
    markAsReadTimer->stop();
}

bool MessageView::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::Wheel) {
        // while the containing scrollview has Qt::StrongFocus, the event forwarding breaks that
        // -> completely disable focus for the following wheel event ...
        parentWidget()->setFocusPolicy(Qt::NoFocus);
        MessageView::event(event);
        // ... set reset it
        parentWidget()->setFocusPolicy(Qt::StrongFocus);
        return true;
    } else if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            MessageView::event(event);
            return true;
        case Qt::Key_Home:
        case Qt::Key_End:
            return false;
        default:
            return QObject::eventFilter(object, event);
        }
    } else {
        return QObject::eventFilter(object, event);
    }
}

QString MessageView::quoteText() const
{
    if (const AbstractPartWidget *w = dynamic_cast<const AbstractPartWidget *>(viewer)) {
        QStringList quote = Composer::quoteText(w->quoteMe().split(QLatin1Char('\n')));
        const Imap::Message::Envelope &e = message.data(Imap::Mailbox::RoleMessageEnvelope).value<Imap::Message::Envelope>();
        QString sender;
        if (!e.from.isEmpty())
            sender = e.from[0].prettyName(Imap::Message::MailAddress::FORMAT_JUST_NAME);
        if (e.from.isEmpty())
            sender = tr("you");

        // One extra newline at the end of the quoted text to separate the response
        quote << QString();

        return tr("On %1, %2 wrote:\n").arg(e.date.toLocalTime().toString(Qt::SystemLocaleLongDate), sender) + quote.join(QStringLiteral("\n"));
    }
    return QString();
}

void MessageView::setNetworkWatcher(Imap::Mailbox::NetworkWatcher *netWatcher)
{
    m_netWatcher = netWatcher;
    factory->setNetworkWatcher(netWatcher);
}

void MessageView::reply(MainWindow *mainWindow, Composer::ReplyMode mode)
{
    if (!message.isValid())
        return;

    // The Message-Id of the original message might have been empty; be sure we can handle that
    QByteArray messageId = message.data(Imap::Mailbox::RoleMessageMessageId).toByteArray();
    QList<QByteArray> messageIdList;
    if (!messageId.isEmpty()) {
        messageIdList.append(messageId);
    }

    ComposeWidget::warnIfMsaNotConfigured(
                ComposeWidget::createReply(mainWindow, mode, message, QList<QPair<Composer::RecipientKind,QString> >(),
                                           Composer::Util::replySubject(message.data(Imap::Mailbox::RoleMessageSubject).toString()),
                                           quoteText(), messageIdList,
                                           message.data(Imap::Mailbox::RoleMessageHeaderReferences).value<QList<QByteArray> >() + messageIdList),
                mainWindow);
}

void MessageView::forward(MainWindow *mainWindow, const Composer::ForwardMode mode)
{
    if (!message.isValid())
        return;

    // The Message-Id of the original message might have been empty; be sure we can handle that
    QByteArray messageId = message.data(Imap::Mailbox::RoleMessageMessageId).toByteArray();
    QList<QByteArray> messageIdList;
    if (!messageId.isEmpty()) {
        messageIdList.append(messageId);
    }

    ComposeWidget::warnIfMsaNotConfigured(
                ComposeWidget::createForward(mainWindow, mode, message, Composer::Util::forwardSubject(message.data(Imap::Mailbox::RoleMessageSubject).toString()),
                                             messageIdList, message.data(Imap::Mailbox::RoleMessageHeaderReferences).value<QList<QByteArray>>() + messageIdList),
                mainWindow);
}

void MessageView::externalsRequested(const QUrl &url)
{
    Q_UNUSED(url);
    externalElements->show();
}

void MessageView::externalsEnabled()
{
    netAccess->setExternalsEnabled(true);
    externalElements->hide();
    AbstractPartWidget *w = dynamic_cast<AbstractPartWidget *>(viewer);
    if (w)
        w->reloadContents();
}

void MessageView::newLabelAction(const QString &tag)
{
    if (!message.isValid())
        return;

    Imap::Mailbox::Model *model = dynamic_cast<Imap::Mailbox::Model *>(const_cast<QAbstractItemModel *>(message.model()));
    model->setMessageFlags(QModelIndexList() << message, tag, Imap::Mailbox::FLAG_ADD);
}

void MessageView::deleteLabelAction(const QString &tag)
{
    if (!message.isValid())
        return;

    Imap::Mailbox::Model *model = dynamic_cast<Imap::Mailbox::Model *>(const_cast<QAbstractItemModel *>(message.model()));
    model->setMessageFlags(QModelIndexList() << message, tag, Imap::Mailbox::FLAG_REMOVE);
}

void MessageView::handleDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight)
{
    Q_ASSERT(topLeft.row() == bottomRight.row() && topLeft.parent() == bottomRight.parent());
    if (topLeft == message) {
        if (viewer == emptyView && message.data(Imap::Mailbox::RoleIsFetched).toBool()) {
            qDebug() << "MessageView: message which was previously not loaded has just became available";
            setEmpty();
            setMessage(topLeft);
        }
        tags->setTagList(message.data(Imap::Mailbox::RoleMessageFlags).toStringList());
    }
}

void MessageView::setHomepageUrl(const QUrl &homepage)
{
    emptyView->load(homepage);
}

void MessageView::showEvent(QShowEvent *se)
{
    QWidget::showEvent(se);
    // The Oxygen style reset the attribute - since we're gonna cause an update() here anyway, it's
    // a good moment to stress that "we know better, Hugo ;-)" -- Thomas
    setAutoFillBackground(true);
}

void MessageView::partContextMenuRequested(const QPoint &point)
{
    if (SimplePartWidget *w = qobject_cast<SimplePartWidget *>(sender())) {
        QMenu menu(w);
        w->buildContextMenu(point, menu);
        menu.exec(w->mapToGlobal(point));
    }
}

void MessageView::partLinkHovered(const QString &link, const QString &title, const QString &textContent)
{
    Q_UNUSED(title);
    Q_UNUSED(textContent);
    emit linkHovered(link);
}

void MessageView::triggerSearchDialog()
{
    emit searchRequestedBy(qobject_cast<EmbeddedWebView*>(sender()));
}

QModelIndex MessageView::currentMessage() const
{
    return message;
}

void MessageView::onWebViewLoadStarted()
{
    QWebView *wv = qobject_cast<QWebView*>(sender());
    Q_ASSERT(wv);

    if (m_netWatcher && m_netWatcher->effectiveNetworkPolicy() != Imap::Mailbox::NETWORK_OFFLINE) {
        m_loadingItems << wv;
        m_loadingSpinner->start(250);
    }
}

void MessageView::onWebViewLoadFinished()
{
    QWebView *wv = qobject_cast<QWebView*>(sender());
    Q_ASSERT(wv);
    m_loadingItems.remove(wv);
    if (m_loadingItems.isEmpty())
        m_loadingSpinner->stop();
}

Plugins::PluginManager *MessageView::pluginManager() const
{
    return m_pluginManager;
}

}
