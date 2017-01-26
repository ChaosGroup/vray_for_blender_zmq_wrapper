#ifndef _ZMQ_WRAPPER_H_
#define _ZMQ_WRAPPER_H_

#define NOMINMAX // zmq includes windows.h
#include <zmq.hpp>

#include <string>
#include <functional>
#include <thread>
#include <memory>
#include <deque>
#include <mutex>
#include <cstdio>

#include <chrono>
#include <condition_variable>
#include <random>
#include <limits>

#include "base_types.h"
#include "zmq_message.hpp"

static const int EXPORTER_TIMEOUT = 5000;
static const int HEARBEAT_TIMEOUT = 2000;
static const int MAX_CONSEQ_MESSAGES = 10;

enum class ClientType: int {
	None,
	Exporter,
	Heartbeat,
};

enum class ControlMessage: int {
	DATA_MSG = 0,

	EXPORTER_CONNECT_MSG = 1000,
	HEARTBEAT_CONNECT_MSG = 1001,

	RENDERER_CREATE_MSG = 2000,
	HEARTBEAT_CREATE_MSG = 2001,

	PING_MSG = 3000,
	PONG_MSG = 3001,
};

static const int ZMQ_PROTOCOL_VERSION = 1000;

struct ControlFrame {
	int version;
	ClientType type;
	ControlMessage control;

	ControlFrame(ClientType type = ClientType::Exporter, ControlMessage ctrl = ControlMessage::DATA_MSG)
		: version(ZMQ_PROTOCOL_VERSION)
		, type(type)
		, control(ctrl) {}

	explicit ControlFrame(const zmq::message_t & msg) {
		if (msg.size() != sizeof(*this)) {
			version = -1;
		} else {
			memcpy(this, msg.data(), sizeof(*this));
		}
	}

	explicit operator bool() {
		return version == ZMQ_PROTOCOL_VERSION;
	}

	static zmq::message_t make(ClientType type = ClientType::Exporter, ControlMessage ctrl = ControlMessage::DATA_MSG) {
		zmq::message_t msg(sizeof(ControlFrame));
		ControlFrame frame(type, ctrl);
		memcpy(msg.data(), &frame, msg.size());
		return msg;
	}
};


/// Async wrapper for zmq::socket_t with callback on data received.
class ZmqWrapper {
public:
	typedef std::function<void(const VRayMessage &, ZmqWrapper *)> ZmqWrapperCallback_t;

	ZmqWrapper(bool isHeatbeat = false);
	~ZmqWrapper();

	/// Send data with size, the data will be copied inside and can be safely freed after the function returns
	/// @data - pointer to bytes
	/// @size - number of bytes in data
	void send(void *data, int size);

	/// Send message while also stealing it's content
	/// @message - the message to send, after the function returns, callee's message is empty
	void send(VRayMessage && message);

	/// Set a callback to be called on message received (messages discarded if not set)
	void setCallback(ZmqWrapperCallback_t cb);

	/// Set or clear flag to flush outstanding messages on stop/exit
	void setFlushOnExit(bool flag);
	/// Check the flush on exit flag
	bool getFlushOnexit() const;

	/// Get number of messages that are yet to be sent to server
	int getOutstandingMessages() const;

	/// Check if the worker is serving
	bool good() const;

	/// Check if currently the socket is connected
	bool connected() const;

	/// Connect to address
	/// @addr - the address to connect to
	void connect(const char * addr);

	/// Stop the server waiting for the worker thread to join
	void syncStop();

private:
	void sendDataMessage(zmq::message_t & msg);

	typedef std::chrono::high_resolution_clock::time_point time_point;

	/// Start function for the worker thread (sends and receives messages)
	void workerThread(volatile bool & socketInit, std::mutex & mtx, std::condition_variable & workerReady);
	/// Send any outstanding messages
	bool workerSendoutMessages(time_point & lastHBSend);

	const ClientType clientType; ///< The type of this client (heartbeat or exporter)
	ZmqWrapperCallback_t callback; ///< Callback to be called on received message
	std::mutex callbackMutex; ///< Mutex protecting @callback

	std::thread worker; ///< Thread serving messages and calling the callback

