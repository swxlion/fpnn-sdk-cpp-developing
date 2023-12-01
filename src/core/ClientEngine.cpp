#include <poll.h>
//#include <sys/sysinfo.h>
//#include <sys/types.h>
//#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <atomic>
#include <list>
#include "Config.h"
#include "FPLog.h"
#include "ignoreSignals.h"
#include "NetworkUtility.h"
#include "TimeUtil.h"
#include "TCPClient.h"
#include "UDPClient.h"
#include "ClientEngine.h"

using namespace fpnn;

/*
	We don't use gc_mutex in ClientEngine business logic, because gc_mutex maybe free before _engine.
	For some compiler, global variable free order maybe stack; but for other compiler, the free order maybe same as the init order.
	e.g. g++ with XCode on MacOS X.
*/
static std::mutex gc_mutex;
static std::atomic<bool> _created(false);
static ClientEnginePtr _engine;

//==================[ Poll Manager ]====================//

struct PollManager
{
	struct pollfd* fds;
	size_t maxCount;
	size_t currentCount;
	size_t appendCount;

	std::map<int, struct pollfd*> allFds;

	PollManager(int initCount, int appendCount_)
	{
		maxCount = initCount;
		currentCount = 0;
		appendCount = appendCount_;

		fds = (struct pollfd*)malloc(sizeof(struct pollfd) * initCount);
	}

	virtual ~PollManager()
	{
		if (fds)
			free(fds);
	}

	void join(int fd);
	void join(std::set<int>& newSockets);
	void quit(std::set<int>& quitSockets);

	void unsetWriteEvent(int fd)
	{
		allFds[fd]->events = POLLIN | POLLPRI;		//-- Linux 暂时不处理 POLLRDHUP.
	}
	void setWriteEvent(std::set<int>& wantWriteSocket);
};

void PollManager::join(int fd)
{
	if (allFds.find(fd) != allFds.end())
		return;

	if (currentCount + 1 > maxCount)
	{
		maxCount += appendCount;
		fds = (struct pollfd*)realloc(fds, maxCount * sizeof(struct pollfd));
	}

	fds[currentCount].fd = fd;
	fds[currentCount].events = POLLIN | POLLPRI;		//-- Linux 暂时不处理 POLLRDHUP.
	fds[currentCount].revents = 0;

	allFds[fd] = &fds[currentCount];

	currentCount += 1;
}

void PollManager::join(std::set<int>& newSockets)
{
	if (currentCount + newSockets.size() > maxCount)
	{
		size_t restCount = currentCount + newSockets.size() - maxCount;
		size_t appendBlockCount = restCount / appendCount;
		if (appendBlockCount * appendCount < restCount)
			appendBlockCount += 1;

		maxCount += appendBlockCount * appendCount;
		fds = (struct pollfd*)realloc(fds, maxCount * sizeof(struct pollfd));
	}

	for (int fd: newSockets)
	{
		if (allFds.find(fd) != allFds.end())
			continue;

		fds[currentCount].fd = fd;
		fds[currentCount].events = POLLIN | POLLPRI;		//-- Linux 暂时不处理 POLLRDHUP.
		fds[currentCount].revents = 0;

		allFds[fd] = &fds[currentCount];
		currentCount += 1;
	}
}

void PollManager::quit(std::set<int>& quitSockets)
{
	for (int fd: quitSockets)
	{
		for (size_t i = 0; i < currentCount; i++)
		{
			if (fds[i].fd == fd)
			{
				fds[i].fd = fds[currentCount - 1].fd;
				fds[i].events = fds[currentCount - 1].events;
				fds[i].revents = fds[currentCount - 1].revents;

				allFds[fds[currentCount - 1].fd] = &fds[i];

				currentCount -= 1;
				break;
			}
		}

		allFds.erase(fd);
	}
}

