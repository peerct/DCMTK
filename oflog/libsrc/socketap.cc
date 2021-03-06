// Module:  Log4CPLUS
// File:    socketappender.cxx
// Created: 5/2003
// Author:  Tad E. Smith
//
//
// Copyright 2003-2010 Tad E. Smith
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdlib>
#include "dcmtk/oflog/socketap.h"
#include "dcmtk/oflog/layout.h"
#include "dcmtk/oflog/spi/logevent.h"
#include "dcmtk/oflog/helpers/loglog.h"
#include "dcmtk/oflog/helpers/sleep.h"
#include "dcmtk/oflog/helpers/property.h"
#include "dcmtk/oflog/thread/syncpub.h"


namespace dcmtk {
namespace log4cplus {

int const DCMTK_LOG4CPLUS_MESSAGE_VERSION = 3;


#if ! defined (DCMTK_LOG4CPLUS_SINGLE_THREADED)
SocketAppender::ConnectorThread::ConnectorThread (
    SocketAppender & socket_appender)
    : sa (socket_appender)
    , trigger_ev ()
    , exit_flag (false)
{ }


SocketAppender::ConnectorThread::~ConnectorThread ()
{ }


void
SocketAppender::ConnectorThread::run ()
{
    while (true)
    {
        trigger_ev.timed_wait (30 * 1000);

        helpers::getLogLog().debug (
            DCMTK_LOG4CPLUS_TEXT("SocketAppender::ConnectorThread::run()")
            DCMTK_LOG4CPLUS_TEXT("- running..."));

        // Check exit condition as the very first thing.

        {
            thread::MutexGuard guard (access_mutex);
            if (exit_flag)
                return;
            trigger_ev.reset ();
        }

        // Do not try to re-open already open socket.

        {
            thread::MutexGuard guard (sa.access_mutex);
            if (sa.socket.isOpen ())
                continue;
        }

        // The socket is not open, try to reconnect.

        helpers::Socket new_socket (sa.host,
            OFstatic_cast(unsigned short, sa.port));
        if (! new_socket.isOpen ())
        {
            helpers::getLogLog().error(
                DCMTK_LOG4CPLUS_TEXT("SocketAppender::ConnectorThread::run()")
                DCMTK_LOG4CPLUS_TEXT("- Cannot connect to server"));

            // Sleep for a short while after unsuccessful connection attempt
            // so that we do not try to reconnect after each logging attempt
            // which could be many times per second.
            helpers::sleep (5);

            continue;
        }

        // Connection was successful, move the socket into SocketAppender.

        {
            thread::MutexGuard guard (sa.access_mutex);
            sa.socket = new_socket;
            sa.connected = true;
        }
    }
}


void
SocketAppender::ConnectorThread::terminate ()
{
    {
        thread::MutexGuard guard (access_mutex);
        exit_flag = true;
        trigger_ev.signal ();
    }
    join ();
}


void
SocketAppender::ConnectorThread::trigger ()
{
    trigger_ev.signal ();
}

#endif


//////////////////////////////////////////////////////////////////////////////
// SocketAppender ctors and dtor
//////////////////////////////////////////////////////////////////////////////

SocketAppender::SocketAppender(const tstring& host_,
    unsigned short port_, const tstring& serverName_)
: socket(),
  host(host_),
  port(port_),
  serverName(serverName_),
  connected(false),
  connector()
{
    openSocket();
    initConnector ();
}



SocketAppender::SocketAppender(const helpers::Properties & properties)
 : Appender(properties),
   socket(),
   host(),
   port(9998),
   serverName(),
   connected(false),
   connector()
{
    host = properties.getProperty( DCMTK_LOG4CPLUS_TEXT("host") );
    properties.getUInt (port, DCMTK_LOG4CPLUS_TEXT("port"));
    serverName = properties.getProperty( DCMTK_LOG4CPLUS_TEXT("ServerName") );

    openSocket();
    initConnector ();
}



SocketAppender::~SocketAppender()
{
#if ! defined (DCMTK_LOG4CPLUS_SINGLE_THREADED)
    connector->terminate ();
#endif

    destructorImpl();
}



//////////////////////////////////////////////////////////////////////////////
// SocketAppender public methods
//////////////////////////////////////////////////////////////////////////////

void 
SocketAppender::close()
{
    helpers::getLogLog().debug(
        DCMTK_LOG4CPLUS_TEXT("Entering SocketAppender::close()..."));

#if ! defined (DCMTK_LOG4CPLUS_SINGLE_THREADED)
    connector->terminate ();
#endif

    socket.close();
    closed = true;
}



//////////////////////////////////////////////////////////////////////////////
// SocketAppender protected methods
//////////////////////////////////////////////////////////////////////////////

void
SocketAppender::openSocket()
{
    if(!socket.isOpen()) {
        socket = helpers::Socket(host, OFstatic_cast(unsigned short, port));
    }
}


void
SocketAppender::initConnector ()
{
#if ! defined (DCMTK_LOG4CPLUS_SINGLE_THREADED)
    connected = true;
    connector = new ConnectorThread (*this);
    connector->start ();
#endif
}


void
SocketAppender::append(const spi::InternalLoggingEvent& event)
{
#if ! defined (DCMTK_LOG4CPLUS_SINGLE_THREADED)
    if (! connected)
    {
        connector->trigger ();
        return;
    }

#else
    if(!socket.isOpen()) {
        openSocket();
        if(!socket.isOpen()) {
            helpers::getLogLog().error(
                DCMTK_LOG4CPLUS_TEXT(
                    "SocketAppender::append()- Cannot connect to server"));
            return;
        }
    }
#endif

    helpers::SocketBuffer buffer(DCMTK_LOG4CPLUS_MAX_MESSAGE_SIZE - sizeof(unsigned int));
    helpers::convertToBuffer (buffer, event, serverName);
    helpers::SocketBuffer msgBuffer(DCMTK_LOG4CPLUS_MAX_MESSAGE_SIZE);

    msgBuffer.appendInt(OFstatic_cast(unsigned, buffer.getSize()));
    msgBuffer.appendBuffer(buffer);

    bool ret = socket.write(msgBuffer);
    if (! ret)
    {
#if ! defined (DCMTK_LOG4CPLUS_SINGLE_THREADED)
        connected = false;
        connector->trigger ();
#endif
    }
}


/////////////////////////////////////////////////////////////////////////////
// namespace helpers methods
/////////////////////////////////////////////////////////////////////////////

namespace helpers
{


void
convertToBuffer(SocketBuffer & buffer,
    const spi::InternalLoggingEvent& event,
    const tstring& serverName)
{
    buffer.appendByte(DCMTK_LOG4CPLUS_MESSAGE_VERSION);
#ifndef UNICODE
    buffer.appendByte(1);
#else
    buffer.appendByte(2);
#endif

    buffer.appendString(serverName);
    buffer.appendString(event.getLoggerName());
    buffer.appendInt(event.getLogLevel());
    buffer.appendString(event.getNDC());
    buffer.appendString(event.getMessage());
    buffer.appendString(event.getThread());
    buffer.appendInt( OFstatic_cast(unsigned int, event.getTimestamp().sec()) );
    buffer.appendInt( OFstatic_cast(unsigned int, event.getTimestamp().usec()) );
    buffer.appendString(event.getFile());
    buffer.appendInt(event.getLine());
    buffer.appendString(event.getFunction());
}


spi::InternalLoggingEvent
readFromBuffer(SocketBuffer& buffer)
{
    unsigned char msgVersion = buffer.readByte();
    if(msgVersion != DCMTK_LOG4CPLUS_MESSAGE_VERSION) {
        LogLog * loglog = LogLog::getLogLog();
        loglog->warn(DCMTK_LOG4CPLUS_TEXT("readFromBuffer() received socket message with an invalid version"));
    }

    unsigned char sizeOfChar = buffer.readByte();

    tstring serverName = buffer.readString(sizeOfChar);
    tstring loggerName = buffer.readString(sizeOfChar);
    LogLevel ll = buffer.readInt();
    tstring ndc = buffer.readString(sizeOfChar);
    if(! serverName.empty ()) {
        if(ndc.empty ()) {
            ndc = serverName;
        }
        else {
            ndc = serverName + DCMTK_LOG4CPLUS_TEXT(" - ") + ndc;
        }
    }
    tstring message = buffer.readString(sizeOfChar);
    tstring thread = buffer.readString(sizeOfChar);
    long sec = buffer.readInt();
    long usec = buffer.readInt();
    tstring file = buffer.readString(sizeOfChar);
    int line = buffer.readInt();
    tstring function = buffer.readString(sizeOfChar);

    // TODO: Pass MDC through.
    spi::InternalLoggingEvent ev (loggerName, ll, ndc,
        MappedDiagnosticContextMap (), message, thread, Time(sec, usec), file,
        line);
    ev.setFunction (function);
    return ev;
}


} // namespace helpers


} // namespace log4cplus
} // end namespace dcmtk
