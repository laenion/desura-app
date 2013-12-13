/*
Desura is the leading indie game distribution platform
Copyright (C) 2011 Mark Chandler (Desura Net Pty Ltd)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>
*/


#include "Common.h"
#include "util_thread/BaseThread.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "util_thread/ReadWriteMutex.h"

#define MS_VC_EXCEPTION 0x406D1388

#ifdef NIX
	#include <sys/prctl.h>
#endif




namespace Thread
{


void waitOnMutex(std::condition_variable &waitCond, std::mutex &waitMutex)
{
	std::unique_lock<std::mutex> lock(waitMutex);
	waitCond.wait(lock);
}

//! Returns true if timeout hit
bool timedWaitOnMutex(std::condition_variable &waitCond, std::mutex &waitMutex, int secs, int msecs)
{
	auto span = std::chrono::seconds(secs) + std::chrono::milliseconds(msecs);

	std::unique_lock<std::mutex> lock(waitMutex);
	return waitCond.wait_for(lock, span) == std::cv_status::timeout;
}


class ReadWriteMutex::ReadWriteMutexPrivates
{
public:
	Thread::Internal::ReadWriteMutex m_Mutex;
};


ReadWriteMutex::ReadWriteMutex()
{
	m_pPrivates = new ReadWriteMutexPrivates();
}

ReadWriteMutex::~ReadWriteMutex()
{
	safe_delete(m_pPrivates);
}

void ReadWriteMutex::readLock()
{
	m_pPrivates->m_Mutex.acquireReadLock();
}

void ReadWriteMutex::readUnlock()
{
	m_pPrivates->m_Mutex.releaseReadLock();
}

void ReadWriteMutex::writeLock()
{
	m_pPrivates->m_Mutex.acquireWriteLock();
}

void ReadWriteMutex::writeUnlock()
{
	m_pPrivates->m_Mutex.releaseWriteLock();
}




class Mutex::MutexPrivates
{
public:
	std::mutex m_WaitMutex;
};


Mutex::Mutex()
{
	m_pPrivates = new MutexPrivates();
}

Mutex::~Mutex()
{
	safe_delete(m_pPrivates);
}

void Mutex::lock()
{
	if (m_pPrivates)
		m_pPrivates->m_WaitMutex.lock();
}

void Mutex::unlock()
{
	if (m_pPrivates)
		m_pPrivates->m_WaitMutex.unlock();
}



class WaitCondition::WaitConditionPrivates
{
public:
	bool checkNotify()
	{
		std::lock_guard<std::mutex> guard(m_NotifyLock);

		if (!m_bNotify)
			return false;

		m_bNotify = false;
		return true;
	}

	void setNotify(bool state)
	{
		std::lock_guard<std::mutex> guard(m_NotifyLock);

		m_bNotify = state;

		if (state)
			m_WaitCond.notify_all();
	}

	std::condition_variable m_WaitCond;
	std::mutex m_WaitMutex;
	
private:
	std::mutex m_NotifyLock;
	volatile bool m_bNotify = false;
};


WaitCondition::WaitCondition()
	: m_pPrivates(new WaitConditionPrivates())
{
}

WaitCondition::~WaitCondition()
{
	safe_delete(m_pPrivates);
}

void WaitCondition::wait()
{
	while (wait(0, 500))
	{
	}
}

bool WaitCondition::wait(int sec, int msec)
{
	if (m_pPrivates->checkNotify())
		return false;

	bool res = timedWaitOnMutex(m_pPrivates->m_WaitCond, m_pPrivates->m_WaitMutex, sec, msec);

	if (m_pPrivates->checkNotify())
		res = false;

	return res;
}

void WaitCondition::notify()
{
	m_pPrivates->setNotify(true);
}














class BaseThread::ThreadPrivates
{
public:
	ThreadPrivates(const char* szName)
		: m_szName(gcString(szName))
	{
	}

	~ThreadPrivates()
	{
		safe_delete(m_pThread);
	}

	const std::string m_szName;

	bool m_bIsRunning = false;
	volatile bool m_bPause = false;
	volatile bool m_bStop = false;

	BaseThread::PRIORITY m_uiPriority = NORMAL;

