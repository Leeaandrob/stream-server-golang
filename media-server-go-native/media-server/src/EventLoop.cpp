#include "EventLoop.h"

#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>

#include "log.h"

size_t EventLoop::MaxSendingQueueSize = 16*1024;


#if __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#else
#include <sys/eventfd.h>
cpu_set_t* alloc_cpu_set(size_t* size) {
	// the CPU set macros don't handle cases like my Azure VM, where there are 2 cores, but 128 possible cores (why???)
	// hence requiring an oversized 16 byte cpu_set_t rather than the 8 bytes that the macros assume to be sufficient.
	// this is the only way (even documented as such!) to figure out how to make a packet big enough
	unsigned long* packet = nullptr;
	int len = 0;
	do {
		++len;
		delete [] packet;
		packet = new unsigned long[len];
	} while(pthread_getaffinity_np(pthread_self(), len * sizeof(unsigned long), reinterpret_cast<cpu_set_t*>(packet)) == EINVAL);

	*size = len * sizeof(unsigned long);
	return reinterpret_cast<cpu_set_t*>(packet);
}

void free_cpu_set(cpu_set_t* s) {
	delete [] reinterpret_cast<unsigned long*>(s);
}
#endif

EventLoop::EventLoop(Listener *listener) :
	listener(listener)
{
}

EventLoop::~EventLoop()
{
	Stop();
}

bool EventLoop::SetAffinity(int cpu)
{
#ifdef THREAD_AFFINITY_POLICY
	if (cpu>=0)
	{
		thread_affinity_policy_data_t policy = { cpu+1 };
		return !thread_policy_set(pthread_mach_thread_np(thread.native_handle()), THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, 1);
	} else  {
		thread_affinity_policy_data_t policy = { 0 };
		return !thread_policy_set(pthread_mach_thread_np(thread.native_handle()), THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, 1);
	}
	
#else
	size_t cpuSize = 0;
	cpu_set_t* cpuSet = alloc_cpu_set(&cpuSize);
	CPU_ZERO_S(cpuSize, cpuSet);

	//Set affinity to the cpu core
	if (cpu>=0)
		//Set mask
		CPU_SET(cpu, cpuSet);
	else
		//Set affinity for all cpus
		for (size_t j=0; j<cpuSize ; j++)
			CPU_SET(j, cpuSet);

	//Set thread affinity
        bool ret =  !pthread_setaffinity_np(thread.native_handle(), cpuSize, cpuSet);
	
	//Clear cpu mask
	free_cpu_set(cpuSet);
	
	//Done
	return ret;
#endif
}

bool EventLoop::Start(std::function<void(void)> loop)
{
	//If already started
	if (thread.get_id()!=std::thread::id())
		//Alredy running
		return false;
#if __APPLE__
	//Create pipe
	if (::pipe(pipe)==-1)
		//Error
		return false;
	
	//Set non blocking
	fcntl(pipe[0], F_SETFL , O_NONBLOCK);
	fcntl(pipe[1], F_SETFL , O_NONBLOCK);
	
#else
	pipe[0] = pipe[1] = eventfd(0, EFD_NONBLOCK);
#endif	
	
	//Store socket
	this->fd = -1;
	
	//Running
	running = true;
	
	//Start thread and run
	thread = std::thread(loop);
	
	//Done
	return true;
}

bool EventLoop::Start(int fd)
{
	//If already started
	if (thread.get_id()!=std::thread::id())
		//Alredy running
		return false;
	
#if __APPLE__
	//Create pipe
	if (::pipe(pipe)==-1)
		//Error
		return false;
	
	//Set non blocking
	fcntl(pipe[0], F_SETFL , O_NONBLOCK);
	fcntl(pipe[1], F_SETFL , O_NONBLOCK);
	
#else
	pipe[0] = pipe[1] = eventfd(0, EFD_NONBLOCK);
#endif	
	
	//Store socket
	this->fd = fd;
	
	//Running
	running = true;
	
	//Start thread and run
	thread = std::thread([this](...){ Run(); });
	
	//Done
	return true;
}