	zmq::context_t context; ///< The zmq context
	std::deque<VRayMessage> messageQue; ///< Queue with outstanding messages
	std::mutex messageMutex; ///< Mutex protecting @messageQue

	std::condition_variable startServingCond; ///< Cond var to signal the worker thread to start serving
	std::mutex startServingMutex; ///< Mutex protecting @startServing flag
	bool startServing; ///< Used to signal worker, the socket is connected and serving can start

	time_point lastHeartbeat; ///< Last time hartbeat was sent/received
	uint64_t pingTimeout; ///< Timeout for pings, if no answer worker stops serving

	bool isWorking; ///< Flag set to true if the thread is serving requests
	bool errorConnect; ///< Flag set to true if we could not connect
	bool flushOnExit; ///< If true when worker is stopping for any reason, outstanding messages will be sent
	std::unique_ptr<zmq::socket_t> frontend; ///< The zmq socket
};


inline ZmqWrapper::ZmqWrapper(bool isHeartbeat)
    : clientType(isHeartbeat ? ClientType::Heartbeat : ClientType::Exporter)
    , context(1)
    , isWorking(true)
    , errorConnect(false)
    , startServing(false)
    , flushOnExit(false)
    , frontend(nullptr)
{
	// send keepalive
	switch (clientType) {
	case ClientType::Heartbeat:
		pingTimeout = HEARBEAT_TIMEOUT;
		break;
	case ClientType::Exporter:
		pingTimeout = EXPORTER_TIMEOUT;
		break;
	default:
		pingTimeout = std::numeric_limits<uint64_t>::max();
	}


	bool socketInit = false;
	std::condition_variable threadReady;
	std::mutex threadMutex;

	worker = std::thread(&ZmqWrapper::workerThread, this, std::ref(socketInit), std::ref(threadMutex), std::ref(threadReady));

	{
		std::unique_lock<std::mutex> lock(threadMutex);
		// wait for the thread to finish initing the socket, else bind & connect might be called before init
		threadReady.wait(lock, [&socketInit] { return socketInit; });
	}
}

