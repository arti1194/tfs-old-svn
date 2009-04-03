//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////
#include "otpch.h"

#include "connection.h"
#include "protocol.h"
#include "outputmessage.h"
#include "tasks.h"
#include "scheduler.h"
#include "configmanager.h"
#include "tools.h"
#include "server.h"

extern ConfigManager g_config;
#ifdef __ENABLE_SERVER_DIAGNOSTIC__
uint32_t Connection::connectionCount = 0;
#endif

ConnectionManager::ConnectionManager()
{
	OTSYS_THREAD_LOCKVARINIT(m_connectionManagerLock);

	maxLoginTries = g_config.getNumber(ConfigManager::LOGIN_TRIES);
	retryTimeout = (int32_t)g_config.getNumber(ConfigManager::RETRY_TIMEOUT) / 1000;
	loginTimeout = (int32_t)g_config.getNumber(ConfigManager::LOGIN_TIMEOUT) / 1000;
}

Connection* ConnectionManager::createConnection(boost::asio::io_service& io_service, ServicePort_ptr servicer)
{
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Creating new connection" << std::endl;
	#endif
	OTSYS_THREAD_LOCK_CLASS lockClass(m_connectionManagerLock);

	Connection* connection = new Connection(io_service, servicer);
	m_connections.push_back(connection);
	return connection;
}

void ConnectionManager::releaseConnection(Connection* connection)
{
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Releasing connection" << std::endl;
	#endif
	OTSYS_THREAD_LOCK_CLASS lockClass(m_connectionManagerLock);

	std::list<Connection*>::iterator it = std::find(m_connections.begin(), m_connections.end(), connection);
	if(it != m_connections.end())
		m_connections.erase(it);
	else
		std::cout << "[Error - ConnectionManager::releaseConnection] Connection not found" << std::endl;
}

bool ConnectionManager::isDisabled(uint32_t clientIp, int32_t protocolId)
{
	OTSYS_THREAD_LOCK_CLASS lockClass(m_connectionManagerLock);
	if(maxLoginTries == 0 || clientIp == 0)
		return false;

	IpConnectionMap::const_iterator it = ipConnectionMap.find(clientIp);
	return it != ipConnectionMap.end() && it->second.lastProtocol != protocolId &&
		it->second.loginsAmount > maxLoginTries && (int32_t)time(NULL) < it->second.lastLogin + loginTimeout;
}

void ConnectionManager::addAttempt(uint32_t clientIp, int32_t protocolId, bool success)
{
	OTSYS_THREAD_LOCK_CLASS lockClass(m_connectionManagerLock);
	if(clientIp == 0)
		return;

	IpConnectionMap::iterator it = ipConnectionMap.find(clientIp);
	if(it == ipConnectionMap.end())
	{
		ConnectionBlock tmp;
		tmp.lastLogin = tmp.loginsAmount = 0;
		tmp.lastProtocol = 0x00;

		ipConnectionMap[clientIp] = tmp;
		it = ipConnectionMap.find(clientIp);
	}

	if(it->second.loginsAmount > maxLoginTries)
		it->second.loginsAmount = 0;

	int32_t currentTime = time(NULL);
	if(!success || (currentTime < it->second.lastLogin + retryTimeout))
		it->second.loginsAmount++;
	else
		it->second.loginsAmount = 0;

	it->second.lastLogin = currentTime;
	it->second.lastProtocol = protocolId;
}

void ConnectionManager::closeAll()
{
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Closing all connections" << std::endl;
	#endif
	OTSYS_THREAD_LOCK_CLASS lockClass(m_connectionManagerLock);

	std::list<Connection*>::iterator it = m_connections.begin();
	while(it != m_connections.end())
	{
		boost::system::error_code error;
		(*it)->m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
		(*it)->m_socket.close(error);
		++it;
	}

	m_connections.clear();
}

uint32_t Connection::getIP() const
{
	//Ip is expressed in network byte order
	boost::system::error_code error;
	const boost::asio::ip::tcp::endpoint endpoint = m_socket.remote_endpoint(error);
	if(!error)
		return htonl(endpoint.address().to_v4().to_ulong());

	PRINT_ASIO_ERROR("Getting remote ip");
	return 0;
}

void Connection::handle(Protocol* protocol)
{
	m_protocol = protocol;
	m_protocol->onConnect();
	accept();
}

void Connection::accept()
{
	// Read size of the first packet
	m_pendingRead++;
	boost::asio::async_read(m_socket, boost::asio::buffer(m_msg.getBuffer(), NetworkMessage::header_length),
		boost::bind(&Connection::parseHeader, this, boost::asio::placeholders::error));
}

