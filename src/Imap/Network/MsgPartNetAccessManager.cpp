/* Copyright (C) 2006 - 2011 Jan Kundrát <jkt@gentoo.org>

   This file is part of the Trojita Qt IMAP e-mail client,
   http://trojita.flaska.net/

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or the version 3 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/
#include <QNetworkRequest>
#include <QStringList>
#include <QDebug>

#include "MsgPartNetAccessManager.h"
#include "ForbiddenReply.h"
#include "MsgPartNetworkReply.h"
#include "Imap/Model/MailboxTree.h"
#include "Imap/Model/Model.h"

namespace Imap {

/** @short Classes associated with the implementation of the QNetworkAccessManager */
namespace Network {

MsgPartNetAccessManager::MsgPartNetAccessManager(QObject* parent):
    QNetworkAccessManager(parent), model(0), message(0), _externalsEnabled(false)
{
}

void MsgPartNetAccessManager::setModelMessage(Imap::Mailbox::Model* _model, Imap::Mailbox::TreeItemMessage* _message)
{
    // FIXME: use QPersistentModelIndex, redmine #6
    model = _model;
    message = _message;
}

/** @short Prepare a network request

This function handles delegating access to the other body parts using various schemes (ie. the special trojita-imap:// one used
by Trojita for internal purposes and the cid: one for referencing to other body parts).  Policy checks for filtering access to
the public Internet are also performed at this level.
*/
QNetworkReply* MsgPartNetAccessManager::createRequest(Operation op, const QNetworkRequest& req, QIODevice* outgoingData)
{
    Q_UNUSED(op);
    Q_UNUSED(outgoingData);
    Q_ASSERT(model);
    Q_ASSERT(message);
    Imap::Mailbox::TreeItemPart* part = pathToPart(req.url().path());
    QModelIndex partIndex = part->toIndex(model);

    if (req.url().scheme() == QLatin1String( "trojita-imap" ) && req.url().host() == QLatin1String("msg")) {
        // Internal Trojita reference
        if (part) {
            return new Imap::Network::MsgPartNetworkReply(this, partIndex);
        } else {
            qDebug() << "No such part:" << req.url();
            return new Imap::Network::ForbiddenReply( this );
        }
    } else if (req.url().scheme() == QLatin1String("cid")) {
        // The cid: scheme for cross-part references
        QByteArray cid = req.url().path().toUtf8();
        if (!cid.startsWith("<"))
            cid = QByteArray("<") + cid;
        if (!cid.endsWith(">"))
            cid += ">";
        Imap::Mailbox::TreeItemPart* target = cidToPart(cid, message);
        if (target) {
            return new Imap::Network::MsgPartNetworkReply(this, target->toIndex(model));
        } else {
            qDebug() << "Content-ID not found" << cid;
            return new Imap::Network::ForbiddenReply( this );
        }
    } else {
        // Regular access -- we've got to check policy here
        if (req.url().scheme() == QLatin1String("http") || req.url().scheme() == QLatin1String("ftp")) {
            if ( _externalsEnabled ) {
                return QNetworkAccessManager::createRequest(op, req, outgoingData);
            } else {
                emit requestingExternal( req.url() );
                return new Imap::Network::ForbiddenReply(this);
            }
        } else {
            qDebug() << "Forbidden per policy:" << req.url();
            return new Imap::Network::ForbiddenReply(this);
        }
    }
}

/** @short Find a message body part through its slash-separated string path */
Imap::Mailbox::TreeItemPart* MsgPartNetAccessManager::pathToPart(const QString& path)
{
    Imap::Mailbox::TreeItemPart* part = 0;
    QStringList items = path.split('/', QString::SkipEmptyParts);
    Imap::Mailbox::TreeItem* target = message;
    bool ok = ! items.isEmpty(); // if it's empty, it's a bogous URL

    for(QStringList::const_iterator it = items.begin(); it != items.end(); ++it) {
        uint offset = it->toUInt(&ok);
        if (!ok) {
            // special case, we have to dive into that funny, irregular special parts now
            if (*it == QString::fromAscii("HEADER"))
                target = target->specialColumnPtr(0, Imap::Mailbox::TreeItem::OFFSET_HEADER);
            else if (*it == QString::fromAscii("TEXT"))
                target = target->specialColumnPtr(0, Imap::Mailbox::TreeItem::OFFSET_TEXT);
            else if (*it == QString::fromAscii("MIME"))
                target = target->specialColumnPtr(0, Imap::Mailbox::TreeItem::OFFSET_MIME);
            break;
        }
        if (offset >= target->childrenCount(model)) {
            ok = false;
            break;
        }
        target = target->child(offset, model);
    }
    part = dynamic_cast<Imap::Mailbox::TreeItemPart*>(target);
    if (ok)
        Q_ASSERT(part);
    return part;
}

/** @short Convert a CID-formatted specification of a MIME part to a TreeItemPart*

The MIME messages contain a scheme which can be used to provide a reference from one message part to another using the content id
headers.  This function walks the MIME tree and tries to find a MIME part whose ID matches the requested item.
*/
Imap::Mailbox::TreeItemPart* MsgPartNetAccessManager::cidToPart(const QByteArray& cid, Imap::Mailbox::TreeItem* root)
{
    // A DFS search through the MIME parts tree of the current message which tries to check for a matching body part
    for (uint i = 0; i < root->childrenCount(model); ++i) {
        Imap::Mailbox::TreeItemPart* part = dynamic_cast<Imap::Mailbox::TreeItemPart*>(root->child(i, model));
        Q_ASSERT(part);
        if (part->bodyFldId() == cid)
            return part;
        part = cidToPart(cid, part);
        if (part)
            return part;
    }
    return 0;
}

/** @short Enable/disable fetching of external items

The external items are anything on the Internet which is outside of the scope of the current message.  At this time, we support
fetching contents via HTTP and FTP protocols.
*/
void MsgPartNetAccessManager::setExternalsEnabled(bool enabled)
{
    _externalsEnabled = enabled;
}

}
}