inline void ZmqWrapper::workerThread(volatile bool & socketInit, std::mutex & mtx, std::condition_variable & workerReady) {
	try {

		this->frontend = std::unique_ptr<zmq::socket_t>(new zmq::socket_t(context, ZMQ_DEALER));
		int linger = 0;
		this->frontend->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));

		int wait = HEARBEAT_TIMEOUT / 2;
		this->frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));

		std::lock_guard<std::mutex> lock(mtx);
		socketInit = true;
	} catch (zmq::error_t & e) {
		printf("ZMQ exception while worker initialization: %s\n", e.what());
		this->isWorking = false;
		workerReady.notify_all();
		return;
	}
	workerReady.notify_all();

	if (!this->startServing) {
		std::unique_lock<std::mutex> lock(this->startServingMutex);
		if (!this->startServing) {
			this->startServingCond.wait(lock, [this]() { return this->startServing; });
		}
	}

	std::shared_ptr<void> atScopeExit(nullptr, [this] (void *) {
		this->frontend->close();
		this->isWorking = false;
	});

	if (this->errorConnect) {
		return;
	}

	zmq::message_t emtpyFrame(0);

	// send handshake
	try {
		if (clientType == ClientType::Exporter) {
			frontend->send(ControlFrame::make(clientType, ControlMessage::EXPORTER_CONNECT_MSG), ZMQ_SNDMORE);
		} else {
			frontend->send(ControlFrame::make(clientType, ControlMessage::HEARTBEAT_CONNECT_MSG), ZMQ_SNDMORE);
		}
		this->frontend->send(emtpyFrame);
	} catch (zmq::error_t & ex) {
		printf("ZMQ failed to send handshake [%s]\n", ex.what());
		return;
	}


	// recv handshake
	try {
		int wait = EXPORTER_TIMEOUT;
		this->frontend->setsockopt(ZMQ_RCVTIMEO, &wait, sizeof(wait));

		zmq::message_t controlMsg, emptyMsg;
		bool recv = frontend->recv(&controlMsg);
		if (!recv) {
			puts("ZMQ server did not respond in expected timeout, stopping client!");
			return;
		}
		frontend->recv(&emptyMsg);

		ControlFrame frame(controlMsg);

		if (!frame) {
			printf("ZMQ expected protocol version [%d], server speaks [%d]\n", ZMQ_PROTOCOL_VERSION, frame.version);
			return;
		}

		if (frame.type != clientType) {
			puts("ZMQ server created mismatching type of worker for us!");
			return;
		}

		if (clientType == ClientType::Exporter) {
			if (frame.control != ControlMessage::RENDERER_CREATE_MSG) {
				puts("ZMQ server responded with different than renderer created!");
				return;
			}
		} else {
			if (frame.control != ControlMessage::HEARTBEAT_CREATE_MSG) {
				puts("ZMQ server responded with different than heartbeat created!");
				return;
			}
		}
	} catch (zmq::error_t & ex) {
		printf("ZMQ failed to receive handshake [%s]\n", ex.what());
		return;
	}

	puts("ZMQ connected to server.");

	auto lastHBRecv = std::chrono::high_resolution_clock::now();
	// ensure we send one HB immediately
	auto lastHBSend = lastHBRecv - std::chrono::milliseconds(HEARBEAT_TIMEOUT * 2);

	zmq::pollitem_t pollContext = {*this->frontend, 0, ZMQ_POLLIN | ZMQ_POLLOUT, 0};

	while (isWorking) {
		bool didWork = false;
		auto now = std::chrono::high_resolution_clock::now();

		try {
			const int pollRes = zmq::poll(&pollContext, 1, 10);
		} catch (zmq::error_t & ex) {
			printf("ZMQ failed [%s] zmq::poll - stopping client.\n", ex.what());
			return;
		}

		if (pollContext.revents & ZMQ_POLLIN) {
			didWork = true;

			for (int c = 0; c < MAX_CONSEQ_MESSAGES && isWorking; ++c) {
				zmq::message_t controlMsg, payloadMsg;
				try {
					this->frontend->recv(&controlMsg);
					this->frontend->recv(&payloadMsg);
				} catch (zmq::error_t & ex) {
					printf("ZMQ failed [%s] zmq::socket_t::recv - stopping client.\n", ex.what());
					return;
				}

				ControlFrame frame(controlMsg);

				if (!frame) {
					printf("ZMQ expected protocol version [%d], server speaks [%d], dropping message.\n", ZMQ_PROTOCOL_VERSION, frame.version);
					continue;
				}

				if (frame.type != clientType) {
					puts("ZMQ server sent mismatching msg type of worker for us!");
					continue;
				}

				lastHBRecv = std::chrono::high_resolution_clock::now();

				if (frame.control == ControlMessage::DATA_MSG) {
					std::lock_guard<std::mutex> cbLock(callbackMutex);
					if (this->callback) {
						this->callback(VRayMessage(payloadMsg), this);
					}
				} else if (frame.control == ControlMessage::PING_MSG) {
					if (payloadMsg.size() != 0) {
						puts("ZMQ missing empty frame after ping");
					}
				} else if (frame.control == ControlMessage::PONG_MSG) {
					if (payloadMsg.size() != 0) {
						puts("ZMQ missing empty frame after pong");
					}
				}

				int more = 0;
				size_t more_size = sizeof (more);
				try {
					frontend->getsockopt(ZMQ_RCVMORE, &more, &more_size);
				} catch (zmq::error_t & ex) {
					printf("ZMQ failed [%s] zmq::socket_t::getsockopt.\n", ex.what());
				}
				if (!more) {
					break;
				}
			}
		}

		if (pollContext.revents & ZMQ_POLLOUT) {
			try {
				now = std::chrono::high_resolution_clock::now();
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHBSend).count() > pingTimeout / 2) {
					bool sent = frontend->send(ControlFrame::make(clientType, ControlMessage::PING_MSG), ZMQ_SNDMORE);
					if (sent) {
						sent = frontend->send(emtpyFrame);
						lastHBSend = now;
						didWork = true;
					}
				}

				didWork = didWork || !messageQue.empty();
				workerSendoutMessages(lastHBSend);
			} catch (zmq::error_t & ex) {
				printf("ZMQ failed [%s] zmq::socket_t::send - stopping client.\n", ex.what());
				return;
			}
		}

		if (clientType == ClientType::Heartbeat && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHBRecv).count() > pingTimeout) {
			puts("ZMQ server unresponsive, stopping client");
			return;
		}

		if (!didWork) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}


	if (this->flushOnExit) {
		try {
			int wait = 200;
			this->frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));
			std::lock_guard<std::mutex> lock(this->messageMutex);

			for (int c = 0; c < this->messageQue.size(); ++c) {
				auto & msg = this->messageQue[c].getMessage();
				bool sent = frontend->send(ControlFrame::make(), ZMQ_SNDMORE);
				sent = sent && this->frontend->send(msg);
				if (!sent) {
					break;
				}
			}

			this->frontend->close();
		} catch (zmq::error_t &e) {
			printf("ZMQ exception while flushing on exit: %s\n", e.what());
		}
	}
}

