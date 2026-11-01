// Guest registers remain serialized; blocked native threads release the machine.
struct GuestThread {
    pthread_t native{};
    uint32_t id{},handle{},start{},parameter{},stack{},tib{},exitCode=259,suspended{};
    int32_t priority=0;
    bool started=false,done=false;
    bool timer=false;
};
struct GuestTimer {
    uint32_t id,delay,callback,parameter;bool periodic,cancelled=false,running=false,synchronous=false;
    std::chrono::steady_clock::time_point due;
};
std::map<uint32_t,std::shared_ptr<GuestTimer>> timerHandles;
std::shared_ptr<GuestThread> timerWorker;uint32_t nextTimerId=1;
thread_local uint32_t runningTimerId=0;
void runGuestTimers();
std::vector<std::shared_ptr<GuestThread>> guestThreads;
std::map<uint32_t,std::shared_ptr<GuestThread>> threadHandles;
uint32_t nextThreadId=2,nextThreadHandle=0xf0030000;
struct GuestEvent {bool manual,signaled;};
std::map<uint32_t,std::shared_ptr<GuestEvent>> eventHandles;
std::map<std::string,std::weak_ptr<GuestEvent>> namedEvents;
uint32_t nextEventHandle=0xf0040000;
struct GuestMutex {uint32_t owner=0,depth=0;bool abandoned=false;};
std::map<uint32_t,std::shared_ptr<GuestMutex>> mutexHandles;
std::map<std::string,std::weak_ptr<GuestMutex>> namedMutexes;
uint32_t nextMutexHandle=0xf0080000;
// Win32 mutexes run without the guest machine (R36S parallel guest threads): the game's sound thread takes
// and releases its mixer mutex ~600 times a second, and each call used to wait for the machine while the
// main thread held it, sometimes while holding that very mutex. The main thread then blocked on the mutex
// for ~30% of every race frame, and the sound fell behind (echo). guestMutexState guards mutexHandles,
// namedMutexes and every GuestMutex; waiters sleep on the guest sync condition (notifyGuestSync), which is
// never notified with guestMutexState held (lock order: guestSyncMutex may be taken inside, not outside).
std::mutex guestMutexState;
std::shared_ptr<GuestMutex> findGuestMutex(uint32_t handle){
    std::lock_guard<std::mutex> l(guestMutexState);auto it=mutexHandles.find(handle);return it==mutexHandles.end()?nullptr:it->second;
}
bool eraseGuestMutex(uint32_t handle){std::lock_guard<std::mutex> l(guestMutexState);return mutexHandles.erase(handle)>0;}
// WAIT_OBJECT_0 (0), WAIT_ABANDONED (128) or WAIT_TIMEOUT (258).
uint32_t acquireGuestMutex(const std::shared_ptr<GuestMutex>& value,uint32_t timeout){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
    for(;;){
        const uint64_t gen=guestSyncGeneration.load();
        {
            std::lock_guard<std::mutex> l(guestMutexState);
            if(!value->owner||value->owner==guestThreadId){
                __atomic_store_n(&value->owner,guestThreadId,__ATOMIC_RELEASE);++value->depth;
                const bool abandoned=value->abandoned;value->abandoned=false;return abandoned?128:0;
            }
        }
        if(timeout!=UINT32_MAX&&std::chrono::steady_clock::now()>=deadline)return 258;
        checkGuestProgress();
        waitGuestSync(gen,timeout==0?0:4,[&]{return __atomic_load_n(&value->owner,__ATOMIC_ACQUIRE)==0;});
    }
}
void createMutex(){
    auto name=guestString(arg(2));std::shared_ptr<GuestMutex> value;
    std::lock_guard<std::mutex> l(guestMutexState);
    if(!name.empty())value=namedMutexes[name].lock();
    if(!name.empty()&&namedEvents.count(name)&&!namedEvents[name].expired()){lastError=6;ret(0,3);return;}
    lastError=value?183:0;
    if(!value){value=std::make_shared<GuestMutex>();if(arg(1)){value->owner=guestThreadId;value->depth=1;}if(!name.empty())namedMutexes[name]=value;}
    uint32_t handle=nextMutexHandle++;mutexHandles.emplace(handle,value);ret(handle,3);
}
void releaseMutex(){
    bool released=false;
    {
        std::lock_guard<std::mutex> l(guestMutexState);
        auto value=mutexHandles.find(arg(0));if(value==mutexHandles.end()){lastError=6;ret(0,1);return;}
        if(value->second->owner!=guestThreadId){lastError=288;ret(0,1);return;}
        if(!--value->second->depth){__atomic_store_n(&value->second->owner,0u,__ATOMIC_RELEASE);released=true;}
    }
    if(released)notifyGuestSync();
    ret(1,1);
}
void createEvent(){
    auto name=guestString(arg(3));std::shared_ptr<GuestEvent> event;
    if(!name.empty()&&namedMutexes.count(name)&&!namedMutexes[name].expired()){lastError=6;ret(0,4);return;}
    if(!name.empty())event=namedEvents[name].lock();
    lastError=event?183:0;
    if(!event){event=std::make_shared<GuestEvent>(GuestEvent{arg(1)!=0,arg(2)!=0});if(!name.empty())namedEvents[name]=event;}
    uint32_t handle=nextEventHandle++;eventHandles.emplace(handle,event);ret(handle,4);
}
void setEvent(){auto event=eventHandles.find(arg(0));if(event==eventHandles.end()){lastError=6;ret(0,1);return;}event->second->signaled=true;notifyGuestSync();ret(1,1);}
void resetEvent(){auto event=eventHandles.find(arg(0));if(event==eventHandles.end()){lastError=6;ret(0,1);return;}event->second->signaled=false;ret(1,1);}
// ExitThread unwinds the lifted frames back to guestThreadEntry.
struct GuestThreadExit{uint32_t code;};
thread_local bool insideGuestThread=false;
void exitThread(){
    if(!insideGuestThread)throw GuestStop("ExitThread en el hilo principal desde "+hex(g_cur_func));
    throw GuestThreadExit{arg(0)};
}
void* guestThreadEntry(void* context){
    insideGuestThread=true;
    auto& thread=*static_cast<GuestThread*>(context);
    // Name host threads after the guest thread and its entry point, so per-thread CPU reports and
    // profiles (R36S [NFSU2 PERF] lines) show which guest code each one runs.
    {char name[16];std::snprintf(name,sizeof(name),"g%u-%06x",thread.id,thread.timer?0u:thread.start&0xffffffu);pthread_setname_np(pthread_self(),name);}
    std::unique_lock<MachineMutex> lease(machine);machineLease=&lease;guestThreadId=thread.id;
    freeRunning=!thread.timer&&std::find(freeThreadEntries.begin(),freeThreadEntries.end(),thread.start)!=freeThreadEntries.end();
    if(freeRunning)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest thread %u (entry %08x) runs in parallel",thread.id,thread.start);
    State fresh;fresh.esp=thread.stack+(4u<<20)-64;fresh.fs=thread.tib;fresh.load();
    try {
        while(thread.suspended){uint64_t gen=guestSyncGeneration.load();checkGuestProgress();if(thread.suspended)waitGuestSync(gen,5);}
        if(thread.timer){
            runGuestTimers();
            thread.exitCode=0;
        }else thread.exitCode=callGuest(thread.start,{thread.parameter},0,true);
    }catch(const GuestThreadExit& exit){
        thread.exitCode=exit.code;
    }catch(const GuestStop& error){
        thread.exitCode=1;
        if(!stopRequested){guestThreadFault=error.what();NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Guest thread %u: %s",thread.id,error.what());}
    }catch(const std::exception& error){thread.exitCode=1;guestThreadFault=error.what();}
    heaps->free(GuestHeaps::process,thread.stack);heaps->free(GuestHeaps::process,thread.tib);
    {std::lock_guard<std::mutex> l(guestMutexState);
     for(auto& value:mutexHandles)if(value.second->owner==thread.id){__atomic_store_n(&value.second->owner,0u,__ATOMIC_RELEASE);value.second->depth=0;value.second->abandoned=true;}}
    forgetThreadTrace();thread.done=true;notifyGuestSync();machineLease=nullptr;return nullptr;
}
void reapGuestThreads(){
    for(auto it=guestThreads.begin();it!=guestThreads.end();){
        auto& thread=*it;
        if(thread->done){if(thread->started){pthread_join(thread->native,nullptr);thread->started=false;}it=guestThreads.erase(it);}
        else ++it;
    }
}
void createThread(){
    reapGuestThreads();
    uint32_t flags=arg(4),idOut=arg(5);
    if(flags&~4u){lastError=87;ret(0,6);return;}
    auto thread=std::make_shared<GuestThread>();thread->id=nextThreadId++;thread->handle=nextThreadHandle++;
    thread->start=arg(2);thread->parameter=arg(3);thread->suspended=flags&4?1:0;
    if(!recomp_lookup(thread->start)){lastError=87;ret(0,6);return;}
    thread->stack=allocate(4u<<20);thread->tib=allocate(4096);
    write32(thread->tib,UINT32_MAX);write32(thread->tib+4,thread->stack+(4u<<20));write32(thread->tib+8,thread->stack);
    write32(thread->tib+0x18,thread->tib);write32(thread->tib+0x20,1);write32(thread->tib+0x24,thread->id);
    pthread_attr_t attributes;int error=pthread_attr_init(&attributes);
    if(!error){error=pthread_attr_setstacksize(&attributes,64u<<20);if(!error)error=pthread_create(&thread->native,&attributes,guestThreadEntry,thread.get());pthread_attr_destroy(&attributes);}
    if(error){heaps->free(GuestHeaps::process,thread->stack);heaps->free(GuestHeaps::process,thread->tib);lastError=8;ret(0,6);return;}
    thread->started=true;guestThreads.push_back(thread);threadHandles.emplace(thread->handle,thread);
    if(idOut)write32(idOut,thread->id);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Created guest thread %u at %08x suspended=%u",thread->id,thread->start,thread->suspended);
    ret(thread->handle,6);
}
void resumeThread(){auto thread=threadHandles.find(arg(0));if(thread==threadHandles.end()){lastError=6;ret(UINT32_MAX,1);return;}uint32_t old=thread->second->suspended;if(old)--thread->second->suspended;notifyGuestSync();ret(old,1);}
void setThreadPriority(){
    uint32_t handle=arg(0);int32_t priority=static_cast<int32_t>(arg(1));
    if(priority< -2&&priority!= -15){lastError=87;ret(0,2);return;}if(priority>2&&priority!=15){lastError=87;ret(0,2);return;}
    auto thread=threadHandles.find(handle);
    if(thread==threadHandles.end()){lastError=6;ret(0,2);return;}
    thread->second->priority=priority;ret(1,2);
}
void threadExitCode(){auto thread=threadHandles.find(arg(0));if(thread==threadHandles.end()){lastError=6;ret(0,2);return;}write32(arg(1),thread->second->exitCode);ret(1,2);}
void waitThread(){
    uint32_t handle=arg(0),timeout=arg(1);
    WaitScope scope(mutexHandles.count(handle)?"WaitForSingleObject(mutex)":eventHandles.count(handle)?"WaitForSingleObject(event)":"WaitForSingleObject(thread)",handle);
    if(auto mutex=findGuestMutex(handle)){
        const bool timeIt=nfsIsGameThread();const auto t0=std::chrono::steady_clock::now();
        const uint32_t result=acquireGuestMutex(mutex,timeout);
        if(timeIt)nfsWaitAdd(8,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
        ret(result,2);return;
    }
    auto found=threadHandles.find(handle);
    auto eventFound=eventHandles.find(handle);
    if(found==threadHandles.end()&&eventFound==eventHandles.end()){lastError=6;ret(UINT32_MAX,2);return;}
    auto thread=found==threadHandles.end()?nullptr:found->second;
    auto event=eventFound==eventHandles.end()?nullptr:eventFound->second;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
    // Diagnostics (NFSU2_HANG_DUMP): report waits that last more than 8 s.
    static const bool reportLongWaits=std::getenv("NFSU2_HANG_DUMP")!=nullptr;
    auto waitStart=std::chrono::steady_clock::now();bool reported=false;
    struct WaitTimer{std::chrono::steady_clock::time_point t0=std::chrono::steady_clock::now();bool on=nfsIsGameThread();
        ~WaitTimer(){if(on)nfsWaitAdd(8,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());}} waitTimer;
    for(uint64_t gen=guestSyncGeneration.load();thread?!thread->done:!event->signaled;gen=guestSyncGeneration.load()){if(timeout!=UINT32_MAX&&std::chrono::steady_clock::now()>=deadline){ret(258,2);return;}
        if(reportLongWaits&&!reported&&std::chrono::steady_clock::now()-waitStart>std::chrono::seconds(8)){reported=true;
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","LONG WAIT guest thread %u on %s %08x (manual=%d) timeout=%u",guestThreadId,thread?"thread":"event",handle,event?int(event->manual):-1,timeout);}
        checkGuestProgress();if(thread?!thread->done:!event->signaled)waitGuestSync(gen,timeout==0?0:4,[&]{return thread?thread->done:event->signaled;});}
    if(event&&!event->manual)event->signaled=false;
    ret(0,2);
}
// WaitForSingleObject for free-running threads (no machine held): mutexes directly, anything else (events,
// threads) under the machine as before.
[[maybe_unused]] void waitSingleFree(){
    if(auto mutex=findGuestMutex(arg(0))){ret(acquireGuestMutex(mutex,arg(1)),2);return;}
    machine.lock();
    try{waitThread();}catch(...){machine.unlock();throw;}
    machine.unlock();
}
// WaitForMultipleObjects over the same thread, event and mutex objects as WaitForSingleObject.
bool waitableReady(uint32_t handle){
    if(auto mutex=mutexHandles.find(handle);mutex!=mutexHandles.end())return !mutex->second->owner||mutex->second->owner==guestThreadId;
    if(auto event=eventHandles.find(handle);event!=eventHandles.end())return event->second->signaled;
    return threadHandles.at(handle)->done;
}
bool acquireWaitable(uint32_t handle){  // true when an abandoned mutex was taken
    if(auto mutex=mutexHandles.find(handle);mutex!=mutexHandles.end()){
        auto& value=*mutex->second;value.owner=guestThreadId;++value.depth;bool abandoned=value.abandoned;value.abandoned=false;return abandoned;
    }
    if(auto event=eventHandles.find(handle);event!=eventHandles.end()&&!event->second->manual)event->second->signaled=false;
    return false;
}
void waitMultiple(){
    uint32_t count=arg(0),list=arg(1),timeout=arg(3);bool all=arg(2)!=0;
    if(!count||count>64||!list){lastError=87;ret(UINT32_MAX,4);return;}
    std::vector<uint32_t> handles(count);
    for(uint32_t i=0;i<count;++i){
        handles[i]=read32(list+i*4);
        if(!mutexHandles.count(handles[i])&&!eventHandles.count(handles[i])&&!threadHandles.count(handles[i])){lastError=6;ret(UINT32_MAX,4);return;}
    }
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
    WaitScope scope(all?"WaitForMultipleObjects(all)":"WaitForMultipleObjects(any)",handles[0]);
    for(;;){
        if(all){
            if(std::all_of(handles.begin(),handles.end(),waitableReady)){
                bool abandoned=false;for(uint32_t handle:handles)abandoned|=acquireWaitable(handle);
                ret(abandoned?128:0,4);return;
            }
        }else for(uint32_t i=0;i<count;++i)if(waitableReady(handles[i])){ret((acquireWaitable(handles[i])?128:0)+i,4);return;}
        if(timeout!=UINT32_MAX&&std::chrono::steady_clock::now()>=deadline){ret(258,4);return;}
        checkGuestProgress();yieldGuestMachine(1);
    }
}
void joinGuestThreads(std::unique_lock<MachineMutex>& lease){
    stopRequested=true;lease.unlock();
    for(auto& thread:guestThreads)if(thread->started){pthread_join(thread->native,nullptr);thread->started=false;}
    lease.lock();
}
void runGuestTimers(){
    while(!stopRequested){
        checkGuestProgress();std::shared_ptr<GuestTimer> next;
        for(const auto& entry:timerHandles)if(!entry.second->cancelled&&(!next||entry.second->due<next->due))next=entry.second;
        if(!next||std::chrono::steady_clock::now()<next->due){
            unsigned ms=5;
            if(next){auto left=std::chrono::duration_cast<std::chrono::milliseconds>(next->due-std::chrono::steady_clock::now()).count();ms=static_cast<unsigned>(std::max<long long>(0,std::min<long long>(left,5)));}
            waitGuestSync(guestSyncGeneration.load(),ms);continue;}
        next->running=true;runningTimerId=next->id;
        try{callGuest(next->callback,{next->id,0,next->parameter,0,0});}
        catch(...){next->running=false;runningTimerId=0;throw;}
        next->running=false;runningTimerId=0;
        if(next->periodic&&!next->cancelled){next->due=std::chrono::steady_clock::now()+std::chrono::milliseconds(next->delay);}
        else timerHandles.erase(next->id);
    }
}
void timeSetEvent(){
    uint32_t flags=arg(4),delay=arg(0),callback=arg(2);
    if(flags&~0x101u)throw GuestStop("timeSetEvent callback flags pending: "+hex(flags));
    if(!delay||(!recomp_lookup(callback)&&!recomp_lookup_manual(callback))){ret(0,5);return;}
    uint32_t id=nextTimerId++;auto timer=std::make_shared<GuestTimer>();
    timer->id=id;timer->delay=delay;timer->callback=callback;timer->parameter=arg(3);timer->periodic=flags&1;timer->synchronous=flags&0x100;
    timer->due=std::chrono::steady_clock::now()+std::chrono::milliseconds(delay);timerHandles.emplace(id,timer);
    if(timerWorker){if(timerWorker->done){timerHandles.erase(id);ret(0,5);return;}
        if(id%500==0)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest timer %u reuses worker thread %u",id,timerWorker->id);ret(id,5);return;}
    auto thread=std::make_shared<GuestThread>();thread->id=nextThreadId++;thread->timer=true;
    thread->stack=allocate(4u<<20);thread->tib=allocate(4096);
    write32(thread->tib,UINT32_MAX);write32(thread->tib+4,thread->stack+(4u<<20));write32(thread->tib+8,thread->stack);
    write32(thread->tib+0x18,thread->tib);write32(thread->tib+0x20,1);write32(thread->tib+0x24,thread->id);
    pthread_attr_t attributes;int error=pthread_attr_init(&attributes);
    if(!error){error=pthread_attr_setstacksize(&attributes,64u<<20);if(!error)error=pthread_create(&thread->native,&attributes,guestThreadEntry,thread.get());pthread_attr_destroy(&attributes);}
    if(error){heaps->free(GuestHeaps::process,thread->stack);heaps->free(GuestHeaps::process,thread->tib);timerHandles.erase(id);ret(0,5);return;}
    thread->started=true;guestThreads.push_back(thread);timerWorker=thread;
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Created persistent guest timer worker thread %u",thread->id);ret(id,5);
}
void timeKillEvent(){
    auto timer=timerHandles.find(arg(0));if(timer==timerHandles.end()){ret(97,1);return;}
    auto value=timer->second;
    if(value->running&&runningTimerId==value->id&&value->synchronous)throw GuestStop("Synchronous timer self-cancellation pending");
    value->cancelled=true;
    if(value->synchronous)while(value->running){checkGuestProgress();yieldGuestMachine(1);}
    timerHandles.erase(value->id);ret(0,1);
}
