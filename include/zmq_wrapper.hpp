#ifndef _ZMQ_WRAPPER_H_
#define _ZMQ_WRAPPER_H_

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

#define NOMINMAX // zmq includes windows.h
#include <zmq.hpp>

#include "base_types.h"
#include "zmq_message.hpp"

static const uint64_t EXPORTER_TIMEOUT = 5000;
static const uint64_t HEARBEAT_TIMEOUT = 2000;
static const int MAX_CONSEQ_MESSAGES = 10;

enum class ClientType: int {
	None,
	Exporter,
	Heartbeat,
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
	typedef std::chrono::high_resolution_clock::time_point time_point;

	/// Start function for the worker thread (sends and receives messages)
	void workerThread(volatile bool & socketInit, std::mutex & mtx, std::condition_variable & workerReady);
	/// Send any outstanding messages
	bool workerSendoutMessages(time_point & lastHBSend);

	const ClientType clientType; ///< The type of this client (heartbeat or exporter)
	ZmqWrapperCallback_t callback; ///< Callback to be called on received message
	std::mutex callbackMutex; ///< Mutex protecting @callback

	std::thread worker; ///< Thread serving messages and calling the callback

	std::unique_ptr<zmq::context_t> context; ///< The zmq context
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
    , context(new zmq::context_t(1))
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
		std::lock_guard<std::mutex> lock(mtx);

		this->frontend = std::unique_ptr<zmq::socket_t>(new zmq::socket_t(*(this->context), ZMQ_DEALER));
		int linger = 0;
		this->frontend->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));

		socketInit = true;
	} catch (zmq::error_t & e) {
		printf("ZMQ exception while worker initialization: %s", e.what());
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

	if (this->errorConnect) {
		this->isWorking = false;
		return;
	}

	auto lastHBRecv = std::chrono::high_resolution_clock::now();
	// ensure we send one HB immediately
	auto lastHBSend = lastHBRecv - std::chrono::milliseconds(HEARBEAT_TIMEOUT * 2);

	zmq::message_t emptyFrame(0);

	bool didSomeWork = false;
	try {
		while (this->isWorking) {
			didSomeWork = false;

			auto now = std::chrono::high_resolution_clock::now();

			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHBSend).count() > pingTimeout / 2) {
				zmq::message_t keepAlive(&clientType, sizeof(clientType));
				this->frontend->send(emptyFrame, ZMQ_SNDMORE);
				if (this->frontend->send(keepAlive)) {
					lastHBSend = now;
				}
				didSomeWork = true;
			}

			if (clientType == ClientType::Heartbeat) {
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHBRecv).count() > pingTimeout) {
					puts("ZMQ hearbeat no responce...");
					break;
				}
			}

			if (this->messageQue.size() && this->frontend->connected()) {
				didSomeWork = didSomeWork || this->workerSendoutMessages(lastHBSend);
			}

			zmq::message_t incoming, e;
			if (this->frontend->recv(&e, ZMQ_NOBLOCK)) {
				assert(!e.size() && "No empty frame expected from server!");
				this->frontend->recv(&incoming);
				didSomeWork = true;

				if (incoming.size() == sizeof(ClientType)) {
					lastHBRecv = now;
				} else if (incoming.size() > sizeof(ClientType)) {
					std::lock_guard<std::mutex> cbLock(callbackMutex);
					if (this->callback) {
						this->callback(VRayMessage(incoming), this);
					}
				} else {
					puts("ZMQ unrecognized server message, discarding...");
				}
			}

			if (!didSomeWork) {
				// if we didn't do anything - just sleep and dont busy wait
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	} catch (zmq::error_t & e) {
		printf("ZMQ exception in worker thread: %s", e.what());
		this->flushOnExit = false;
		assert(false && "Zmq exception!");
	}

	if (this->flushOnExit) {
		try {
			int wait = 200;
			this->frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));
			std::lock_guard<std::mutex> lock(this->messageMutex);

			for (int c = 0; c < this->messageQue.size(); ++c) {
				auto & msg = this->messageQue[c].getMessage();

				if (!this->frontend->send(emptyFrame, ZMQ_SNDMORE)) {
					break;
				}

				if (!this->frontend->send(msg)) {
					break;
				}
			}

			this->frontend->close();
		} catch (zmq::error_t &e) {
			printf("ZMQ exception while flushing on exit: %s", e.what());
		}
	}

	this->isWorking = false;
}

inline bool ZmqWrapper::workerSendoutMessages(time_point & lastHBSend) {
	bool didWork = false;
	zmq::message_t emptyFrame(0);
	for (int c = 0; c < MAX_CONSEQ_MESSAGES && !this->messageQue.empty(); ++c) {
		didWork = true;
		std::lock_guard<std::mutex> lock(this->messageMutex);
		auto & msg = this->messageQue.front().getMessage();

		// since the wrapper is relying on msg size of sizeof(clientType) to ping wrap(pad so parsing is not broken)
		// user messages that are with size sizeof(clientType) or less.
		// since messages are wrapped in VRayMessage user does/should not rely on the actual message size for anything
		if (msg.size() <= sizeof(clientType)) {
			zmq::message_t wrapper(sizeof(clientType) + 1);
			memcpy(wrapper.data(), msg.data(), msg.size());
			msg.move(&wrapper);
		}

		int wait = 200;
		this->frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));
		if (!this->frontend->send(emptyFrame, ZMQ_SNDMORE)) {
			break;
		}
		wait = -1;
		this->frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));


		try {
			this->frontend->send(msg);
		} catch (zmq::error_t & e) {
			printf("ZMQ exception while zmq send: %s", e.what());
			assert(false && "Failed to send payload after empty frame && exception");
		}

		// update hb send since we sent a message
		lastHBSend = std::chrono::high_resolution_clock::now();
		this->messageQue.pop_front();
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
		printf("ZMQ::connect(%s) exception: %s", addr, e.what());
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
	this->isWorking = false;
	this->startServing = true;
	if (this->worker.joinable()) {
		this->worker.join();
	}
	this->worker = std::thread();
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