inline bool ZmqWrapper::workerSendoutMessages(time_point & lastHBSend) {
	bool didWork = false;
	for (int c = 0; c < MAX_CONSEQ_MESSAGES && !this->messageQue.empty() && isWorking; ++c) {
		didWork = true;
		std::lock_guard<std::mutex> lock(this->messageMutex);
		auto & msg = this->messageQue.front().getMessage();

		bool sent = frontend->send(ControlFrame::make(ClientType::Exporter, ControlMessage::DATA_MSG), ZMQ_SNDMORE);
		if (sent) {
			sent = frontend->send(msg);
			// update hb send since we sent a message
			lastHBSend = std::chrono::high_resolution_clock::now();
			this->messageQue.pop_front();

			int more = 0;
			size_t more_size = sizeof (more);
			frontend->getsockopt(ZMQ_RCVMORE, &more, &more_size);
			if (!more) {
				break;
			}
		} else {
			break;
		}

	}

	return didWork;
}

inline void ZmqWrapper::connect(const char * addr) {
	std::random_device device;
	std::mt19937_64 generator(device());
	uint64_t id = generator();

	this->frontend->setsockopt(ZMQ_IDENTITY, &id, sizeof(id));

	try {
		this->frontend->connect(addr);
	} catch (zmq::error_t & e) {
		printf("ZMQ zmq::socket_t::connect(%s) exception: %s\n", addr, e.what());
		this->errorConnect = true;
	}

	{
		std::lock_guard<std::mutex> lock(startServingMutex);
		this->startServing = true;
	}
	startServingCond.notify_one();
}

inline int ZmqWrapper::getOutstandingMessages() const {
	return this->messageQue.size();
}

inline bool ZmqWrapper::connected() const {
	return this->startServing && !this->errorConnect;
}

inline bool ZmqWrapper::good() const {
	return this->isWorking;
}

inline void ZmqWrapper::syncStop() {
	{
		std::lock_guard<std::mutex> lock(startServingMutex);
		isWorking = false;
		startServing = true;
		startServingCond.notify_all();
	}

	context.close();
	if (worker.joinable()) {
		worker.join();
	}
	worker = std::thread();
}

inline ZmqWrapper::~ZmqWrapper() {
	this->syncStop();
}

inline void ZmqWrapper::setFlushOnExit(bool flag) {
	flushOnExit = flag;
}

inline bool ZmqWrapper::getFlushOnexit() const {
	return flushOnExit;
}

inline void ZmqWrapper::setCallback(ZmqWrapperCallback_t cb) {
	std::lock_guard<std::mutex> cbLock(callbackMutex);
	this->callback = cb;
}

inline void ZmqWrapper::send(VRayMessage && message) {
	std::lock_guard<std::mutex> lock(this->messageMutex);
	this->messageQue.push_back(std::move(message));
}

inline void ZmqWrapper::send(void * data, int size) {
	VRayMessage msg(size);
	memcpy(msg.getMessage().data(), data, size);

	std::lock_guard<std::mutex> lock(this->messageMutex);
	this->messageQue.push_back(std::move(msg));
}


#endif // _ZMQ_WRAPPER_H_