bool EventLoop::Stop()
{
	//Check if running
	if (!running)
		//Nothing to do
		return false;
	
	//Not running
	running = false;
	
	//If it was not external
	if (thread.joinable())
	{
		//Signal the thread this will cause the poll call to exit
		Signal();

		//Nulifi thread
		thread.join();
	}
	
	//Close pipe
	close(pipe[0]);
	close(pipe[1]);
	
	//Empyt pipe
	pipe[0] = pipe[1] = 0;
	
	//Done
	return true;
	
}

void EventLoop::Send(const uint32_t ipAddr, const uint16_t port, Packet&& packet)
{
	//Get approximate queued size
	auto aprox = sending.size_approx();
	
	//Check if there is too much in the queue already
	if (aprox>MaxSendingQueueSize)
	{
		//Check state
		if (state!=State::Overflown)
		{
			//We are overflowing
			state = State::Overflown;
			//Log
			Error("-EventLoop::Send() | sending queue overflown [aprox:%u]\n",aprox);
		}
		//Do not enqueue more
		return;
	} else if (aprox>MaxSendingQueueSize/2 && state==State::Normal) {
		//We are lagging behind
		state = State::Lagging;
		//Log
		Error("-EventLoop::Send() | sending queue lagging behind [aprox:%u]\n",aprox);
	} else if (aprox<MaxSendingQueueSize/4 && state!=State::Normal)  {
		//We are lagging behind
		state = State::Lagging;
		//Log
		Log("-EventLoop::Send() | sending queue back to normal [aprox:%u]\n",aprox);
	}
	
	//Create send packet
	SendBuffer send = {ipAddr, port, std::move(packet)};
	
	//Move it back to sending queue
	sending.enqueue(std::move(send));
	
	//Signal the thread this will cause the poll call to exit
	Signal();
}

std::future<void> EventLoop::Async(std::function<void(std::chrono::milliseconds)> func)
{
	//UltraDebug(">EventLoop::Async()\n");
	
	//Create task
	std::promise<void> promise;
	auto task = std::make_pair(std::move(promise),std::move(func));
	
	//Get future before moving the promise
	auto future = task.first.get_future();
	
	//If not in the same thread
	if (std::this_thread::get_id()!=thread.get_id())
	{
		//Add to pending taks
		tasks.enqueue(std::move(task));

		//Signal the thread this will cause the poll call to exit
		Signal();
	} else {
		//Call now otherwise
		task.second(GetNow());
		
		//Resolve the promise
		task.first.set_value();
	}
	
	//UltraDebug("<EventLoop::Async()\n");
	
	//Return the future for the promise
	return future;
}

Timer::shared EventLoop::CreateTimer(std::function<void(std::chrono::milliseconds)> callback)
{
	//Create timer without scheduling it
	auto timer = std::make_shared<TimerImpl>(*this,0ms,callback);
	//Done
	return std::static_pointer_cast<Timer>(timer);
}

Timer::shared EventLoop::CreateTimer(const std::chrono::milliseconds& ms, std::function<void(std::chrono::milliseconds)> callback)
{
	//Timer without repeat
	return CreateTimer(ms,0ms,callback);
}

Timer::shared EventLoop::CreateTimer(const std::chrono::milliseconds& ms, const std::chrono::milliseconds& repeat, std::function<void(std::chrono::milliseconds)> callback)
{
	//Create timer
	auto timer = std::make_shared<TimerImpl>(*this,repeat,callback);
	
	//Get next
	auto next = this->GetNow() + ms;
	
	//Add it async
	Async([this,timer,next](...){
		//Set next tick
		timer->next = next;

		//Add to timer list
		timers.emplace(next, timer);
	});
	
	//Done
	return std::static_pointer_cast<Timer>(timer);
}

void EventLoop::TimerImpl::Cancel()
{
	//Add it async
	loop.Async([timer = shared_from_this()](...){
		//Remove us
		timer->loop.CancelTimer(timer);
	});
}