void PollManager::setWriteEvent(std::set<int>& wantWriteSocket)
{
	for (int fd: wantWriteSocket)
	{
		auto it = allFds.find(fd);
		if (it != allFds.end())
			it->second->events = POLLIN | POLLPRI | POLLOUT;		//-- Linux 暂时不处理 POLLRDHUP. MacOS & Linux 暂时不处理 POLLWRBAND。
	}	
}

//==================[ Client Engine ]====================//

ClientEnginePtr ClientEngine::create(const ClientEngineInitParams *params)
{
	if (!_created)
	{
		std::unique_lock<std::mutex> lck(gc_mutex);
		if (!_created)
		{
			_engine.reset(new ClientEngine(params));
			_created = true;
		}
	}
	return _engine;
}

ClientEngine::ClientEngine(const ClientEngineInitParams *params): _running(true),
	_newSocketSetChanged(false), _waitWriteSetChanged(false), _quitSocketSetChanged(false), _loopTicket(0)
{
	ClientEngineInitParams defaultParams;
	if (!params)
		params = &defaultParams;

	if (params->ignoreSignals)
		ignoreSignals();

	_logHolder = FPLog::instance();

	_connectTimeout = params->globalConnectTimeoutSeconds * 1000;
	_questTimeout = params->globalQuestTimeoutSeconds * 1000;

	_pollInitCount = params->basicConcurrencyCount;
	_pollAppendCount = params->appendConcurrencyCount;

	if (pipe(_notifyFds) != 0)	//-- Will failed when current processor using to many fds, or the system limitation reached.
		LOG_FATAL("ClientEngine create pipe for notification failed.");

	nonblockedFd(_notifyFds[0]);
	nonblockedFd(_notifyFds[1]);

	_callbackPool.init(0, 1, params->residentTaskThread, params->maxTaskThreads);

	_loopThread = std::thread(&ClientEngine::loopThread, this);
	_timeoutChecker = std::thread(&ClientEngine::timeoutCheckThread, this);
}

ClientEngine::~ClientEngine()
{
	_running = false;

	int count = (int)write(_notifyFds[1], this, 4);
	(void)count;

	_timeoutChecker.join();
	_loopThread.join();

	close(_notifyFds[1]);
	close(_notifyFds[0]);
}

bool ClientEngine::join(const BasicConnection* connection, bool waitForSending)
{
	int socket = connection->socket();
	_connectionMap.insert(socket, (BasicConnection*)connection);

	{
		std::unique_lock<std::mutex> lck(_mutex);

		_quitSocketSet.erase(socket);
		_newSocketSet.insert(socket);
		_newSocketSetChanged = true;

		if (waitForSending)
		{
			_waitWriteSet.insert(socket);
			_waitWriteSetChanged = true;
		}
	}

	int count = (int)write(_notifyFds[1], this, 4);
	(void)count;
	
	return true;
}

bool ClientEngine::waitSendEvent(const BasicConnection* connection)
{
	{
		std::unique_lock<std::mutex> lck(_mutex);
		if (_quitSocketSet.find(connection->socket()) != _quitSocketSet.end())
			return false;
		
		_waitWriteSet.insert(connection->socket());
		_waitWriteSetChanged = true;
	}

	int count = (int)write(_notifyFds[1], this, 4);
	(void)count;
	return true;
}

void ClientEngine::quit(BasicConnection* connection)
{
	int socket = connection->socket();
	_connectionMap.remove(socket);

	{
		std::unique_lock<std::mutex> lck(_mutex);
		_waitWriteSet.erase(socket);
		_newSocketSet.erase(socket);
		_quitSocketSet.insert(socket);
		_quitSocketSetChanged = true;
	}

	connection->_quitEngineLoopTicket = _loopTicket;

	int count = (int)write(_notifyFds[1], this, 4);
	(void)count;
}

void ClientEngine::sendTCPData(int socket, uint64_t token, std::string* data)
{
	if (!_connectionMap.sendTCPData(socket, token, data))
	{
		delete data;
		LOG_ERROR("TCP data not send at socket %d. socket maybe closed.", socket);
	}
}

