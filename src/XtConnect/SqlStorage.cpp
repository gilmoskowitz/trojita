/*
    Certain enhancements (www.xtuple.com/trojita-enhancements)
    are copyright © 2010 by OpenMFG LLC, dba xTuple.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    - Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
    - Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    - Neither the name of xTuple nor the names of its contributors may be used to
    endorse or promote products derived from this software without specific prior
    written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
    ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
    ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "SqlStorage.h"
#include <QCryptographicHash>
#include <QDebug>
#include <QSqlError>
#include <QTimer>
#include <QVariant>

#define DEBUG false

namespace XtConnect {

SqlStorage::SqlStorage(QObject *parent, const QString &host, const int port, const QString &dbname, const QString &username, const QString &password ) :
    QObject(parent), _host(host), _port(port), _dbname(dbname), _username(username), _password(password)
{
    reconnect = new QTimer( this );
    reconnect->setSingleShot( true );
    reconnect->setInterval( 10 * 1000 );
    connect( reconnect, SIGNAL(timeout()), this, SLOT(slotReconnect()) );
}

void SqlStorage::open()
{
    db = QSqlDatabase::addDatabase( QLatin1String("QPSQL"), QLatin1String("xtconnect-sqlstorage") );
    if ( ! _host.isEmpty() )
        db.setHostName(_host);

    if ( _port != 5432 && _port > 0 && _port < 65536 )
        db.setPort(_port);

    if ( ! _dbname.isEmpty() )
        db.setDatabaseName( _dbname );

    if ( ! _username.isEmpty() )
        db.setUserName( _username );

    if ( ! _password.isEmpty() )
        db.setPassword( _password );

    if ( ! db.open() ) {
        _fail( QLatin1String("Failed to open database connection"), db );
    }

    _prepareStatements();
}

void SqlStorage::_prepareStatements()
{
    _queryFindMailBody = XSqlQuery(db);
    if ( ! _queryFindMailBody.prepare( QLatin1String("SELECT emlbody_id FROM xtbatch.emlbody "
                                                     " WHERE :emlbody_hash=emlbody_hash::bytea;")) )
                                                     _fail( QLatin1String("Failed to prepare query _queryFindMailBody"), _queryFindMailBody );

    _queryInsertMailBody = XSqlQuery(db);
    if ( ! _queryInsertMailBody.prepare( QLatin1String("INSERT INTO xtbatch.emlbody "
                                                   "(emlbody_hash, emlbody_body, emlbody_msg) "
                                                   "VALUES "
                                                   "(:emlbody_hash, :emlbody_body, :emlbody_msg) "
                                                   "returning emlbody_id;")) )
                                                    _fail( QLatin1String("Failed to prepare query _queryInsertMailBody"), _queryInsertMailBody );

    _queryInsertMail = XSqlQuery(db);
    if ( ! _queryInsertMail.prepare( QLatin1String("INSERT INTO xtbatch.eml "
                                                   "(eml_date, eml_subj, eml_emlbody_id, eml_status) "
                                                   "VALUES "
                                                   "(:eml_date, :eml_subj, :eml_emlbody_id, 'I') "
                                                   "returning eml_id;")) )
                                                    _fail( QLatin1String("Failed to prepare query _queryInsertMail"), _queryInsertMail );

    _queryInsertAddress = XSqlQuery(db);
    if ( ! _queryInsertAddress.prepare( QLatin1String("INSERT INTO xtbatch.emladdr "
                                                      "(emladdr_eml_id, emladdr_type, emladdr_addr, emladdr_name) "
                                                      "VALUES (:emladdr_eml_id, :emladdr_type, :emladdr_addr, "
                                                      ":emladdr_name)") ) )
        _fail( QLatin1String("Failed to prepare query _queryInsertAddress"), _queryInsertAddress );

    _queryMarkMailReady = QSqlQuery(db);
    if ( ! _queryMarkMailReady.prepare( QLatin1String("UPDATE xtbatch.eml SET eml_status = 'O' WHERE eml_id = ?") ) )
        _fail( QLatin1String("Failed to prepare query _queryMarkMailReady"), _queryMarkMailReady );
}

SqlStorage::ResultType SqlStorage::insertMail( const QDateTime &dateTime, const QString &subject, const QString &readableText, const QByteArray &headers, const QByteArray &body, quint64 &emlId )
{
    QByteArray hashValue = QCryptographicHash::hash( body, QCryptographicHash::Sha1 );
    int emlBodyId;

    if (DEBUG) qDebug() << "insertMail" << hashValue;
    _queryFindMailBody.bindValue( QLatin1String(":emlbody_hash"), hashValue );
    if ( ! _queryFindMailBody.exec() ) {
        _fail( QLatin1String("Query _queryFindMailBody failed"), _queryFindMailBody );
        return RESULT_ERROR;
    } else if ( _queryFindMailBody.first() ) {
        emlBodyId = _queryFindMailBody.value( 0 ).toULongLong();
    } else {
        if (DEBUG) qDebug() << "insertMail about to insert" << hashValue << readableText;
        _queryInsertMailBody.bindValue( QLatin1String(":emlbody_hash"), hashValue );
        _queryInsertMailBody.bindValue( QLatin1String(":emlbody_body"), readableText );
        _queryInsertMailBody.bindValue( QLatin1String(":emlbody_msg"), QVariant(headers + body));

        if ( ! _queryInsertMailBody.exec() ) {
            _fail( QLatin1String("Query _queryInsertMailBody failed"), _queryInsertMailBody );
            return RESULT_ERROR;
        } else if ( _queryInsertMailBody.first() ) {
            emlBodyId = _queryInsertMailBody.value( 0 ).toULongLong();
        } else {
            if ( _queryInsertMailBody.lastError().type() != QSqlError::NoError)
              _fail( QLatin1String("Query _queryInsertMailBody failed"), _queryInsertMailBody );
            else
              _fail( QLatin1String("Query _queryInsertMailBody returned no rows"), _queryInsertMailBody );
            return RESULT_ERROR;
        }
    }

    _queryInsertMail.bindValue( QLatin1String(":eml_hash"), hashValue );
    // Use ISODate, because it will specify that the time is in UTC.
    // Otherwise time is assumed to be local which would be bad
    _queryInsertMail.bindValue( QLatin1String(":eml_date"), dateTime.toString(Qt::ISODate));
    _queryInsertMail.bindValue( QLatin1String(":eml_subj"), subject );
    _queryInsertMail.bindValue( QLatin1String(":eml_emlbody_id"), emlBodyId );

    if ( ! _queryInsertMail.exec() ) {
        _fail( QLatin1String("Query _queryInsertMail failed"), _queryInsertMail );
        return RESULT_ERROR;
    }

    if ( _queryInsertMail.first() ) {
        emlId = _queryInsertMail.value( 0 ).toULongLong();
        return RESULT_OK;
    } else {
        return RESULT_ERROR;
    }
}

void SqlStorage::_fail(const QString &message, const QSqlQuery &query)
{
    if (!db.isOpen())
        reconnect->start();
    emit encounteredError(QString::fromLatin1("SqlStorage: Query Error: %1: %2").arg(message, query.lastError().text()));
}

void SqlStorage::_fail(const QString &message, const QSqlDatabase &database)
{
    if (!db.isOpen())
        reconnect->start();
    emit encounteredError(QString::fromLatin1("SqlStorage: Query Error: %1: %2").arg(message, database.lastError().text()));
}

void SqlStorage::fail(const QString &message)
{
    _fail(message, db);
}

Common::SqlTransactionAutoAborter SqlStorage::transactionGuard()
{
    return Common::SqlTransactionAutoAborter(&db);
}

SqlStorage::ResultType SqlStorage::insertAddress( const quint64 emlId, const QString &name, const QString &address, const QLatin1String kind )
{
    _queryInsertAddress.bindValue( QLatin1String(":emladdr_eml_id"), emlId );
    _queryInsertAddress.bindValue( QLatin1String(":emladdr_type"), kind );
    _queryInsertAddress.bindValue( QLatin1String(":emladdr_addr"), address );
    _queryInsertAddress.bindValue( QLatin1String(":emladdr_name"), name );

    if ( ! _queryInsertAddress.exec() ) {
        _fail( QLatin1String("Query _queryInsertAddress failed"), _queryInsertAddress );
        return RESULT_ERROR;
    }

    return RESULT_OK;
}

SqlStorage::ResultType SqlStorage::markMailReady( const quint64 emlId )
{
    _queryMarkMailReady.bindValue( 0, emlId );

    if ( ! _queryMarkMailReady.exec() ) {
        _fail( QLatin1String("Query _queryMarkMailReady failed"), _queryMarkMailReady );
        return RESULT_ERROR;
    }

    return RESULT_OK;
}

void SqlStorage::slotReconnect()
{
    qDebug() << "Trying to reconnect to the database...";
    // Release all DB resources
    _queryInsertAddress.clear();
    _queryInsertMail.clear();
    _queryMarkMailReady.clear();
    db.close();

    open();
}

}
