/* Copyright (C) 2007 - 2010 Jan Kundrát <jkt@flaska.net>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or version 3 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/


#include "CreateMailboxTask.h"
#include "CreateConnectionTask.h"
#include "Model.h"
#include "MailboxTree.h"

namespace Imap {
namespace Mailbox {


CreateMailboxTask::CreateMailboxTask( Model* _model, const QString& _mailbox ):
    ImapTask( _model ), mailbox(_mailbox)
{
    conn = new CreateConnectionTask( _model, 0 );
    conn->addDependentTask( this );
}

void CreateMailboxTask::perform()
{
    parser = conn->parser;
    model->_parsers[ parser ].activeTasks.append( this );

    tagCreate = parser->create( mailbox );
    model->_parsers[ parser ].commandMap[ tagCreate ] = Model::Task( Model::Task::CREATE, 0 );
    emit model->activityHappening( true );
}

bool CreateMailboxTask::handleStateHelper( Imap::Parser* ptr, const Imap::Responses::State* const resp )
{
    if ( resp->tag == tagCreate ) {
        IMAP_TASK_ENSURE_VALID_COMMAND( tagCreate, Model::Task::CREATE );

        if ( resp->kind == Responses::OK ) {
            emit model->mailboxCreationSucceded( mailbox );
            tagList = parser->list( QLatin1String(""), mailbox );
            model->_parsers[ parser ].commandMap[ tagList ] = Model::Task( Model::Task::LIST_AFTER_CREATE, command->str );
            emit model->activityHappening( true );
        } else {
            emit model->mailboxCreationFailed( mailbox, resp->message );
            _completed();
            // FIXME: proper error handling for the Tasks API
        }
        IMAP_TASK_CLEANUP_COMMAND;
        return true;
    } else if ( resp->tag == tagList ) {
        IMAP_TASK_ENSURE_VALID_COMMAND( tagList, Model::Task::LIST_AFTER_CREATE );
        if ( resp->kind == Responses::OK ) {
            model->_finalizeIncrementalList( ptr, mailbox );
        } else {
            // FIXME
        }
        _completed();
        IMAP_TASK_CLEANUP_COMMAND;
        return true;
    } else {
        return false;
    }
}


}
}