void Connection::close()
{
	//any thread
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Connection::close" << std::endl;
	#endif
	OTSYS_THREAD_LOCK_CLASS lockClass(m_connectionLock);
	if(m_closeState != CLOSE_STATE_NONE)
		return;

	m_closeState = CLOSE_STATE_REQUESTED;
	Dispatcher::getDispatcher().addTask(createTask(boost::bind(&Connection::closeConnection, this)));
}

bool Connection::send(OutputMessage_ptr msg)
{
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Connection::send init" << std::endl;
	#endif
	OTSYS_THREAD_LOCK(m_connectionLock, "");
	if(m_closeState == CLOSE_STATE_CLOSING || m_writeError)
	{
		OTSYS_THREAD_UNLOCK(m_connectionLock, "");
		return false;
	}

	if(msg->getProtocol())
		msg->getProtocol()->onSendMessage(msg);

	if(m_pendingWrite == 0)
	{
		#ifdef __DEBUG_NET_DETAIL__
		std::cout << "Connection::send " << msg->getMessageLength() << std::endl;
		#endif
		internalSend(msg);
	}
	else
	{
		#ifdef __DEBUG_NET__
		std::cout << "Connection::send Adding to queue " << msg->getMessageLength() << std::endl;
		#endif
		m_outputQueue.push_back(msg);

		m_pendingWrite++;
		if(m_pendingWrite > 500 && g_config.getBool(ConfigManager::FORCE_CLOSE_SLOW_CONNECTION))
		{
			std::cout << "> NOTICE: Forcing slow connection to disconnect" << std::endl;
			close();
		}
	}

	OTSYS_THREAD_UNLOCK(m_connectionLock, "");
	return true;
}

void Connection::internalSend(OutputMessage_ptr msg)
{
	m_pendingWrite++;
	boost::asio::async_write(m_socket, boost::asio::buffer(msg->getOutputBuffer(), msg->getMessageLength()),
		boost::bind(&Connection::onWrite, this, msg, boost::asio::placeholders::error));
}

void Connection::closeConnection()
{
	//dispatcher thread
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Connection::closeConnection" << std::endl;
	#endif

	OTSYS_THREAD_LOCK(m_connectionLock, "");
	if(m_closeState != CLOSE_STATE_REQUESTED)
	{
		std::cout << "[Error - Connection::closeConnection] m_closeState = " << m_closeState << std::endl;
		OTSYS_THREAD_UNLOCK(m_connectionLock, "");
		return;
	}

	m_closeState = CLOSE_STATE_CLOSING;
	if(m_protocol)
	{
		Dispatcher::getDispatcher().addTask(createTask(boost::bind(&Protocol::releaseProtocol, m_protocol)));
		m_protocol->setConnection(NULL);
		m_protocol = NULL;
	}

	if(!write())
		OTSYS_THREAD_UNLOCK(m_connectionLock, "");
}

void Connection::releaseConnection()
{
	if(m_refCount > 0) //Reschedule it and try again.
		Scheduler::getScheduler().addEvent(createSchedulerTask(SCHEDULER_MINTICKS, boost::bind(&Connection::releaseConnection, this)));
	else
		deleteConnection();
}

void Connection::deleteConnection()
{
	//dispatcher thread
	assert(m_refCount == 0);
	delete this;
}

void Connection::parseHeader(const boost::system::error_code& error)
{
	OTSYS_THREAD_LOCK(m_connectionLock, "");
	m_pendingRead--;
	if(m_closeState == CLOSE_STATE_CLOSING)
	{
		if(!write())
			OTSYS_THREAD_UNLOCK(m_connectionLock, "");

		return;
	}

	int32_t size = m_msg.decodeHeader();
	if(!error && size > 0 && size < NETWORKMESSAGE_MAXSIZE - 16)
	{
		// Read packet content
		m_pendingRead++;
		m_msg.setMessageLength(size + NetworkMessage::header_length);
		boost::asio::async_read(m_socket, boost::asio::buffer(m_msg.getBodyBuffer(), size),
			boost::bind(&Connection::parsePacket, this, boost::asio::placeholders::error));
	}
	else
		handleReadError(error);

	OTSYS_THREAD_UNLOCK(m_connectionLock, "");
}

