#ifndef _ZMQ_WRAPPER_H_
#define _ZMQ_WRAPPER_H_

#include <string>
#include <functional>
#include <thread>
#include <memory>
#include <queue>
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


enum class ClientType: int {
	None,
	Exporter,
	Heartbeat,
};

/**
 * Async wrapper for zmq::socket_t with callback on data received.
 */
class ZmqWrapper {
public:
	typedef std::function<void(const VRayMessage &, ZmqWrapper *)> ZmqWrapperCallback_t;

	ZmqWrapper(bool isHeatbeat = false);
	~ZmqWrapper();

	/// send will copy size bytes from data internally, callee can free memory immediately
	void send(void *data, int size);

	/// send steals the message contents and is ZmqWrapper's resposibility
	void send(VRayMessage && message);

	void setCallback(ZmqWrapperCallback_t cb);

	// if set, all messages will be sent when dtor is called
	void setFlushOnExit(bool flag);
	bool getFlushOnexit() const;

	bool good() const;
	bool connected() const;
	void connect(const char * addr);

	void forceFree();
	void syncStop();

private:
	const ClientType clientType;
	ZmqWrapperCallback_t callback;
	std::thread worker;

	std::unique_ptr<zmq::context_t> context;
	std::queue<VRayMessage> messageQue;
	std::mutex messageMutex;
	std::mutex callbackMutex;

	std::chrono::high_resolution_clock::time_point lastHeartbeat;
	uint64_t pingTimeout;

	bool isWorking;
	bool errorConnect;
	bool isInit;
	bool flushOnExit;
	std::unique_ptr<zmq::socket_t> frontend;
};


inline ZmqWrapper::ZmqWrapper(bool isHeartbeat)
    : context(new zmq::context_t(1))
    , isWorking(true)
    , errorConnect(false)
    , isInit(false)
    , flushOnExit(false)
    , frontend(nullptr)
    , clientType(isHeartbeat ? ClientType::Heartbeat : ClientType::Exporter)
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

	this->worker = std::thread([this, &threadReady, &socketInit, &threadMutex] {
		try {
			std::lock_guard<std::mutex> lock(threadMutex);

			this->frontend = std::unique_ptr<zmq::socket_t>(new zmq::socket_t(*(this->context), ZMQ_DEALER));
			int linger = 0;
			this->frontend->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));

			socketInit = true;
		} catch (zmq::error_t & e) {
			printf("ZMQ exception while worker initialization: %s", e.what());
			this->isWorking = false;
			return;
		}
		threadReady.notify_all();

		while (!this->isInit || this->errorConnect) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
					while (this->messageQue.size()) {
						didSomeWork = true;
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
						lastHBSend = now;
						this->messageQue.pop();
					}
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
			assert(false && "Zmq exception!");
		}

		if (this->flushOnExit) {
			try {
				int wait = 200;
				this->frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));
				std::lock_guard<std::mutex> lock(this->messageMutex);

				while (!this->messageQue.empty()) {
					auto & msg = this->messageQue.front().getMessage();

					if (!this->frontend->send(emptyFrame, ZMQ_SNDMORE)) {
						break;
					}

					this->frontend->send(msg);
					this->messageQue.pop();
				}

				this->frontend->close();
			} catch (zmq::error_t &e) {
				printf("ZMQ exception while flushing on exit: %s", e.what());
			}
		}

		this->isWorking = false;
	});


	{
		std::unique_lock<std::mutex> lock(threadMutex);
		// wait for the thread to finish initing the socket, else bind & connect might be called before init
		threadReady.wait(lock, [&socketInit] { return socketInit; });
	}
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
	this->isInit = true;
}

inline bool ZmqWrapper::connected() const {
	return this->isInit && !this->errorConnect;
}

inline bool ZmqWrapper::good() const {
	return this->isWorking;
}

inline void ZmqWrapper::syncStop() {
	this->isWorking = false;
	this->isInit = true;
	if (this->worker.joinable()) {
		this->worker.join();
	}
}

inline void ZmqWrapper::forceFree() {
	this->isInit = true;
	this->isWorking = false;
	this->worker.detach();
}

inline ZmqWrapper::~ZmqWrapper() {
	this->isWorking = false;
	this->isInit = true;

	if (this->worker.joinable()) {
		this->worker.join();
	}

	this->worker = std::thread();
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
	this->messageQue.push(std::move(message));
}

inline void ZmqWrapper::send(void * data, int size) {
	VRayMessage msg(size);
	memcpy(msg.getMessage().data(), data, size);

	std::lock_guard<std::mutex> lock(this->messageMutex);
	this->messageQue.push(std::move(msg));
}


#endif // _ZMQ_WRAPPER_H_
