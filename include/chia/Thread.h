/*
 * Thread.h
 *
 *  Created on: May 23, 2021
 *      Author: mad
 */

#ifndef INCLUDE_CHIA_THREAD_H_
#define INCLUDE_CHIA_THREAD_H_

#include <mutex>
#include <thread>
#include <functional>
#include <condition_variable>

#include <pthread.h>


template<typename T>
class Thread {
public:
	Thread(const std::function<void(T&)>& func, const std::string& name = "")
		:	execute(func)
	{
		thread = std::thread(&Thread::loop, this, name);
	}
	
	virtual ~Thread() {
		close();
	}
	
	void take(T& data) {
		{
			std::unique_lock<std::mutex> lock(mutex);
			while(is_avail) {
				signal.wait(lock);
			}
			input = std::move(data);
			is_avail = true;
		}
		signal.notify_all();
	}
	
	void close() {
		std::unique_lock<std::mutex> lock(mutex);
		do_run = false;
		if(thread.joinable()) {
			lock.unlock();
			signal.notify_all();
			thread.join();
		}
	}
	
private:
	void loop(const std::string& name) noexcept
	{
		if(!name.empty()) {
			std::string thread_name = name;
			// limit the name to 15 chars, otherwise pthread_setname_np() fails
			if(thread_name.size() > 15) {
				thread_name.resize(15);
			}
			pthread_setname_np(pthread_self(), thread_name.c_str());
		}
		std::unique_lock<std::mutex> lock(mutex);
		while(true) {
			while(do_run && !is_avail) {
				signal.wait(lock);
			}
			if(!do_run) {
				break;
			}
			lock.unlock();
			execute(input);
			lock.lock();
			is_avail = false;
			signal.notify_all();
		}
	}
	
private:
	T input;
	bool do_run = true;
	bool is_avail = false;
	std::mutex mutex;
	std::thread thread;
	std::condition_variable signal;
	std::function<void(T&)> execute;
	
};


#endif /* INCLUDE_CHIA_THREAD_H_ */
