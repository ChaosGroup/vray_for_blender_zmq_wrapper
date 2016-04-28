#ifndef _ZMQ_WRAPPER_H_
#define _ZMQ_WRAPPER_H_

#include <string>
#include <functional>
#include <thread>
#include <memory>
#include <queue>
#include <mutex>
#include <cstdio>

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

	std::chrono::high_resolution_clock::time_point lastHeartbeat;
	uint64_t pingTimeout;

	bool isWorking;
	bool errorConnect;
	bool isInit;
	bool flushOnExit;
	std::unique_ptr<zmq::socket_t> frontend;
};

#endif // _ZMQ_WRAPPER_H_
