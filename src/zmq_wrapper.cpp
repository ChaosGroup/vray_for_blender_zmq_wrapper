#include "zmq_wrapper.h"
#include "zmq.hpp"
#include <chrono>
#include <condition_variable>
#include <random>


ZmqWrapper::ZmqWrapper()
    : context(new zmq::context_t(1))
    , isWorking(true)
    , errorConnect(false)
    , isInit(false)
    , flushOnExit(false)
    , frontend(nullptr)
{
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

		auto lastActiveTime = std::chrono::high_resolution_clock::now();
		zmq::message_t emptyFrame(0);
		bool didSomeWork = false;
		try {
			while (this->isWorking) {
				didSomeWork = false;

#ifdef VRAY_ZMQ_PING
				auto now = std::chrono::high_resolution_clock::now();

				// send keepalive
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActiveTime).count() > DISCONNECT_TIMEOUT / 2) {
					zmq::message_t keepAlive(1);
					this->frontend->send(emptyFrame, ZMQ_SNDMORE);
					if (this->frontend->send(keepAlive)) {
						lastActiveTime = now;
					}
					didSomeWork = true;
				}
#endif
				if (this->messageQue.size() && this->frontend->connected()) {
					while (this->messageQue.size()) {
						didSomeWork = true;
						std::lock_guard<std::mutex> lock(this->messageMutex);
						auto & msg = this->messageQue.front().getMessage();

						// since the wrapper is relying on msg size of 1 to ping wrap(pad so parsing is not broken) user messages that are with size 1 or less.
						// since messages are wrapped in VRayMessage user does/should not rely on the actual message size for anything
						if (msg.size() <= 1) {
							zmq::message_t wrapper(2);
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

#ifdef VRAY_ZMQ_PING
						lastActiveTime = now;
#endif
						this->messageQue.pop();
					}
				}

				zmq::message_t incoming, e;
				if (this->frontend->recv(&e, ZMQ_NOBLOCK)) {
					assert(!e.size() && "No empty frame expected from server!");
					this->frontend->recv(&incoming);
					didSomeWork = true;
#ifdef VRAY_ZMQ_PING
					bool propagateMessage = incoming.size() > 1;
#else
					bool propagateMessage = true;
#endif
					if (propagateMessage && this->callback) {
						VRayMessage msg(incoming);
						this->callback(msg, this);
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

bool ZmqWrapper::connected() const {
	return this->isInit && !this->errorConnect;
}

bool ZmqWrapper::good() const {
	return this->isWorking;
}

void ZmqWrapper::syncStop() {
	this->isWorking = false;
	this->isInit = true;
	if (this->worker.joinable()) {
		this->worker.join();
	}
}

void ZmqWrapper::forceFree() {
	this->isInit = true;
	this->isWorking = false;
	this->worker.detach();
}

ZmqWrapper::~ZmqWrapper() {
	this->isWorking = false;
	this->isInit = true;

	if (this->worker.joinable()) {
		this->worker.join();
	}

	this->worker = std::thread();
}

void ZmqWrapper::setFlushOnExit(bool flag) {
	flushOnExit = flag;
}

bool ZmqWrapper::getFlushOnexit() const {
	return flushOnExit;
}

void ZmqWrapper::setCallback(ZmqWrapperCallback_t cb) {
	this->callback = cb;
}

void ZmqWrapper::send(VRayMessage && message) {
	std::lock_guard<std::mutex> lock(this->messageMutex);
	this->messageQue.push(std::move(message));
}

void ZmqWrapper::send(void * data, int size) {
	VRayMessage msg(size);
	memcpy(msg.getMessage().data(), data, size);

	std::lock_guard<std::mutex> lock(this->messageMutex);
	this->messageQue.push(std::move(msg));
}

void ZmqClient::connect(const char * addr) {
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