	std::thread *m_pThread = nullptr;
	std::condition_variable m_PauseCond;
	std::mutex m_PauseMutex;
	std::recursive_mutex m_PauseInitMutex;
};



BaseThread::BaseThread(const char* name)
	: m_pPrivates(new ThreadPrivates(name))
{
}

BaseThread::~BaseThread()
{
	stop();
	safe_delete(m_pPrivates);
}

const char* BaseThread::getName()
{
	return m_pPrivates->m_szName.c_str();
}

void BaseThread::start()
{
	if (m_pPrivates->m_pThread)
		return;

	auto waitCond = std::make_shared<WaitCondition>();

	m_pPrivates->m_pThread = new std::thread([this, waitCond](){

		waitCond->wait();
		assert(m_pPrivates->m_pThread);

		m_pPrivates->m_bIsRunning = true;
		applyPriority();
		setThreadName();
		run();	
	});

	waitCond->notify();
}

bool BaseThread::isRunning()
{
	return m_pPrivates->m_bIsRunning;
}

volatile bool BaseThread::isStopped()
{
	return m_pPrivates->m_bStop;
}

volatile bool BaseThread::isPaused()
{
	return (!m_pPrivates->m_bStop && m_pPrivates->m_bPause);
}

void BaseThread::doPause()
{
	if (isPaused())
		waitOnMutex(m_pPrivates->m_PauseCond, m_pPrivates->m_PauseMutex);
}

void BaseThread::pause()
{
	std::lock_guard<std::recursive_mutex> guard(m_pPrivates->m_PauseInitMutex);

	if (m_pPrivates->m_bPause)
		return;

	m_pPrivates->m_bPause = true;
	onPause();
}

void BaseThread::unpause()
{
	std::lock_guard<std::recursive_mutex> guard(m_pPrivates->m_PauseInitMutex);

	if (!m_pPrivates->m_bPause)
		return;

	m_pPrivates->m_bPause = false;
	onUnpause();
	m_pPrivates->m_PauseCond.notify_all();
}

void BaseThread::stop()
{
	unpause();
	nonBlockStop();

	auto thread = m_pPrivates->m_pThread;

	if (thread && thread->joinable())
		thread->join();
}

void BaseThread::nonBlockStop()
{
	if (!m_pPrivates->m_pThread)
		return;

	m_pPrivates->m_bStop = true;
	onStop();
}

void BaseThread::join()
{
	auto thread = m_pPrivates->m_pThread;

	if (!thread)
		return;

	if (thread->joinable())
		thread->join();
}

void BaseThread::setPriority(PRIORITY priority)
{
	m_pPrivates->m_uiPriority = priority;
}

void BaseThread::applyPriority()
{

#ifdef WIN32
	if (!m_pPrivates->m_pThread)
		return;

	BOOL res;
	HANDLE th = GetCurrentThread();

	switch (m_pPrivates->m_uiPriority)
	{
	case REALTIME		: res = SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);	break;
	case HIGH			: res = SetThreadPriority(th, THREAD_PRIORITY_HIGHEST);			break;
	case ABOVE_NORMAL	: res = SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);	break;
	case NORMAL			: res = SetThreadPriority(th, THREAD_PRIORITY_NORMAL);			break;
	case BELOW_NORMAL	: res = SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);	break;
	case IDLE			: res = SetThreadPriority(th, THREAD_PRIORITY_LOWEST);			break;
	}
#endif
}

#if defined(WIN32) && !defined(__MINGW32__)
typedef struct tagTHREADNAME_INFO
{
	DWORD dwType; // must be 0x1000
	LPCSTR szName; // pointer to name (in user addr space)
	DWORD dwThreadID; // thread ID (-1=caller thread)
	DWORD dwFlags; // reserved for future use, must be zero
} THREADNAME_INFO;

static void ThrowException(THREADNAME_INFO &info)
{
   // what the hell is happening here?
	__try
	{
		RaiseException( MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(DWORD), (DWORD*)&info );
	}
	__except(EXCEPTION_CONTINUE_EXECUTION)
	{
	}
}
#endif

void BaseThread::setThreadName(const char* nameOveride)
{
	if (!nameOveride)
		nameOveride = m_pPrivates->m_szName.c_str();

#if defined(WIN32) && !defined(__MINGW32__)
	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = nameOveride;
	info.dwThreadID = (DWORD)GetCurrentThreadId();;
	info.dwFlags = 0;

	gcString strThreadName("Thread 0x{0:x} name is \"{1}\"\n", GetCurrentThreadId(), nameOveride);
	OutputDebugStringA(strThreadName.c_str());

	ThrowException(info);
#else
	char name[16];
	strncpy(name, nameOveride, 15);
		
	name[15] = '\0'; 
	prctl(PR_SET_NAME, name, 0, 0, 0);
#endif
}


uint64 BaseThread::GetCurrentThreadId()
{
#ifdef WIN32
	return ::GetCurrentThreadId();
#else
	return (uint64)pthread_self();
#endif
}

}