void EventLoop::TimerImpl::Again(const std::chrono::milliseconds& ms)
{
	//UltraDebug(">EventLoop::Again() | Again triggered in %u\n",ms.count());
	
	//Get next
	auto next = loop.GetNow() + ms;
	
	//Reschedule it async
	loop.Async([timer = shared_from_this(),next](...){
		//Remove us
		timer->loop.CancelTimer(timer);

		//Set next tick
		timer->next = next;
		
		//Add to timer list
		timer->loop.timers.emplace(next, timer);
	});
	
	//UltraDebug("<EventLoop::Again() | timer triggered at %llu\n",next.count());
}

void EventLoop::CancelTimer(TimerImpl::shared timer)
{
	//Asert we are on same thread than loop
	AssertThread();
	
	//UltraDebug(">EventLoop::CancelTimer() \n");

	//We don't have to repeat this
	timer->repeat = 0ms;
	
	//If not scheduled
	if (!timer->next.count())
		//Nothing
		return;
	
	//Get all timers at that tick
	auto result = timers.equal_range(timer->next);
	
	//Reset next tick
	timer->next = 0ms;

	// Iterate over the range
	for (auto it = result.first; it != result.second; ++it)
	{
		//If it is the same impl
		if (it->second.get()==timer.get())
		{
			//Remove this one
			timers.erase(it);
			//Found
			break;
		}
	}
	//UltraDebug("<EventLoop::CancelTimer() \n");
}

const std::chrono::milliseconds EventLoop::Now()
{
	//Get new now and store in cache
	return now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
}

void EventLoop::Signal()
{
	//UltraDebug("-EventLoop::Signal()\r\n");
	uint64_t one = 1;
	
	//If we are in the same thread or already signaled
	if (std::this_thread::get_id()==thread.get_id() || signaled) 
		//No need to do anything
		return;
	
	//We have signaled it
	//worst case scenario is that race happens between this to points
	//and that we signal it twice
	signaled = true;
	
	//Write to tbe pipe, and assign to one to avoid warning in compile time
	one = write(pipe[1],(uint8_t*)&one,sizeof(one));
}