void ClientEngine::sendUDPData(int socket, uint64_t token, std::string* data, int64_t expiredMS, bool discardable)
{
	if (expiredMS == 0)
		expiredMS = slack_real_msec() + _questTimeout;

	if (!_connectionMap.sendUDPData(socket, token, data, expiredMS, discardable))
	{
		delete data;
		LOG_WARN("UDP data not send at socket %d. socket maybe closed.", socket);
	}
}

void ClientEngine::clearConnectionQuestCallbacks(BasicConnection* connection, int errorCode)
{
	for (auto callbackPair: connection->_callbackMap)
	{
		BasicAnswerCallback* callback = callbackPair.second;
		if (callback->syncedCallback())		//-- check first, then fill result.
			callback->fillResult(NULL, errorCode);
		else
		{
			callback->fillResult(NULL, errorCode);

			BasicAnswerCallbackPtr task(callback);
			_callbackPool.wakeUp(task);
		}
	}
	// connection->_callbackMap.clear(); //-- If necessary.
}

void ClientEngine::closeUDPConnection(UDPClientConnection* connection)
{
	quit(connection);

	UDPClientPtr client = connection->client();
	if (client)
	{
		client->clearConnectionQuestCallbacks(connection, FPNN_EC_CORE_CONNECTION_CLOSED);
		client->willClose(connection, false);
	}
	else
	{
		clearConnectionQuestCallbacks(connection, FPNN_EC_CORE_CONNECTION_CLOSED);

		std::shared_ptr<ClientCloseTask> task(new ClientCloseTask(connection->questProcessor(), connection, false));
		_callbackPool.wakeUp(task);
		reclaim(task);
	}
}

void ClientEngine::clearConnection(int socket, int errorCode)
{
	BasicConnection* conn = _connectionMap.takeConnection(socket);
	if (conn == NULL)
		return;

	_connectionMap.remove(socket);
	clearConnectionQuestCallbacks(conn, errorCode);

	if (conn->connectionType() == BasicConnection::TCPClientConnectionType)
	{
		TCPClientPtr client = ((TCPClientConnection*)conn)->client();
		if (client)
		{
			client->willClose(conn, false);
			return;
		}
	}

	if (conn->connectionType() == BasicConnection::UDPClientConnectionType)
	{
		UDPClientPtr client = ((UDPClientConnection*)conn)->client();
		if (client)
		{
			client->willClose(conn, false);
			return;
		}
	}
	
	{
		std::shared_ptr<ClientCloseTask> task(new ClientCloseTask(conn->questProcessor(), conn, false));
		_callbackPool.wakeUp(task);
		reclaim(task);
	}
}

void ClientEngine::clean()
{
	_loopTicket += 5;	//-- Plus any number larger than 2 for all connections can be closed.

	std::set<int> fdSet;
	_connectionMap.getAllSocket(fdSet);
	
	for (int socket: fdSet)
		clearConnection(socket, FPNN_EC_CORE_CONNECTION_CLOSED);

	_connectionMap.waitForEmpty();
}

void ClientEngine::processConnectionIO(int fd, bool canRead, bool canWrite)
{
	BasicConnection* conn = _connectionMap.signConnection(fd);
	if (!conn)
		return;

	if (conn->connectionType() == BasicConnection::TCPClientConnectionType)
		TCPClientIOProcessor::processConnectionIO((TCPClientConnection*)conn, canRead, canWrite);
	else
		UDPClientIOProcessor::processConnectionIO((UDPClientConnection*)conn, canRead, canWrite);
}