void Connection::parsePacket(const boost::system::error_code& error)
{
	OTSYS_THREAD_LOCK(m_connectionLock, "");
	m_pendingRead--;
	if(m_closeState == CLOSE_STATE_CLOSING)
	{
		if(!write())
			OTSYS_THREAD_UNLOCK(m_connectionLock, "");

		return;
	}

	if(!error)
	{
		// Checksum
		uint32_t length = m_msg.getMessageLength() - m_msg.getReadPos() - 4, receivedChecksum = m_msg.PeekU32(), checksum = 0;
		if(length)
			checksum = adlerChecksum((uint8_t*)(m_msg.getBuffer() + m_msg.getReadPos() + 4), length);

		bool checksumEnabled = false;
		if(receivedChecksum == checksum)
		{
			m_msg.SkipBytes(4);
			checksumEnabled = true;
		}

		// Protocol selection
		if(!m_receivedFirst)
		{
			// First message received
			m_receivedFirst = true;
			if(!m_protocol)
			{
				m_protocol = m_port->makeProtocol(checksumEnabled, m_msg);
				if(!m_protocol)
				{
					close();
					OTSYS_THREAD_UNLOCK(m_connectionLock, "");
					return;
				}

				m_protocol->setConnection(this);
			}
			else
				m_msg.SkipBytes(1);

			m_protocol->onRecvFirstMessage(m_msg);
		}
		else
			m_protocol->onRecvMessage(m_msg);

		// Wait to the next packet
		m_pendingRead++;
		boost::asio::async_read(m_socket, boost::asio::buffer(m_msg.getBuffer(), NetworkMessage::header_length),
			boost::bind(&Connection::parseHeader, this, boost::asio::placeholders::error));
	}
	else
		handleReadError(error);

	OTSYS_THREAD_UNLOCK(m_connectionLock, "");
}

bool Connection::write()
{
	//any thread
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Connection::write" << std::endl;
	#endif
	if(m_pendingWrite == 0 || m_writeError)
	{
		if(!m_socketClosed)
		{
			#ifdef __DEBUG_NET_DETAIL__
			std::cout << "Closing socket" << std::endl;
			#endif

			boost::system::error_code error;
			m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
			if(error && error != boost::asio::error::not_connected)
				PRINT_ASIO_ERROR("Shutdown");

			m_socket.close(error);
			m_socketClosed = true;
			if(error)
				PRINT_ASIO_ERROR("Close");
		}

		if(m_pendingRead == 0)
		{
			#ifdef __DEBUG_NET_DETAIL__
			std::cout << "Deleting Connection" << std::endl;
			#endif

			OTSYS_THREAD_UNLOCK(m_connectionLock, "");
			Dispatcher::getDispatcher().addTask(createTask(boost::bind(&Connection::releaseConnection, this)));
			return true;
		}
	}

	return false;
}

void Connection::onWrite(OutputMessage_ptr msg, const boost::system::error_code& error)
{
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "onWrite" << std::endl;
	#endif
	OTSYS_THREAD_LOCK(m_connectionLock, "");

	msg.reset();
	if(!error)
	{
		if(m_pendingWrite > 0)
		{
			if(!m_outputQueue.empty())
			{
				msg = m_outputQueue.front();
				m_outputQueue.pop_front();

				m_pendingWrite--;
				internalSend(msg);
				#ifdef __DEBUG_NET_DETAIL__
				std::cout << "Connection::onWrite send " << msg->getMessageLength() << std::endl;
				#endif
			}

			m_pendingWrite--;
		}
		else // Pending operations counter is 0, but we are getting a notification!
			std::cout << "[Error - Connection::onWrite] Getting unexpected notification!" << std::endl;
	}
	else
	{
		m_pendingWrite--;
		handleWriteError(error);
	}

	if(m_closeState == CLOSE_STATE_CLOSING)
	{
		if(!write())
			OTSYS_THREAD_UNLOCK(m_connectionLock, "");

		return;
	}

	OTSYS_THREAD_UNLOCK(m_connectionLock, "");
}

void Connection::handleReadError(const boost::system::error_code& error)
{
	#ifdef __DEBUG_NET_DETAIL__
	PRINT_ASIO_ERROR("Reading - detail");
	#endif
	if(error == boost::asio::error::operation_aborted)
	{
		//Operation aborted because connection will be closed
		//Do NOT call closeConnection() from here
	}
	else if(error == boost::asio::error::eof || error == boost::asio::error::connection_reset
		|| error == boost::asio::error::connection_aborted) //No more to read or connection closed remotely
		close();
	else
	{
		PRINT_ASIO_ERROR("Reading");
		close();
	}

	m_readError = true;
}

void Connection::handleWriteError(const boost::system::error_code& error)
{
	#ifdef __DEBUG_NET_DETAIL__
	PRINT_ASIO_ERROR("Writing - detail");
	#endif
	if(error == boost::asio::error::operation_aborted)
		{} //Operation aborted because connection will be closed, do NOT call closeConnection() from here
	else if(error == boost::asio::error::eof || error == boost::asio::error::connection_reset
		|| error == boost::asio::error::connection_aborted) //No more to read or connection closed remotely
		close();
	else
	{
		PRINT_ASIO_ERROR("Writing");
		close();
	}

	m_writeError = true;
}