void EventLoop::Run(const std::chrono::milliseconds &duration)
{
	//Log(">EventLoop::Run() | [%p,running:%d,duration:%llu]\n",this,running,duration.count());
	
	//Recv data
	uint8_t data[MTU] ZEROALIGNEDTO32;
	size_t  size = MTU;
	sockaddr_in from;
	memset(&from,0,sizeof(sockaddr_in));
	
	//Pending data
	SendBuffer item;
	bool pending = false;
	
	//Set values for polling
	ufds[0].fd = fd;
	ufds[1].fd = pipe[0];
	//Set to wait also for write events on pipe for signaling
	ufds[1].events = POLLIN | POLLERR | POLLHUP;

	//Set non blocking so we can get an error when we are closed by end
	int fsflags = fcntl(fd,F_GETFL,0);
	fsflags |= O_NONBLOCK;
	fcntl(fd,F_SETFL,fsflags);
	
	//Catch all IO errors and do nothing
	signal(SIGIO,[](int){});
	
	//Get now
	auto now = Now();
	
	//calculate until when
	auto until = duration < std::chrono::milliseconds::max() ? now + duration : std::chrono::milliseconds::max();
		
	//Run until ended
	while(running && now<=until)
	{
		//If we have anything to send set to wait also for write events
		ufds[0].events = sending.size_approx() ? POLLIN | POLLOUT | POLLERR | POLLHUP : POLLIN | POLLERR | POLLHUP;
		
		//Until signaled
		int timeout = -1;
		
		//Check if we have any pending task to wait or exit poll inmediatelly
		if (tasks.size_approx()) 
		{
			//No wait
			timeout = 0;
		}
		//If we have any timer or a timeout
		else if (timers.size())
		{
			//Get first timer in  queue
			auto next = std::min(timers.begin()->first,until);
			//Override timeout
			timeout = next > now ? std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count() : 0;
		} 
		//If we have a maximum duration
		else if (duration!=std::chrono::milliseconds::max()) 
		{
			//Override timeout
			timeout = until > now ? std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count() : 0;
		}

		//UltraDebug(">EventLoop::Run() | poll timeout:%d timers:%d tasks:%d\n",timeout,timers.size(),tasks.size_approx());
		
		//Wait for events
		poll(ufds,sizeof(ufds)/sizeof(pollfd),timeout);
		
		//Update now
		now = Now();
		
		//UltraDebug("<EventLoop::Run() | poll timeout:%d timers:%d tasks:%d\n",timeout,timers.size(),tasks.size_approx());
		
		//Check for cancel
		if ((ufds[0].revents & POLLHUP) || (ufds[0].revents & POLLERR) || (ufds[1].revents & POLLHUP) || (ufds[1].revents & POLLERR))
		{
			//Error
			Log("-EventLoop::Run() | Pool error event [%d]\n",ufds[0].revents);
			//Exit
			break;
		}
		
		//Read first
		if (ufds[0].revents & POLLIN)
		{
			//UltraDebug("-EventLoop::Run() | ufds[0].revents & POLLIN\n");
			//Len
			uint32_t fromLen = sizeof(from);
			//Leemos del socket
			int len = recvfrom(fd,data,size,MSG_DONTWAIT,(sockaddr*)&from,&fromLen);
			//Run callback
			if (listener) listener->OnRead(ufds[0].fd,data,len,ntohl(from.sin_addr.s_addr),ntohs(from.sin_port));
		}
		
		//Check read is possible
		if (ufds[0].revents & POLLOUT)
		{
			//UltraDebug("-EventLoop::Run() | ufds[0].revents & POLLOUT\n");
			
			//Check if we had any pending item
			if (!pending)
				//Get first
				pending = sending.try_dequeue(item);
			
			//Now send all that we can
			while (pending)
			{
				//Create send address
				sockaddr_in to;
				memset(&to,0,sizeof(sockaddr_in));
				to.sin_family	   = AF_INET;
				to.sin_addr.s_addr = htonl(item.ipAddr);
				to.sin_port	   = htons(item.port);

				//Send it
				int ret = sendto(fd,item.packet.GetData(),item.packet.GetSize(),MSG_DONTWAIT,(sockaddr*)&to,sizeof(to));
				
				//It we have an error
				if (ret<0 && state==State::Normal)
				{
					//Do we need to retry current packet?
					pending = errno==EAGAIN || errno==EWOULDBLOCK;
					//Retry again later
					break;
				}
				//Get next item
				pending = sending.try_dequeue(item);
			}
			
		}
		
		//Run queued task
		std::pair<std::promise<void>,std::function<void(std::chrono::milliseconds)>> task;
		//Get all pending taks
		while (tasks.try_dequeue(task))
		{
			//UltraDebug("-EventLoop::Run() | task pending\n");
			//Execute it
			task.second(now);
			//Resolce promise
			task.first.set_value();
		}

		//Timers triggered
		std::vector<TimerImpl::shared> triggered;
		//Get all timers to process in this lop
		for (auto it = timers.begin(); it!=timers.end(); )
		{
			//Check it we are still on the time
			if (it->first>now)
				//It is yet to come
				break;
			//Get timer
			triggered.push_back(std::move(it->second));
			//Remove from the list
			it = timers.erase(it);
		}

		//Now process all timers triggered
		for (auto timer : triggered)
		{
			//UltraDebug("-EventLoop::Run() | timer triggered at ll%u\n",now.count());
			//We are executing
			timer->next = 0ms;
			//Execute it
			timer->callback(now);
			//If we have to reschedule it again
			if (timer->repeat.count() && !timer->next.count())
			{
				//Set next
				timer->next = now + timer->repeat;
				//Schedule
				timers.emplace(timer->next, timer);
			}
		}
		
		//Read first from signal pipe
		if (ufds[1].revents & POLLIN)
		{
			//Remove pending data
			while (read(pipe[0],data,size)>0) 
			{
				//DO nothing
			}
			//We are not signaled anymore
			signaled = false;
		}
		
		//Update now
		now = Now();
	}
	
	//Run queued task
	std::pair<std::promise<void>,std::function<void(std::chrono::milliseconds)>> task;
	//Get all pending taks
	while (tasks.try_dequeue(task))
	{
		//UltraDebug("-EventLoop::Run() | task pending\n");
		//Update now
		auto now = Now();
		//Execute it
		task.second(now);
		//Resolce promise
		task.first.set_value();
	}

	//Log("<EventLoop::Run()\n");
}