void ClientEngine::loopThread()
{
	PollManager pollManager(_pollInitCount, _pollAppendCount);

	pollManager.join(_notifyFds[0]);		//-- _notifyFds[0] become pollManager.fds[0].

	while (_running)
	{
		int activeCount = poll(pollManager.fds, pollManager.currentCount, -1);
		if (activeCount > 0)
		{
			_loopTicket++;

			if (pollManager.fds[0].revents & (POLLIN | POLLPRI))	//-- pollManager.fds[0] is _notifyFds[0].
			{
				activeCount -= 1;
				consumeNotifyData();
				pollManager.fds[0].revents = 0;
			}

			if (_running == false)
				break;

			std::set<int> dropped;
			for (size_t i = 1; i < pollManager.currentCount; i++)
			{
				if (pollManager.fds[i].revents)
				{
					bool canRead = pollManager.fds[i].revents & (POLLIN | POLLPRI); 
					bool canWrite = pollManager.fds[i].revents & (POLLOUT | POLLWRBAND);

					if (canWrite)
						pollManager.unsetWriteEvent(pollManager.fds[i].fd);

					if (canRead || canWrite)
						processConnectionIO(pollManager.fds[i].fd, canRead, canWrite);
					else
					{
						// POLLERR or POLLHUP or POLLNVAL. On Linux, include POLLRDHUP.
						dropped.insert(pollManager.fds[i].fd);
					}

					pollManager.fds[i].revents = 0;
					activeCount -= 1;

					if (activeCount == 0)
						break;
				}
			}

			for (int socket: dropped)
			{
				clearConnection(socket, FPNN_EC_CORE_UNKNOWN_ERROR);
			}

			//-- check event flags
			{
				_loopTicket++;

				std::unique_lock<std::mutex> lck(_mutex);
				if (_quitSocketSetChanged)
				{
					dropped.insert(_quitSocketSet.begin(), _quitSocketSet.end());
					_quitSocketSet.clear();
					_quitSocketSetChanged = false;
				}

				if (_newSocketSetChanged)
				{
					pollManager.join(_newSocketSet);
					_newSocketSet.clear();
					_newSocketSetChanged = false;
				}

				if (_waitWriteSetChanged)
				{
					pollManager.setWriteEvent(_waitWriteSet);
					_waitWriteSet.clear();
					_waitWriteSetChanged = false;
				}
			}

			pollManager.quit(dropped);
		}
		else if (activeCount == -1)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			//-- If you meet these following error log, please tell swxlion@hotmail.com.
			if (errno == EFAULT)
			{
				LOG_ERROR("EFAULT when poll()! current %d, max %d.", (int)pollManager.currentCount, (int)pollManager.maxCount);
				continue;
			}

			if (errno == ENOMEM)
			{
				LOG_ERROR("ENOMEM when poll()!");
				break;
			}

			if (errno == EINVAL)
			{
				LOG_ERROR("EINVAL when poll()! OPEN_MAX is %d.", OPEN_MAX);
				break;
			}
			
			LOG_ERROR("Unknown Error when poll() errno: %d", errno);
			break;
		}
	}

	clean();
}

void ClientEngine::consumeNotifyData()
{
	const int buf_len = 4;

	int fd = _notifyFds[0];
	char buf[buf_len];

	while (true)
	{
		int readBytes = (int)::read(fd, buf, buf_len);
		if (readBytes != buf_len)
		{
			if (errno == EINTR || (readBytes > 0 && errno == 0))
				continue;
			else
				return;
		}
	}
}

void ClientEngine::reclaimConnections()
{
	std::set<IReleaseablePtr> deleted;
	uint64_t currentLoopTicket = _loopTicket;

	{
		std::unique_lock<std::mutex> lck(_mutex);
		for (IReleaseablePtr object: _reclaimedConnections)
		{
			if (object->releaseable(currentLoopTicket))
				deleted.insert(object);
		}
		for (IReleaseablePtr object: deleted)
			_reclaimedConnections.erase(object);
	}
	deleted.clear();
}

void ClientEngine::clearTimeoutQuest()
{
	int64_t current = TimeUtil::curr_msec();
	std::list<std::map<uint32_t, BasicAnswerCallback*> > timeouted;

	_connectionMap.extractTimeoutedCallback(current, timeouted);
	for (auto bacMap: timeouted)
	{
		for (auto bacPair: bacMap)
		{
			if (bacPair.second)
			{
				BasicAnswerCallback* callback = bacPair.second;
				if (callback->syncedCallback())		//-- check first, then fill result.
					callback->fillResult(NULL, FPNN_EC_CORE_TIMEOUT);
				else
				{
					callback->fillResult(NULL, FPNN_EC_CORE_TIMEOUT);

					BasicAnswerCallbackPtr task(callback);
					_callbackPool.wakeUp(task);
				}
			}
		}
	}
}

void ClientEngine::timeoutCheckThread()
{
	while (_running)
	{
		//-- Step 1: UDP period sending check

		int cyc = 100;
		int udpSendingCheckSyc = 5;
		while (_running && cyc--)
		{
			udpSendingCheckSyc -= 1;
			if (udpSendingCheckSyc == 0)
			{
				udpSendingCheckSyc = 5;
				std::unordered_set<UDPClientConnection*> invalidOrExpiredConnections;
				_connectionMap.periodUDPSendingCheck(invalidOrExpiredConnections);

				for (UDPClientConnection* conn: invalidOrExpiredConnections)
					closeUDPConnection(conn);
			}

			usleep(10000);
		}


		//-- Step 2: TCP client keep alive

		std::list<TCPClientConnection*> invalidConnections;
		std::list<TCPClientConnection*> connectExpiredConnections;

		_connectionMap.TCPClientKeepAlive(invalidConnections, connectExpiredConnections);
		for (auto conn: invalidConnections)
		{
			quit(conn);
			clearConnectionQuestCallbacks(conn, FPNN_EC_CORE_INVALID_CONNECTION);

			TCPClientPtr client = conn->client();
			if (client)
			{
				client->willClose(conn, true);
			}
			else
			{
				std::shared_ptr<ClientCloseTask> task(new ClientCloseTask(conn->questProcessor(), conn, true));
				_callbackPool.wakeUp(task);
				reclaim(task);
			}
		}

		for (auto conn: connectExpiredConnections)
		{
			quit(conn);
			clearConnectionQuestCallbacks(conn, FPNN_EC_CORE_INVALID_CONNECTION);

			TCPClientPtr client = conn->client();
			if (client)
			{
				client->connectFailed(conn->_connectionInfo, FPNN_EC_CORE_INVALID_CONNECTION);
				client->willClose(conn, true);
			}
			else
			{
				std::shared_ptr<ClientCloseTask> task(new ClientCloseTask(conn->questProcessor(), conn, true));
				_callbackPool.wakeUp(task);
				reclaim(task);
			}
		}

		//-- Step 3: clean timeouted callbacks

		clearTimeoutQuest();
		reclaimConnections();
	}
}

void ClientCloseTask::run()
{
	_executed = true;

	if (_questProcessor)
	try
	{
		if (_connection->connectionType() == BasicConnection::TCPClientConnectionType)
		{
			bool requireCallConnectionCannelledEvent;
			bool callCloseEvent = _connection->getCloseEventCallingPermission(requireCallConnectionCannelledEvent);

			if (callCloseEvent)
			{
				_questProcessor->connectionWillClose(*(_connection->_connectionInfo), _error);
			}
			else if (requireCallConnectionCannelledEvent)
			{
				_questProcessor->connected(*(_connection->_connectionInfo), false);
			}
		}
		else
		{
			_questProcessor->connectionWillClose(*(_connection->_connectionInfo), _error);
		}
	}
	catch (const FpnnError& ex){
		LOG_ERROR("ClientCloseTask::run() error:(%d)%s. %s", ex.code(), ex.what(), _connection->_connectionInfo->str().c_str());
	}
	catch (...)
	{
		LOG_ERROR("Unknown error when calling ClientCloseTask::run() function. %s", _connection->_connectionInfo->str().c_str());
	}
}
