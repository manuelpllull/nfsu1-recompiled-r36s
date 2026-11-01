#include <initializer_list>
#include <set>
#include <condition_variable>
#include "guest_runtime.h"
#include "android_resolution.h"
#include "guest_memory.h"
#include "guest_heap.h"
#include "runtime_log.h"
#include "frame_pacer.h"
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
#include "d3d9_bridge.h"
#include "nfs_widescreen.h"
extern "C" void nfsu1_present_hook(void);  // src/runtime/nfsu1_runtime.c: Speed.exe 1.4 per-frame work (game thread)
#endif
#include <array>
#include <utility>
#include <unordered_map>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <fstream>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <pthread.h>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cerrno>
#include <deque>
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#elif defined(NFS_SDL_AUDIO)
#include <SDL2/SDL.h>
#endif
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

extern "C" {
#include "recomp_types.h"
#include "pe_format.h"
#include "nfs_runtime.h"

// Guest CPU state: per host thread when RECOMP_TLS is __thread (R36S parallel guest threads, see
// HANDOVER 6.18), otherwise one global CPU swapped by State::save/load at machine handovers.
#ifdef RECOMP_TLS_STRUCT
// One struct (recomp_types.h, RECOMP_TLS_STRUCT): registers, flags, x87 window (fp_sp starts in the middle
// of the 256 slots), FPU control word, segments, MMX. Positional: eax..ebp, flags, cur_func, fs/gs base,
// icall_va, fp_top, fp_sp, fpu_cw.
static_assert(sizeof(((RecompCpu*)0)->st_win)/sizeof(double)==RECOMP_X87_SLOTS,"x87 window size");
RECOMP_TLS RecompCpu g_cpu={0,0,0,0,0,0,0,0, 0,0,0,0, 0,0,0,0, 0,RECOMP_X87_SLOTS/2, 0x027f};
#define nfs_icall_va (g_cpu.icall_va)
#else
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
#ifndef RECOMP_X87_WINDOW
double g_st[8];
#endif
RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fpu_cw = 0x027f;
RECOMP_TLS uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
RECOMP_TLS uint64_t g_mm[8];
RECOMP_TLS uint32_t g_fs_base, g_gs_base, g_cur_func;
RECOMP_TLS uint32_t nfs_icall_va;  // VA of the indirect call in flight (arm64_guest.h)
#endif
RECOMP_TLS V128 g_xmm[8];
RECOMP_TLS uint32_t g_mxcsr = 0x1f80;
ptrdiff_t g_mem_base;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx, g_icall_count;
}

namespace {
std::atomic<uint32_t> displayWidth{1280},displayHeight{720};
std::atomic<uint32_t> renderWidth{0},renderHeight{0};
std::atomic<unsigned> frameLimit{0};
std::string hex(uint32_t va) {
    std::ostringstream out; out << "0x" << std::hex << std::setw(8) << std::setfill('0') << va;
    return out.str();
}
struct GuestStop : std::runtime_error { using std::runtime_error::runtime_error; };
struct State {
    uint32_t eax{}, ecx{}, edx{}, esp{}, ebx{}, esi{}, edi{}, ebp{}, fs{}, gs{}, cur{};
    uint32_t flags[4]{}, mxcsr{0x1f80};
    uint16_t segments[6]{}, cw{0x027f};
    double st[8]{}; int top{};
    uint64_t mm[8]{}; V128 xmm[8]{};
    void save() {
        eax=g_eax; ecx=g_ecx; edx=g_edx; esp=g_esp; ebx=g_ebx; esi=g_esi; edi=g_edi; ebp=g_ebp;
        fs=g_fs_base; gs=g_gs_base; cur=g_cur_func; mxcsr=g_mxcsr;
        flags[0]=g_flag_k; flags[1]=g_flag_a; flags[2]=g_flag_b; flags[3]=g_flag_cf;
        segments[0]=g_seg_cs; segments[1]=g_seg_ds; segments[2]=g_seg_es;
        segments[3]=g_seg_fs; segments[4]=g_seg_gs; segments[5]=g_seg_ss;
        std::memcpy(st,g_st,sizeof st); top=g_fp_top; cw=g_fpu_cw;
        std::memcpy(mm,g_mm,sizeof mm); std::memcpy(xmm,g_xmm,sizeof xmm);
    }
    void load() const {
        g_eax=eax; g_ecx=ecx; g_edx=edx; g_esp=esp; g_ebx=ebx; g_esi=esi; g_edi=edi; g_ebp=ebp;
        g_fs_base=fs; g_gs_base=gs; g_cur_func=cur; g_mxcsr=mxcsr;
        g_flag_k=flags[0]; g_flag_a=flags[1]; g_flag_b=flags[2]; g_flag_cf=flags[3];
        g_seg_cs=segments[0]; g_seg_ds=segments[1]; g_seg_es=segments[2];
        g_seg_fs=segments[3]; g_seg_gs=segments[4]; g_seg_ss=segments[5];
        std::memcpy(g_st,st,sizeof st); g_fp_top=top; g_fpu_cw=cw;
        std::memcpy(g_mm,mm,sizeof mm); std::memcpy(g_xmm,xmm,sizeof xmm);
    }
};
struct Import { std::string name; recomp_func_t function{}; uint64_t calls{}; bool lockFree{}; recomp_func_t freeFunction{}; };
// R36S (NFS_PARALLEL_GUEST, guest CPU state thread-local through RECOMP_TLS): guest threads run lifted
// code in parallel and hold the machine only inside the runtime (imports, overrides, D3D9 sync calls).
// Before, one guest thread ran at a time, so the game's 100 Hz sound thread and the main thread took
// turns on one core. Enabled right before the game's entry point; NFSU2_PARALLEL_GUEST=0 disables it.
bool parallelGuest=false;
#ifndef NFS_DEFAULT_FREE_THREADS
#define NFS_DEFAULT_FREE_THREADS "654a90"   // Speed.exe 1.4: the 100 Hz DirectSound buffer-filler thread
#endif
// Which guest threads run without the machine ("free"): only those whose entry point is listed in
// NFSU2_PARALLEL_THREADS (default NFS_DEFAULT_FREE_THREADS: Speed.exe's sound/streaming thread). The game's main
// thread and its other workers keep taking turns under the machine: x86 orders stores for every other
// core (TSO), ARM does not, and lifted code has no barriers, so threads that hand data to each other
// through plain memory flags (the movie decoder did: a garbage pointer during the EA logo) must not run
// at the same time. The sound thread talks to the game through Win32 mutexes (barriers in the runtime).
thread_local bool freeRunning=false;
std::vector<uint32_t> freeThreadEntries;
std::unique_ptr<GuestMemory> memory;
std::unique_ptr<GuestHeaps> heaps;
// The guest machine lock: FIFO by ticket and recursive per thread, with the
// std::recursive_mutex interface the scheduler code already uses.
//
// std::recursive_mutex is not fair. The game thread releases the machine
// around imports and every 500 us, then asks for it again at once and usually
// wins; the guest audio mixer thread waited behind it, filled its DirectSound
// ring too late, and the mixer replayed stale data ("the sound loops"). The
// same starvation made the Windows build stutter until native32 got a ticket
// lock. Here every unlock hands the machine to the longest waiter.
std::atomic<uint64_t> statMachineLocks{0},statMachineWaits{0},statSyncWaits{0},statSyncNotifies{0};
extern "C" void nfsWaitAdd(int kind,uint64_t ns);
extern "C" bool nfsIsGameThread();
extern "C" void nfsCsWait(uint32_t address,uint64_t ns);
#if !defined(NFS_R36S)   // the R36S build has the real ones (src/runtime/fx_cache.c)
extern "C" int nfs_fx_cap_depth = 0;
extern "C" void nfs_fx_capture(uint32_t, const uint32_t*, uint32_t, int) {}
#endif
class MachineMutex {
    std::mutex m_;
    std::condition_variable cv_;
    uint64_t next_=0,serving_=0;
    std::thread::id owner_{};
    unsigned depth_=0;
public:
    void lock(){
        const auto me=std::this_thread::get_id();
        std::unique_lock<std::mutex> l(m_);
        if(owner_==me){++depth_;return;}
        const uint64_t ticket=next_++;
        ++statMachineLocks;
        if(serving_!=ticket){
            ++statMachineWaits;
            const auto t0=std::chrono::steady_clock::now();
            cv_.wait(l,[&]{return serving_==ticket;});
            const uint64_t waited=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count();
            if(nfsIsGameThread())nfsWaitAdd(2,waited);
            else if(freeRunning){nfsWaitAdd(12,waited);nfsWaitAdd(13,1);}
        }
        owner_=me;depth_=1;acquired=std::chrono::steady_clock::now();
    }
    // When the current owner got the machine; only the owner reads it.
    std::chrono::steady_clock::time_point acquired{};
    // True when another thread holds a ticket (is queued for the machine). Called by the owner.
    bool hasWaiters(){std::lock_guard<std::mutex> l(m_);return next_!=serving_+1;}
    bool try_lock(){
        const auto me=std::this_thread::get_id();
        std::lock_guard<std::mutex> l(m_);
        if(owner_==me){++depth_;return true;}
        if(next_!=serving_)return false;
        ++next_;owner_=me;depth_=1;acquired=std::chrono::steady_clock::now();return true;
    }
    // Parallel guest threads (R36S): the owner check, and a full release/re-acquire around guest code.
    bool heldByMe(){std::lock_guard<std::mutex> l(m_);return owner_==std::this_thread::get_id();}
    unsigned releaseAll(){
        unsigned depth;
        {
            std::lock_guard<std::mutex> l(m_);
            if(owner_!=std::this_thread::get_id())return 0;
            depth=depth_;depth_=0;owner_={};++serving_;
            if(next_==serving_)return depth;
        }
        cv_.notify_all();return depth;
    }
    void reacquire(unsigned depth){
        if(!depth)return;
        lock();
        std::lock_guard<std::mutex> l(m_);depth_=depth;
    }
    void unlock(){
        {
            std::lock_guard<std::mutex> l(m_);
            if(--depth_)return;
            owner_={};++serving_;
            if(next_==serving_)return;          // nobody waiting
        }
        cv_.notify_all();
    }
};
MachineMutex machine;
thread_local std::unique_lock<MachineMutex>* machineLease=nullptr;
thread_local uint32_t guestThreadId=1;
std::vector<Import> imports;
std::map<std::string,uint32_t> modules;
thread_local uint32_t lastError=0;
uint32_t imageBase{}, imageSize{}, entry{};
uint32_t resourceRva{},resourceSize{};
// Set and read under the machine lock with no yield in between (lookup -> call):
// plain globals, not thread_local, which cost a TLS descriptor call per COM call.
thread_local uint32_t lastImport{};
std::string hostGameRoot;
std::string guestLanguage="Spanish";
bool guestWidescreen=true;
std::atomic<bool> stopRequested{false};
std::atomic<bool> guestPaused{false};
std::chrono::steady_clock::time_point bootDeadline;
std::string guestThreadFault;
bool timerTestActive=false;std::vector<uint32_t> timerTestCalls;
constexpr uint32_t timerTestAddress=0x7b000010u;
uint32_t read32(uint32_t va);
// Stall diagnostics: where each guest thread last handed the machine over and
// what it was waiting for, dumped when the game stops presenting frames.
struct GuestThreadTrace {
    std::atomic<uint32_t> func{0},handle{0},esp{0};std::atomic<const char*> wait{nullptr};
    std::array<std::atomic<uint32_t>,16> held{};  // critical sections this thread owns (outermost entries)
    void hold(uint32_t cs){for(auto& slot:held){uint32_t empty=0;if(slot.compare_exchange_strong(empty,cs))return;}}
    void release(uint32_t cs){for(auto& slot:held){uint32_t value=cs;if(slot.compare_exchange_strong(value,0))return;}}
    bool holdsLocks() const {
        for(const auto& slot:held){if(slot.load(std::memory_order_acquire)!=0)return true;}
        return false;
    }
    bool holds(uint32_t cs) const {
        if(!cs)return false;
        for(const auto& slot:held){if(slot.load(std::memory_order_acquire)==cs)return true;}
        return false;
    }
};
std::mutex threadTracesMutex;
std::map<uint32_t,std::shared_ptr<GuestThreadTrace>> threadTraces;
thread_local std::shared_ptr<GuestThreadTrace> threadTrace;
GuestThreadTrace& currentTrace(){
    if(!threadTrace){threadTrace=std::make_shared<GuestThreadTrace>();std::lock_guard<std::mutex> lock(threadTracesMutex);threadTraces[guestThreadId]=threadTrace;}
    return *threadTrace;
}
void forgetThreadTrace(){std::lock_guard<std::mutex> lock(threadTracesMutex);threadTraces.erase(guestThreadId);threadTrace.reset();}
struct WaitScope {
    WaitScope(const char* what,uint32_t handle){auto& trace=currentTrace();trace.wait=what;trace.handle=handle;}
    ~WaitScope(){currentTrace().wait=nullptr;}
};
std::atomic<int64_t> lastPresentMs{0};
int64_t steadyMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void startStallWatch(){
    static std::once_flag started;
    std::call_once(started,[]{std::thread([]{
        int64_t reportedFor=0;
        while(!stopRequested.load()){
            std::this_thread::sleep_for(std::chrono::seconds(2));
            int64_t last=lastPresentMs.load(),idle=steadyMs()-last;
            if(guestPaused.load()||idle<12000||reportedFor==last)continue;
            reportedFor=last;
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Guest stall: no frame for %lld s; guest threads:",static_cast<long long>(idle/1000));
            std::lock_guard<std::mutex> lock(threadTracesMutex);
            for(auto& [id,trace]:threadTraces){
                const char* wait=trace->wait.load();uint32_t handle=trace->handle.load();
                std::string detail,held,callers;
                // Read-only peeks at guest memory from this host thread: stable while the guest is stuck.
                if(wait&&!std::strcmp(wait,"EnterCriticalSection")&&memory&&memory->readable(handle,24))
                    detail=" (owner thread "+std::to_string(read32(handle+12))+", count "+std::to_string(read32(handle+8))+")";
                for(auto& slot:trace->held)if(uint32_t cs=slot.load())held+=" "+hex(cs);
                uint32_t esp=trace->esp.load();
                for(unsigned i=0,found=0;memory&&i<256&&found<10;++i){
                    if(!memory->readable(esp+i*4,4))break;
                    uint32_t value=read32(esp+i*4);
                    if(value>=0x401000u&&value<0x697000u&&recomp_lookup(value)==nullptr){callers+=" "+hex(value);++found;}
                }
                NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","  thread %u at %08x %s %08x%s; holds:%s; stack:%s",id,trace->func.load(),
                    wait?wait:"running",wait?handle:0u,detail.c_str(),held.empty()?" none":held.c_str(),callers.c_str());
            }
        }
    }).detach();});
}
void yieldGuestMachine(unsigned milliseconds=0,std::chrono::steady_clock::time_point deadline={}) {
    if(parallelGuest&&!machine.heldByMe()){if(milliseconds)std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));return;}
    if(!machineLease||!machineLease->owns_lock())throw GuestStop("Guest scheduler ownership missing");
    // Nothing queued for the machine: a plain yield would only cost an unlock, a syscall and a relock.
    if(!milliseconds&&!machine.hasWaiters())return;
    if(!milliseconds&&currentTrace().holdsLocks())return;
    {auto& trace=currentTrace();trace.func=g_cur_func;trace.esp=g_esp;}
    State saved;saved.save();machineLease->unlock();
    if(deadline!=std::chrono::steady_clock::time_point{})std::this_thread::sleep_until(deadline);
    else if(milliseconds)std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));else std::this_thread::yield();
    machineLease->lock();saved.load();
}
thread_local bool renderThread=false;
// Blocking waits for guest synchronization objects. Guest threads only touch events, mutexes and
// critical sections while holding the machine, so: read the generation under the machine, test the
// condition, then waitGuestSync(generation) releases the machine and sleeps until any sync object
// changes (notifyGuestSync) or the timeout expires. This replaces 1 ms polling loops that kept every
// worker waking up and contending for the machine lock (see HANDOVER 6.10).
std::mutex guestSyncMutex;
std::condition_variable guestSyncCv;
std::atomic<uint64_t> guestSyncGeneration{0};
// Threads inside waitGuestSync, changed under guestSyncMutex like the generation: a notify that sees none skips the
// futex wake (the game releases its mutexes and leaves critical sections ~100 times a frame, mostly unwatched); a
// waiter that registers later sees the new generation before it sleeps.
unsigned guestSyncWaiters=0;
uint64_t statSyncNotifiesUnwatched=0;
void notifyGuestSync(){
    ++statSyncNotifies;
    bool watched;
    {std::lock_guard<std::mutex> l(guestSyncMutex);guestSyncGeneration.fetch_add(1);watched=guestSyncWaiters!=0;if(!watched)++statSyncNotifiesUnwatched;}
    if(watched)guestSyncCv.notify_all();
}
// ready (optional) is evaluated under guestSyncMutex without the machine: writers change the object
// under the machine and then call notifyGuestSync (which takes guestSyncMutex), so the change is visible.
// Waiters whose own object did not change keep sleeping instead of queueing for the machine.
template<class Ready>
void waitGuestSync(uint64_t seen,unsigned maxMilliseconds,Ready ready){
    ++statSyncWaits;
    if(parallelGuest&&!machine.heldByMe()){
        std::unique_lock<std::mutex> l(guestSyncMutex);
        ++guestSyncWaiters;
        guestSyncCv.wait_for(l,std::chrono::milliseconds(maxMilliseconds),[&]{return stopRequested.load()||(guestSyncGeneration.load()!=seen&&ready());});
        --guestSyncWaiters;
        return;
    }
    if(!machineLease||!machineLease->owns_lock())throw GuestStop("Guest scheduler ownership missing");
    State saved;saved.save();machineLease->unlock();
    {
        std::unique_lock<std::mutex> l(guestSyncMutex);
        ++guestSyncWaiters;
        guestSyncCv.wait_for(l,std::chrono::milliseconds(maxMilliseconds),[&]{return stopRequested.load()||(guestSyncGeneration.load()!=seen&&ready());});
        --guestSyncWaiters;
    }
    machineLease->lock();saved.load();
}
void waitGuestSync(uint64_t seen,unsigned maxMilliseconds){waitGuestSync(seen,maxMilliseconds,[]{return true;});}
void checkGuestProgress() {
    while(guestPaused.load()&&!stopRequested.load())yieldGuestMachine(20);  // sleeps when not holding the machine
    if(stopRequested.load())throw GuestStop("Guest execution cancelled");
    // Imports and lifted back edges can reach this hook many times per frame.
    // Keep cancellation immediate, but amortize full register saves and native
    // scheduler handovers over a short time slice rather than every call.
    // One time slice for whichever guest thread holds the machine.
#ifdef NFS_R36S
    static thread_local unsigned checkpoints=0;
    static thread_local auto nextYield=std::chrono::steady_clock::time_point::min();
    if((++checkpoints&63u)==0){
        auto now=std::chrono::steady_clock::now();
        if(bootDeadline!=std::chrono::steady_clock::time_point::max()&&now>bootDeadline)throw GuestStop("CRT bring-up time budget reached at "+hex(g_cur_func));
        // Time slicing only matters while threads take turns; parallel guest code holds no machine.
        if(now>=nextYield&&(!parallelGuest||machine.heldByMe())){
            if(!currentTrace().holdsLocks()){
                if(machine.hasWaiters())yieldGuestMachine();
                nextYield=std::chrono::steady_clock::now()+std::chrono::microseconds(500);
            }
        }
#else
    //
    // Slices count from when this thread acquired the machine (a shared
    // deadline left the incoming thread an expired slice, so it handed the
    // machine straight back). The render thread, the one presenting, gets
    // 3 ms and the others 500 us: every handover wakes the queued threads in
    // turn, and with 500 us slices the Underground 2 render thread spent 56%
    // of its wall time waiting to get the machine back (Redmi Note 8).
    static unsigned checkpoints=0;
    if((++checkpoints&63u)==0){
        auto now=std::chrono::steady_clock::now();
        if(now>bootDeadline)throw GuestStop("CRT bring-up time budget reached at "+hex(g_cur_func));
        auto slice=renderThread?std::chrono::microseconds(3000):std::chrono::microseconds(500);
        if(now-machine.acquired>=slice){if(machine.hasWaiters())yieldGuestMachine();}
#endif
    }
    if(!guestThreadFault.empty())throw GuestStop(guestThreadFault);
}
constexpr uint32_t importBase = 0x7f000000u;
constexpr uint32_t stackBase = 0x20000000u, stackSize = 16u << 20, tib = 0x21000000u;

void* ptr(uint32_t va, uint64_t bytes) { return memory->address(va, bytes); }
uint32_t read32(uint32_t va) { uint32_t v; std::memcpy(&v, ptr(va,4),4); return v; }
void write32(uint32_t va, uint32_t v) { std::memcpy(ptr(va,4),&v,4); }
uint32_t arg(uint32_t index) { return read32(g_esp + 4 + index*4); }
void ret(uint32_t value, uint32_t argc) { g_eax=value; g_esp+=4+argc*4; }
thread_local uint32_t stackTib=0, stackLow=0, stackHigh=0, stackMinEsp=UINT32_MAX;
thread_local bool stackOutsideLogged=false;
void sampleGuestStack() {
    if (!g_fs_base || !memory || !memory->readable(g_fs_base, 12)) return;
    if (stackTib != g_fs_base) {
        stackTib = g_fs_base;
        stackLow = read32(g_fs_base + 8);
        stackHigh = read32(g_fs_base + 4);
        stackMinEsp = stackHigh;
        stackOutsideLogged = false;
    }
    if (g_esp < stackLow || g_esp > stackHigh) {
        if (!stackOutsideLogged) {
            stackOutsideLogged = true;
            NFS_RUNTIME_LOG(ANDROID_LOG_ERROR, "NFSU2", "Guest ESP outside TIB stack: esp=%08x range=%08x-%08x tid=%u",
                            g_esp, stackLow, stackHigh, guestThreadId);
        }
        return;
    }
    if (g_esp < stackMinEsp) stackMinEsp = g_esp;
}
std::string guestString(uint32_t address) {
    if(!address) return {};
    std::string s;
    for(uint32_t i=0;i<32768;++i) {
        char c=*static_cast<char*>(ptr(address+i,1));
        if(!c) return s;
        s.push_back(c);
    }
    throw GuestStop("Unterminated guest string at "+hex(address));
}
std::string lower(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));}); return s;
}
uint32_t allocate(uint32_t bytes) {
    uint32_t result=heaps->alloc(GuestHeaps::process,8,bytes);
    if(!result) throw GuestStop("Guest allocator exhausted");
    return result;
}
void unsupported() {
    uint32_t index = (lastImport-importBase)/16;
    throw GuestStop("HLE pendiente: " + imports.at(index).name + " desde " + hex(g_cur_func));
}
void getVersion() {
    uint32_t out = arg(0), bytes=read32(out);
    if (bytes != 148 && bytes != 156) { ret(0,1); return; }
    std::memset(ptr(out+4,bytes-4),0,bytes-4);
    write32(out+4,5); write32(out+8,1); write32(out+12,2600); write32(out+16,2);
    if (bytes==156) { auto* p=static_cast<unsigned char*>(ptr(out+148,8)); p[6]=1; }
    ret(1,1);
}
// guest_modules.inc (game DLLs run as lifted code) needs callGuest, so it comes later.
uint32_t loadGuestModule(const std::string& name);
bool freeGuestModule(uint32_t handle);
uint32_t guestModuleHandle(const std::string& name);
bool guestModuleExport(uint32_t handle,const std::string& name,uint32_t& va);
bool isGuestModule(uint32_t handle);
void moduleHandle() {
    if(!arg(0)) { ret(imageBase,1); return; }
    auto name=lower(guestString(arg(0)));
    auto separator=name.find_last_of("/\\"); if(separator!=std::string::npos) name=name.substr(separator+1);
    if(name=="speed.exe"||name=="speed2.exe") { ret(imageBase,1); return; }
    if(name.find('.')==std::string::npos) name+=".dll";
    if(uint32_t loaded=guestModuleHandle(name)) { ret(loaded,1); return; }
    auto module=modules.find(name);
    static std::set<std::string> loggedLookups;
    if(loggedLookups.insert(name).second)
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Module lookup %s",name.c_str());
    if(module==modules.end()) lastError=126;
    ret(module==modules.end()?0:module->second,1);
}
void heapCreate() {
    ret(heaps->create(arg(0),arg(1),arg(2)),3);
}
void heapAlloc() { ret(heaps->alloc(arg(0),arg(1),arg(2)),3); }
void heapFree() { ret(heaps->free(arg(0),arg(2)),3); }
void heapRealloc() { ret(heaps->realloc(arg(0),arg(1),arg(2),arg(3)),4); }
void heapSize() { ret(heaps->size(arg(0),arg(2)),3); }
void heapDestroy() { ret(heaps->destroy(arg(0)),1); }
void getProcessHeap() { ret(GuestHeaps::process,0); }
void getProcAddress();
thread_local uint32_t qpcStreak=0;  // consecutive QueryPerformanceCounter reads (guest_kernel.inc)
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
thread_local uint32_t d3dToken{};
void direct3DCreate(){ret(createGuestD3D9(arg(0)),1);}
void freeD3DGuest(uint32_t address){heaps->free(GuestHeaps::process,address);}
void serviceGuestSound();
// D3D9 methods read at most args[0..9]: copy 12.
extern "C" int nfs_fx_cap_depth;
extern "C" void nfs_fx_capture(uint32_t token,const uint32_t* args,uint32_t count,int deferred);
void d3dDispatch(){uint32_t args[32];std::memcpy(args,ptr(g_esp+4,48),48);uint32_t count=0;uint32_t token=d3dToken;
    if(__builtin_expect(nfs_fx_cap_depth!=0,0))nfs_fx_capture(token,nullptr,0,0);  // effect state memo: not replayable
    // Fast path: a queued (or filtered) call other than Present returns straight away.
    if(parallelGuest&&token!=0x7e002110u){uint32_t result;
        if(dispatchGuestD3D9TryDefer(token,args,&count,&result)){qpcStreak=0;ret(result,count);return;}}
    qpcStreak=0;  // drawing: not a busy-wait (see performanceCounter)
    static uint64_t bridgeCalls=0;static std::chrono::nanoseconds bridgeTime{},presentTime{};
    // Time one call in 64 (and every Present) so the statistic does not cost
    // two clock reads on each of the ~40k calls in a race frame; the stack-depth
    // sample (a guest memory map lookup) follows the same rate.
    bool timed=(++bridgeCalls&63u)==0||token==0x7e002110u;
    if(timed&&!parallelGuest)sampleGuestStack();  // reads the guest page map: not without the machine
    std::chrono::steady_clock::time_point bridgeStart;if(timed)bridgeStart=std::chrono::steady_clock::now();uint32_t result;
    try{
        if(!parallelGuest)result=dispatchGuestD3D9(token,args,&count);
        else if(token!=0x7e002110u||!dispatchGuestD3D9TryDefer(token,args,&count,&result)){  // others: tried above
            // Synchronous D3D9 call: the bridge allocates guest memory (heaps): needs the machine.
            if(!freeRunning)result=dispatchGuestD3D9(token,args,&count);
            else{
                machine.lock();
                try{result=dispatchGuestD3D9(token,args,&count);}catch(...){machine.unlock();throw;}
                machine.unlock();
            }
        }
    }catch(...){
        NFS_RUNTIME_LOG(ANDROID_LOG_ERROR,"NFSU1","D3D9 guest caller=%08x stack=%08x token=%08x self=%08x",g_cur_func,g_esp,token,args[0]);throw;}
    if(timed){auto spent=std::chrono::steady_clock::now()-bridgeStart;bridgeTime+=token==0x7e002110u?spent:spent*64;presentTime+=token==0x7e002110u?spent:std::chrono::nanoseconds{};}
    if(token==0x7e002110u){
        nfs_widescreen_apply_frame_rate();
#if defined(NFS_R36S)
        nfsu1_present_hook();
#endif
        lastPresentMs=steadyMs();
        startStallWatch();
        // Diagnostics: NFSU2_IMPORT_STATS=1 logs the most-called imports (per frame) every 600 frames.
        static const bool importStats=std::getenv("NFSU2_IMPORT_STATS")!=nullptr;
        static unsigned statFrames=0;static std::vector<uint64_t> lastCalls;
        if(importStats&&++statFrames%600==0){
            lastCalls.resize(imports.size());
            std::vector<std::pair<uint64_t,size_t>> rows;
            for(size_t i=0;i<imports.size();++i){rows.push_back({imports[i].calls-lastCalls[i],i});lastCalls[i]=imports[i].calls;}
            std::sort(rows.rbegin(),rows.rend());
            static uint64_t l0=0,l1=0,l2=0,l3=0;
            static uint64_t l4=0;
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","sync stats: machine locks %.1f/frame (%.1f waited), guest sync waits %.1f/frame, notifies %.1f/frame (%.1f with nobody waiting)",
                (statMachineLocks-l0)/600.0,(statMachineWaits-l1)/600.0,(statSyncWaits-l2)/600.0,(statSyncNotifies-l3)/600.0,(statSyncNotifiesUnwatched-l4)/600.0);
            l0=statMachineLocks;l1=statMachineWaits;l2=statSyncWaits;l3=statSyncNotifies;l4=statSyncNotifiesUnwatched;
            for(size_t i=0;i<rows.size()&&i<12&&rows[i].first;++i)
                NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","import stats: %s %.1f/frame%s",imports[rows[i].second].name.c_str(),rows[i].first/600.0,imports[rows[i].second].lockFree?" (lock-free)":"");
        }
    }
    if(token==0x7e002110u&&result==0){
#if defined(NFS_R36S)
        static unsigned appliedLimit=0;static auto nextFrame=std::chrono::steady_clock::now();
        unsigned cap=frameLimit.load(std::memory_order_relaxed);
        auto now=std::chrono::steady_clock::now();
        if(cap!=appliedLimit){appliedLimit=cap;nextFrame=now;}
        // Next deadline one period after the previous one, but never in the past: a frame that ran late
        // starts the next period now. (It used to reset only after falling 200 ms behind and then sleep a
        // whole period: at 5 fps that slept 33 ms every other frame.)
        if(cap){nextFrame=std::max(nextFrame+std::chrono::nanoseconds(1000000000ull/cap),now);
            if(nextFrame>now){std::this_thread::sleep_until(nextFrame);if(nfsIsGameThread())nfsWaitAdd(9,std::chrono::duration_cast<std::chrono::nanoseconds>(nextFrame-now).count());}}
#else
        static FramePacer pacer;
        auto now=std::chrono::steady_clock::now();
        auto deadline=pacer.deadline(now,frameLimit.load(std::memory_order_relaxed));
        // The renderer has returned; guest audio/streaming workers can use the
        // machine while this thread waits. Late frames incur no extra delay.
        renderThread=true;
        if(deadline>now)yieldGuestMachine(0,deadline);
        else if(machine.hasWaiters())yieldGuestMachine();
#endif
        static unsigned frames=0;static uint64_t totalFrames=0;static auto since=std::chrono::steady_clock::now();
        ++totalFrames;
        if(++frames==120){serviceGuestSound();auto now=std::chrono::steady_clock::now();double seconds=std::chrono::duration<double>(now-since).count();
            uint32_t stackUsed=(stackHigh>stackMinEsp)?stackHigh-stackMinEsp:0;
            uint32_t stackSize=(stackHigh>stackLow)?stackHigh-stackLow:0;
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest D3D9 frame=%llu %.1f FPS over %.2f seconds; bridge ~%.2f ms/frame (present %.2f), %.0f calls/frame; guest stack %u/%u KiB",
                static_cast<unsigned long long>(totalFrames),frames/seconds,seconds,double(bridgeTime.count())/1e6/frames,double(presentTime.count())/1e6/frames,double(bridgeCalls)/frames,stackUsed/1024,stackSize/1024);
            frames=0;since=now;bridgeTime=presentTime=std::chrono::nanoseconds{};bridgeCalls=0;}
    }ret(result,count);}
#endif
// threadStart: a thread procedure may be stdcall or cdecl; Windows exits the thread right after either.
uint32_t callGuest(uint32_t va,const uint32_t* args,size_t count,uint32_t cleanup,bool threadStart);
// Braced argument lists stay on the host stack: callbacks run many times a
// frame and a std::vector per call cost a malloc/free pair.
inline uint32_t callGuest(uint32_t va,std::initializer_list<uint32_t> args,uint32_t cleanup=0,bool threadStart=false){return callGuest(va,args.begin(),args.size(),cleanup,threadStart);}
inline uint32_t callGuest(uint32_t va,const std::vector<uint32_t>& args,uint32_t cleanup=0,bool threadStart=false){return callGuest(va,args.data(),args.size(),cleanup,threadStart);}
#include "guest_threads.inc"
#include "guest_files.inc"
#include "guest_kernel.inc"
#include "guest_input.inc"
#include "guest_audio.inc"
#include "guest_network.inc"
void testGuestScheduling(){
    State original;original.save();uint32_t savedError=lastError,savedTLS=tlsValues[1087];
    g_eax=0x11223344;g_fs_base=0x21000000;g_st[0]=123.5;g_xmm[7].u64[0]=0x123456789abcdef0ull;
    lastError=111;tlsValues[1087]=0x55;
    bool done=false;std::string failure;
    std::thread worker([&]{
        std::unique_lock<MachineMutex> lease(machine);machineLease=&lease;guestThreadId=9000;
        State fresh;fresh.eax=0xaabbccdd;fresh.fs=0xdead1000;fresh.st[0]=-456.25;fresh.xmm[7].u64[0]=0xfedcba9876543210ull;fresh.load();
        lastError=222;tlsValues[1087]=0xaa;
        try{for(unsigned i=0;i<100;++i){checkGuestProgress();yieldGuestMachine();
            if(g_eax!=0xaabbccdd||g_fs_base!=0xdead1000||g_st[0]!=-456.25||g_xmm[7].u64[0]!=0xfedcba9876543210ull||lastError!=222||tlsValues[1087]!=0xaa)
                throw GuestStop("Guest worker context was not isolated");}}
        catch(const std::exception& error){failure=error.what();}
        forgetThreadTrace();done=true;machineLease=nullptr;
    });
    try{while(!done){checkGuestProgress();yieldGuestMachine(0,std::chrono::steady_clock::now()+std::chrono::milliseconds(1));
        if(g_eax!=0x11223344||g_fs_base!=0x21000000||g_st[0]!=123.5||g_xmm[7].u64[0]!=0x123456789abcdef0ull||lastError!=111||tlsValues[1087]!=0x55)
            failure="Main guest context was not preserved during handover";
    }}catch(const std::exception& error){failure=error.what();}
    machineLease->unlock();worker.join();machineLease->lock();original.load();lastError=savedError;tlsValues[1087]=savedTLS;
    if(!failure.empty())throw GuestStop(failure);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest scheduler passed 100 handovers with separate integer, FS, x87, XMM, TLS and last-error state");
}
void testGuestSoundCaps(){
    State saved;saved.save();
    uint32_t output=allocate(96),device=createSoundObject(1);
    write32(output,96);
    uint32_t result=callGuest(soundMethodBase+0x1000+4*16,{device,output});
    bool ok=result==0&&read32(output)==96&&read32(output+4)==0xf1f&&read32(output+8)==100&&
        read32(output+12)==200000&&read32(output+16)==1&&read32(output+20)==0;
    callGuest(soundMethodBase+0x1000+2*16,{device});heaps->free(GuestHeaps::process,output);saved.load();
    if(!ok)throw GuestStop("DirectSound PCM capabilities regression");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSMW","DirectSound caps passed: mono/stereo, 8/16-bit, continuous rates, software buffers and guest ABI");
}
recomp_func_t resolveFunction(const std::string& dll, const std::string& name) {
    if((dll=="shfolder.dll"||dll=="shell32.dll")&&name=="SHGetFolderPathA")return shellFolderPath;
    if(dll=="dsound.dll"&&(name=="#1"||name=="DirectSoundCreate"))return directSoundCreate;
    if(dll=="dinput8.dll"&&name=="DirectInput8Create")return directInputCreate;
    if(dll=="gdi32.dll"&&name=="DeleteObject")return deleteGdiObject;
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    if(dll=="d3d9.dll"&&name=="Direct3DCreate9")return direct3DCreate;
#endif
    if(dll=="advapi32.dll"&&name=="RegOpenKeyA")return regOpenKeyA;
    if(dll=="advapi32.dll"&&name=="RegOpenKeyExA")return regOpenKeyExA;
    if(dll=="advapi32.dll"&&name=="RegCloseKey")return regCloseKey;
    if(dll=="advapi32.dll"&&name=="RegQueryValueExA")return regQueryValueExA;
    if(dll=="advapi32.dll"&&name=="RegSetValueExA")return regSetValueExA;
    if(dll=="ws2_32.dll"||dll=="wsock32.dll"){auto found=networkFunctions.find(name);if(found!=networkFunctions.end())return found->second;}
    if(dll=="netapi32.dll"&&name=="Netbios")return netbios;
    if(dll=="tapi32.dll"&&name=="lineInitialize")return lineInitialize;
    if(dll=="tapi32.dll"&&name=="lineShutdown")return lineShutdown;
    if(dll=="user32.dll") {auto found=windowFunctions.find(name);if(found!=windowFunctions.end())return found->second;}
    if(dll=="winmm.dll") {auto found=timerFunctions.find(name);if(found!=timerFunctions.end())return found->second;}
    if (dll == "kernel32.dll") {
        auto resource=resourceFunctions.find(name);if(resource!=resourceFunctions.end())return resource->second;
        auto file=fileFunctions.find(name);if(file!=fileFunctions.end())return file->second;
        auto kernel=kernelFunctions.find(name); if(kernel!=kernelFunctions.end()) return kernel->second;
        if (name=="GetVersionExA") return getVersion;
        if (name=="GetModuleHandleA") return moduleHandle;
        if (name=="HeapCreate") return heapCreate;
        if (name=="HeapAlloc") return heapAlloc;
        if (name=="HeapFree") return heapFree;
        if (name=="HeapReAlloc") return heapRealloc;
        if (name=="HeapSize") return heapSize;
        if (name=="HeapDestroy") return heapDestroy;
        if (name=="GetProcessHeap") return getProcessHeap;
    }
    return unsupported;
}
void getProcAddress() {
    uint32_t module=arg(0),namePointer=arg(1);
    auto found=std::find_if(modules.begin(),modules.end(),[&](auto& m){return m.second==module;});
    auto name=namePointer<=0xffff?"#"+std::to_string(namePointer):guestString(namePointer);
    if(uint32_t va;found==modules.end()&&guestModuleExport(module,name,va)) { ret(va,2); return; }
    if(found==modules.end()) { lastError=isGuestModule(module)?127:126; ret(0,2); return; }
    auto full=found->first+"!"+name;
    for(uint32_t i=0;i<imports.size();++i) if(imports[i].name==full) { ret(importBase+i*16,2); return; }
    // Only advertise exports that are actually implemented, not permissive stubs.
    auto fn=resolveFunction(found->first,name);
    if(fn==unsupported) { lastError=127; ret(0,2); return; }
    uint32_t va=importBase+uint32_t(imports.size())*16;
    imports.push_back({full,fn}); ret(va,2);
}

template<class T> T fileStruct(const std::vector<unsigned char>& file, uint64_t offset) {
    if (offset > file.size() || sizeof(T) > file.size()-offset) throw GuestStop("Truncated PE structure");
    T value{}; std::memcpy(&value,file.data()+offset,sizeof value); return value;
}
void moduleRange(uint32_t size, uint32_t rva, uint64_t bytes) {
    if (rva > size || bytes > uint64_t(size)-rva) throw GuestStop("PE image range invalid");
}
std::string moduleString(uint32_t base, uint32_t size, uint32_t rva) {
    moduleRange(size,rva,1); std::string value;
    for (uint32_t i=rva; i<size && value.size()<512; ++i) {
        char c = *static_cast<char*>(ptr(base+i,1)); if (!c) return value; value.push_back(c);
    }
    throw GuestStop("Unterminated PE import name");
}
void imageRange(uint32_t rva, uint64_t bytes) { moduleRange(imageSize,rva,bytes); }
// Writes an import token into every IAT slot of a mapped image (Speed.exe or a game DLL).
void bindImports(uint32_t base, uint32_t size, uint32_t directoryRva, uint32_t directorySize) {
    moduleRange(size,directoryRva,directorySize);
    bool terminated=false;
    for (uint32_t offset=0; uint64_t(offset)+sizeof(pe_import_descriptor)<=directorySize; offset+=sizeof(pe_import_descriptor)) {
        pe_import_descriptor descriptor{};
        std::memcpy(&descriptor,ptr(base+directoryRva+offset,sizeof descriptor),sizeof descriptor);
        if (!descriptor.Name && !descriptor.FirstThunk) { terminated=true; break; }
        auto dll=moduleString(base,size,descriptor.Name);
        std::transform(dll.begin(),dll.end(),dll.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(!modules.count(dll)) modules[dll]=0x71000000u+uint32_t(modules.size())*0x10000u;
        uint32_t names=descriptor.OriginalFirstThunk ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
        for (uint32_t i=0;;++i) {
            uint64_t nameRva=uint64_t(names)+i*4ull, iatRva=uint64_t(descriptor.FirstThunk)+i*4ull;
            if (nameRva>UINT32_MAX || iatRva>UINT32_MAX) throw GuestStop("PE thunk overflow");
            moduleRange(size,uint32_t(nameRva),4); moduleRange(size,uint32_t(iatRva),4);
            uint32_t thunk=read32(base+uint32_t(nameRva)); if (!thunk) break;
            std::string name;
            if (thunk&PE_ORDINAL_FLAG) name="#"+std::to_string(thunk&0xffff);
            else { moduleRange(size,thunk,3); name=moduleString(base,size,thunk+2); }
            if (imports.size()>=4096) throw GuestStop("Too many PE imports");
            uint32_t va=importBase+uint32_t(imports.size())*16;
            imports.push_back({dll+"!"+name,resolveFunction(dll,name)});
            write32(base+uint32_t(iatRva),va);
        }
    }
    if (!terminated) throw GuestStop("Unterminated PE import descriptors");
}
void loadImage(const char* path) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) throw GuestStop("Cannot open game PE");
    std::vector<unsigned char> file((std::istreambuf_iterator<char>(stream)),{});
    auto dos=fileStruct<pe_dos_header>(file,0);
    if (dos.e_magic!=0x5a4d || dos.e_lfanew<0) throw GuestStop("Invalid DOS header");
    auto nt=fileStruct<pe_nt_headers32>(file,dos.e_lfanew);
    if (nt.Signature!=0x4550 || nt.FileHeader.Machine!=0x14c || nt.OptionalHeader.Magic!=0x10b ||
        nt.FileHeader.SizeOfOptionalHeader<sizeof(pe_opt_header32)) throw GuestStop("Expected x86 PE32");
    auto opt=nt.OptionalHeader; imageBase=opt.ImageBase; imageSize=opt.SizeOfImage;
    resourceRva=opt.DataDirectory[2].VirtualAddress;resourceSize=opt.DataDirectory[2].Size;
    if (imageBase!=0x400000 || imageSize!=0x394368 || opt.AddressOfEntryPoint!=0x270cb5)  // Speed.exe 1.4
        throw GuestStop("PE layout does not match generated NFSU1 code");
    memory->commit(imageBase,imageSize);
    imageRange(0,opt.SizeOfHeaders);
    if (opt.SizeOfHeaders>file.size()) throw GuestStop("Truncated PE headers");
    std::memcpy(ptr(imageBase,opt.SizeOfHeaders),file.data(),opt.SizeOfHeaders);
    uint64_t sections=uint64_t(dos.e_lfanew)+24+nt.FileHeader.SizeOfOptionalHeader;
    for (uint32_t i=0;i<nt.FileHeader.NumberOfSections;++i) {
        auto sec=fileStruct<pe_section_header>(file,sections+i*sizeof(pe_section_header));
        imageRange(sec.VirtualAddress,std::max(sec.VirtualSize,sec.SizeOfRawData));
        if (uint64_t(sec.PointerToRawData)+sec.SizeOfRawData>file.size()) throw GuestStop("Truncated PE section");
        std::memcpy(ptr(imageBase+sec.VirtualAddress,sec.SizeOfRawData),file.data()+sec.PointerToRawData,sec.SizeOfRawData);
    }
    entry=imageBase+opt.AddressOfEntryPoint;
    // Guest VAs stay fixed; the 64-bit host base is ADDR's offset. No PE
    // relocation toward the host address and no executable x86 mapping.
    auto directory=opt.DataDirectory[PE_DIR_IMPORT];
    bindImports(imageBase,imageSize,directory.VirtualAddress,directory.Size);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest PE mapped: base=%08x size=%08x entry=%08x; %zu imports bound; %u lifted functions",
        imageBase,imageSize,entry,imports.size(),recomp_dispatch_count);
}
uint32_t callGuest(uint32_t va, const uint32_t* args, size_t count, uint32_t cleanup, bool threadStart) {
    State saved; saved.save();
    // Parallel guest threads: guest code runs without the machine; the runtime re-takes it on the
    // way back in (import/override trampolines). Restored to the same depth on return.
    const unsigned held=parallelGuest&&freeRunning?machine.releaseAll():0;
    try {
        recomp_func_t fn=recomp_lookup_manual(va);if(!fn)fn=recomp_lookup(va);
        if (!fn) fn=recomp_lookup_import(va);
        if (!fn) throw GuestStop("No guest function at " + hex(va));
        for (size_t i=count;i--;) { g_esp-=4; write32(g_esp,args[i]); }
        g_esp-=4; write32(g_esp,RECOMP_RETADDR);
        fn(); g_esp+=cleanup;
        if (g_esp!=saved.esp&&!(threadStart&&g_esp+4u*uint32_t(count)==saved.esp)) throw GuestStop("Guest call stack imbalance at " + hex(va));
        uint32_t result=g_eax; machine.reacquire(held); saved.load(); return result;
    } catch (...) { machine.reacquire(held); saved.load(); throw; }
}
#include "guest_modules.inc"
void timerTestCallback(){timerTestCalls.push_back(arg(0));ret(0,5);}
void testGuestTimers(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name==name)return importBase+i*16;throw GuestStop("Timer test import unavailable");};
    uint32_t set=imported("winmm.dll!timeSetEvent"),kill=imported("winmm.dll!timeKillEvent");
    auto wait=[&](unsigned count){auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(timerTestCalls.size()<count){if(std::chrono::steady_clock::now()>until)throw GuestStop("Timer callback deadline failed");checkGuestProgress();yieldGuestMachine(1);}};
    timerTestActive=true;timerTestCalls.clear();
    try{
        auto first=callGuest(set,{2,0,timerTestAddress,0,0x100});if(!first)throw GuestStop("Timer creation failed");wait(1);
        if(timerTestCalls[0]!=first||timerHandles.count(first))throw GuestStop("One-shot timer retirement failed");
        auto worker=timerWorker->id;
        auto periodic=callGuest(set,{2,0,timerTestAddress,0,0x101});wait(3);
        if(callGuest(kill,{periodic})||timerHandles.count(periodic))throw GuestStop("Periodic timer cancellation failed");
        size_t after=timerTestCalls.size();auto cancelled=callGuest(set,{20,0,timerTestAddress,0,0x100});
        if(callGuest(kill,{cancelled}))throw GuestStop("Pending timer cancellation failed");
        auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(25);while(std::chrono::steady_clock::now()<until){checkGuestProgress();yieldGuestMachine(1);}
        if(timerTestCalls.size()!=after||!timerHandles.empty()||timerWorker->id!=worker||guestThreads.size()!=1)throw GuestStop("Timer cancellation/worker reuse failed");
        timerTestActive=false;
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Timer worker passed: one-shot, periodic, synchronous cancel, pending cancel, retirement and single-thread reuse");
    }catch(...){timerTestActive=false;for(auto& timer:timerHandles)timer.second->cancelled=true;timerHandles.clear();throw;}
}
void testGuestFileTime(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name==name)return importBase+i*16;return 0u;};
    uint32_t local=imported("kernel32.dll!FileTimeToLocalFileTime"),system=imported("kernel32.dll!FileTimeToSystemTime");
    if(!local||!system){  // the self-check drives the game's own imports; Speed.exe has neither
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest FILETIME self-check skipped: the game imports no FILETIME conversion");
        return;
    }
    uint32_t data=allocate(48);uint64_t epoch=116444736000000000ull+1234ull*10000ull;
    std::memset(ptr(data,48),0xa5,48);std::memcpy(ptr(data,8),&epoch,8);
    bool okay=callGuest(local,{data,data+8})==1&&read32(data+8)==uint32_t(epoch)&&read32(data+12)==uint32_t(epoch>>32)
        &&callGuest(system,{data+8,data+16})==1;
    const uint16_t expected[]={1970,1,4,1,0,0,1,234};
    okay=okay&&!std::memcmp(ptr(data+16,16),expected,16)&&read32(data+32)==0xa5a5a5a5u;
    okay=okay&&callGuest(local,{data,data})==0;
    uint64_t invalid=0x8000000000000000ull;std::memcpy(ptr(data,8),&invalid,8);
    okay=okay&&callGuest(system,{data,data+16})==0;
    heaps->free(GuestHeaps::process,data);
    if(!okay)throw GuestStop("Guest file-time conversion self-check failed");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest UTC FILETIME conversions passed: epoch, milliseconds, guards and invalid input");
}
void testGuestCriticalSections(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name=="kernel32.dll!"+name)return importBase+i*16;throw GuestStop("Critical-section import unavailable");};
    uint32_t initialize=imported("InitializeCriticalSection"),enter=imported("EnterCriticalSection"),leave=imported("LeaveCriticalSection"),remove=imported("DeleteCriticalSection");
    uint32_t data=allocate(32);std::memset(ptr(data,32),0xa5,32);
    callGuest(initialize,{data});callGuest(enter,{data});callGuest(enter,{data});
    bool okay=read32(data+8)==2&&read32(data+12)==guestThreadId&&read32(data+24)==0xa5a5a5a5u;
    callGuest(remove,{data});okay=okay&&criticalSections.count(data)&&pendingCriticalDeletes.count(data);
    callGuest(leave,{data});okay=okay&&read32(data+8)==1&&criticalSections.count(data);
    callGuest(leave,{data});okay=okay&&!criticalSections.count(data)&&!pendingCriticalDeletes.count(data)&&read32(data+8)==0;
    callGuest(initialize,{data});callGuest(remove,{data});okay=okay&&!criticalSections.count(data);
    heaps->free(GuestHeaps::process,data);

    // Deadlock cycle resolution test: Thread 1 holds csA and requests csB; Worker 9001 holds csB and requests csA.
    uint32_t csA=allocate(32),csB=allocate(32);
    callGuest(initialize,{csA});callGuest(initialize,{csB});
    callGuest(enter,{csA});
    std::atomic<bool> workerHoldsB{false},workerDone{false};
    std::thread deadlockWorker([&]{
        std::unique_lock<MachineMutex> lease(machine);machineLease=&lease;guestThreadId=9001;
        uint32_t workerStack=allocate(64*1024);
        State fresh;fresh.esp=workerStack+64*1024-64;fresh.load();
        callGuest(enter,{csB});
        workerHoldsB=true;
        notifyGuestSync();
        callGuest(enter,{csA});
        callGuest(leave,{csA});
        callGuest(leave,{csB});
        heaps->free(GuestHeaps::process,workerStack);
        forgetThreadTrace();
        workerDone=true;
        notifyGuestSync();
        machineLease=nullptr;
    });
    while(!workerHoldsB.load()){checkGuestProgress();waitGuestSync(guestSyncGeneration.load(),5);}
    while(true){
        {
            std::lock_guard<std::mutex> lock(threadTracesMutex);
            auto it=threadTraces.find(9001);
            if(it!=threadTraces.end()&&it->second){
                const char* w=it->second->wait.load();
                if(w&&!std::strcmp(w,"EnterCriticalSection")&&it->second->handle.load()==csA)break;
            }
        }
        checkGuestProgress();waitGuestSync(guestSyncGeneration.load(),5);
    }
    callGuest(enter,{csB});
    bool deadlockPassed=read32(csA+12)==guestThreadId&&read32(csB+12)==guestThreadId;
    callGuest(leave,{csB});
    callGuest(leave,{csA});
    machineLease->unlock();
    deadlockWorker.join();
    machineLease->lock();
    callGuest(remove,{csA});callGuest(remove,{csB});
    heaps->free(GuestHeaps::process,csA);heaps->free(GuestHeaps::process,csB);
    okay=okay&&deadlockPassed&&workerDone.load();

    if(!okay)throw GuestStop("Guest critical-section retirement self-check failed");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest critical sections passed recursion, owner, guards, deferred retirement and reinitialization");
}
} // namespace
// QueryPerformanceCounter for native code (src/runtime/native_math.c: sub_0043BE50), with the busy-wait
// detection of the import.
extern "C" uint64_t nfsQpcNow(){return qpcRead();}

#ifdef NFS_SERVER_DLL
extern "C" const recomp_dispatch_entry_t server_recomp_dispatch_table[];
extern "C" const uint32_t server_recomp_dispatch_count;
#endif
namespace {
// Every indirect call and virtual method resolves its target here, so use an
// open-addressing table (built once, read-only afterwards) instead of a binary
// search over ~28k entries with a cache miss per step.
#include "guest_asset_guards.inc"
struct DispatchHash {
    static constexpr uint32_t slots=1u<<16;
    std::unique_ptr<uint32_t[]> keys{new uint32_t[slots]()};
    std::unique_ptr<recomp_func_t[]> funcs{new recomp_func_t[slots]()};
    static uint32_t slot(uint32_t va){return (va*0x9e3779b1u)>>16;}
    void add(const recomp_dispatch_entry_t* table,uint32_t count){
        for(uint32_t i=0;i<count;++i){
            uint32_t va=table[i].address,s=slot(va);
            if(va-0x7c000000u<0x04000000u)throw GuestStop("Lifted function overlaps reserved native dispatch stubs");
            while(keys[s])s=(s+1)&(slots-1);
            keys[s]=va;funcs[s]=table[i].func;
        }
    }
    DispatchHash(){
        static_assert(slots>2*(27742+1531),"dispatch hash load factor");
        add(recomp_dispatch_table,recomp_dispatch_count);
#ifdef NFS_SERVER_DLL
        add(server_recomp_dispatch_table,server_recomp_dispatch_count);
#endif
        uint32_t used=0;for(uint32_t i=0;i<slots;++i)used+=keys[i]!=0;
        if(used>slots/2)throw GuestStop("Dispatch hash over half full: "+std::to_string(used));
    }
    recomp_func_t find(uint32_t va)const{
        if(!va)return nullptr;
        for(uint32_t s=slot(va);keys[s];s=(s+1)&(slots-1))if(keys[s]==va)return funcs[s];
        return nullptr;
    }
};
}
extern "C" void nfs_validate_asset_entry(uint32_t va){
    if(va==0x00447c00u)validateResourceChunks(arg(2),arg(3));
    else if(va==0x0059fb80u)validateLanguageTable();
}
extern "C" recomp_func_t recomp_lookup(uint32_t va) {
    // Sound/input/D3D COM and import stubs resolve in recomp_lookup_import.
    // Avoid an always-failing hash probe on every native method call. The
    // table constructor checks that no lifted module occupies this range.
    if(va-0x7c000000u<0x04000000u)return nullptr;
    static const DispatchHash hash;
    return hash.find(va);
}
extern "C" recomp_func_t nfs_abk_lookup(uint32_t va);  // src/runtime/nfs_abk.h
extern "C" int nfs_guest_readable(uint32_t va,uint32_t n){return memory&&memory->readable(va,n);}
// Parallel guest threads: runtime functions reached from lifted code run under the machine. The lookup
// takes it and returns this trampoline, which runs the function and releases it (the lookup's side
// effects -- method tokens -- stay protected until the function has used them).
thread_local recomp_func_t pendingRuntimeCall=nullptr;
void lockedRuntimeTrampoline(){
    recomp_func_t fn=pendingRuntimeCall;
    try{fn();}catch(...){machine.unlock();throw;}
    machine.unlock();
}
recomp_func_t lookupManualUnlocked(uint32_t va){
    if(timerTestActive&&va==timerTestAddress)return timerTestCallback;
    // Routines of EA's sound banks run from the heap: their lifted copies (nfs_abk.h).
    if((va>=GuestHeaps::firstArena&&va<GuestHeaps::firstArenaEnd)||(va>=GuestHeaps::secondArena&&va-GuestHeaps::secondArena<GuestHeaps::secondArenaSize))
        if(recomp_func_t fn=nfs_abk_lookup(va))return fn;
    return nfs_override_lookup(va);
}
extern "C" recomp_func_t nfs_abk_lookup_cached(uint32_t va);  // src/runtime/abk_code.c: hit path only
extern "C" recomp_func_t recomp_lookup_manual(uint32_t va) {
    // Threads that take turns hold the machine already (taking it again would leave the handler's own
    // yields unable to release it): only free-running threads go through the trampoline.
    if(!parallelGuest||!freeRunning)return lookupManualUnlocked(va);
    // Cheap rejection first: most indirect calls are lifted functions or imports.
    const bool arena=(va>=GuestHeaps::firstArena&&va<GuestHeaps::firstArenaEnd)||
        (va>=GuestHeaps::secondArena&&va-GuestHeaps::secondArena<GuestHeaps::secondArenaSize);
    if(arena&&!timerTestActive){
        // Sound-bank routines in the guest heap are lifted code: they run without the machine. The
        // lookup's cache hit compares the routine's bytes and needs no page map (heap arenas stay
        // committed); a miss reads the page map, which only the machine protects.
        if(recomp_func_t fn=nfs_abk_lookup_cached(va))return fn;
        machine.lock();recomp_func_t fn;
        try{fn=nfs_abk_lookup(va);}catch(...){machine.unlock();throw;}
        machine.unlock();
        return fn;
    }
    if(!arena&&!timerTestActive&&!nfs_override_lookup(va))return nullptr;
    machine.lock();
    recomp_func_t fn;
    try{fn=lookupManualUnlocked(va);}catch(...){machine.unlock();throw;}
    if(!fn){machine.unlock();return nullptr;}
    pendingRuntimeCall=fn;return lockedRuntimeTrampoline;
}
recomp_func_t lookupImportLocked(uint32_t va);
std::unordered_map<uint32_t,uint64_t> freeLockedCalls;
std::mutex freeLockedMutex;
std::string importName(uint32_t va);
extern "C" void nfsReportFreeLockedCalls(unsigned frames){
    std::vector<std::pair<uint64_t,uint32_t>> v;
    {std::lock_guard<std::mutex> l(freeLockedMutex);for(auto& e:freeLockedCalls)v.push_back({e.second,e.first});freeLockedCalls.clear();}
    std::sort(v.rbegin(),v.rend());
    std::string line="[NFSU2 WAIT] sound thread runtime calls needing the machine (per frame):";
    char buf[96];for(size_t i=0;i<v.size()&&i<10;++i){std::snprintf(buf,sizeof buf," %s=%.1f",importName(v[i].second).c_str(),double(v[i].first)/frames);line+=buf;}
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","%s",line.c_str());
}
std::string importName(uint32_t va){
    if(va>=importBase&&!((va-importBase)%16)){uint32_t i=(va-importBase)/16;if(i<imports.size())return imports[i].name;}
    char b[32];std::snprintf(b,sizeof b,"%08x",va);return b;
}
extern "C" recomp_func_t recomp_lookup_import(uint32_t va) {
    if(!parallelGuest||!freeRunning)return lookupImportLocked(va);
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    // D3D9: queued calls need no machine; d3dDispatch takes it only for synchronous calls.
    if(va>=0x7e001000u&&va<0x7e010000u&&!(va%16)){d3dToken=va;return d3dDispatch;}
#endif
    // DirectSound buffer methods (GetStatus, GetCurrentPosition, Lock, Unlock, ...): soundStateMutex, no machine.
    // QueryInterface/AddRef/Release and the DirectSound object itself allocate guest memory: machine.
    if(va>=soundMethodBase+0x2000&&va<soundMethodBase+0x3000&&!(va%16)&&((va-soundMethodBase-0x2000)/16)>2){soundToken=va;return soundDispatch;}
    if(va>=importBase&&!((va-importBase)%16)){
        uint32_t index=(va-importBase)/16;
        if(index<imports.size()&&imports[index].freeFunction){lastImport=va;return imports[index].freeFunction;}
        if(index<imports.size()&&imports[index].lockFree){lastImport=va;++imports[index].calls;return imports[index].function;}
    }
    machine.lock();
    {std::lock_guard<std::mutex> l(freeLockedMutex);++freeLockedCalls[va];}  // diagnostics: which runtime calls make free threads take the machine
    recomp_func_t fn;
    try{fn=lookupImportLocked(va);}catch(...){machine.unlock();throw;}
    if(!fn){machine.unlock();return nullptr;}
    pendingRuntimeCall=fn;return lockedRuntimeTrampoline;
}
recomp_func_t lookupImportLocked(uint32_t va) {
    checkGuestProgress();
    if(va>=soundMethodBase+0x1000&&va<soundMethodBase+0x3000&&!(va%16)){soundToken=va;return soundDispatch;}
    if(va>=inputMethodBase+0x1000&&va<inputMethodBase+0x5000&&!(va%16)){inputToken=va;return inputDispatch;}  // kinds 1-4: DirectInput, keyboard, mouse, gamepad
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    if(va>=0x7e001000u&&va<0x7e010000u&&!(va%16)){d3dToken=va;return d3dDispatch;}
#endif
    if (va<importBase || (va-importBase)%16) return nullptr;
    uint32_t index=(va-importBase)/16;
    if (index>=imports.size()) return nullptr;
    lastImport=va;
    if(++imports[index].calls<=3)
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest import %s from %08x",imports[index].name.c_str(),g_cur_func);
    return imports[index].function;
}
extern "C" void recomp_unresolved(const char* kind,uint32_t va,uint32_t from) {
    throw GuestStop(std::string(kind)+" unresolved "+hex(va)+" from "+hex(from));
}
extern "C" void recomp_unimpl(uint32_t va,const char* what) {
    throw GuestStop("Instruction unsupported at "+hex(va)+": "+what);
}
extern "C" void recomp_dump_trace(const char*) {}
// (NFSU2 rewrites its six-resolution menu here and intercepts the selector. Speed.exe 1.4: the R36S
// build answers the game's racing-resolution query natively; see nfs_r36s_racing_resolution below.)
extern "C" int nfs_android_resolution(){return 0;}

std::string connectGuestRuntime(const char* executable) {
    std::unique_lock<MachineMutex> guard(machine);machineLease=&guard;
    if (memory) return "Runtime guest ya inicializado";
    State original; original.save();
    try {
        stopRequested=false;
        // Interactive sessions (Android, R36S, Desktop) stop through player exit or signal cancellation.
        // Optional timeout can be enforced if NFSU2_BOOT_TIMEOUT is explicitly set in seconds (e.g. for CI/benchmarking).
        const char* timeoutEnv = std::getenv("NFSU2_BOOT_TIMEOUT");
        if (timeoutEnv && *timeoutEnv) {
            int seconds = std::atoi(timeoutEnv);
            bootDeadline = (seconds > 0) ? (std::chrono::steady_clock::now() + std::chrono::seconds(seconds))
                                         : std::chrono::steady_clock::time_point::max();
        } else {
            bootDeadline = std::chrono::steady_clock::time_point::max();
        }
        recomp_yield_hook=checkGuestProgress;
        memory=std::make_unique<GuestMemory>();
        hostGameRoot=std::string(executable); hostGameRoot=hostGameRoot.substr(0,hostGameRoot.find_last_of('/'));
        g_mem_base=reinterpret_cast<ptrdiff_t>(memory->base());
        testGuestHeaps(*memory);
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Guest heaps passed ownership, realloc preservation, zero tail, in-place failure and reuse checks");
        heaps=std::make_unique<GuestHeaps>(*memory);
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        configureD3D9Bridge(memory->base(),allocate,freeD3DGuest);
#endif
        loadImage(executable);
#if defined(NFS_R36S)
        // The game renders at the size main.cpp chose for this panel (chooseResolution: the panel's own
        // size, 4:3 inside narrower panels, or NFSU_RENDER/NFSU_RENDER_SCALE), whatever its resolution
        // menu holds; wider than 4:3 turns on the widescreen fix (hor+ FOV, HUD at the edges, minimap,
        // mirror, cropped FMVs).
        // No pointing device: hide the front-end mouse arrow (NFSU_SHOW_CURSOR=1 keeps it).
        nfs_widescreen_hide_cursor(std::getenv("NFSU2_SHOW_CURSOR")==nullptr);
        if(renderWidth.load()&&renderHeight.load()){
            nfs_widescreen_set_render_size(renderWidth.load(),renderHeight.load());
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Game render size %ux%u (the resolution menu is overridden)",renderWidth.load(),renderHeight.load());
        }
        if(guestWidescreen&&renderWidth.load()&&renderHeight.load()){
            nfs_widescreen_init(hostGameRoot.c_str());
            const float aspect=float(renderWidth.load())/float(renderHeight.load());
            nfs_widescreen_set_display_aspect(aspect);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Widescreen: aspect %.3f, 3D FOV, HUD, minimap, mirror and FMVs adapted",aspect);
        }else NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Widescreen off: original 4:3 geometry");
#else
        nfs_frontend_cursor_units(1);  // touch hands out the cursor in front-end units
        if(guestWidescreen){
            nfs_widescreen_init(hostGameRoot.c_str());
            // Landscape surface: the long side over the short one.
            float longSide=float(std::max(displayWidth.load(),displayHeight.load())),shortSide=float(std::min(displayWidth.load(),displayHeight.load()));
            float aspect=shortSide>0?longSide/shortSide:16.f/9.f;
            nfs_widescreen_set_display_aspect(aspect);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Widescreen: render width follows the display aspect %.3f, 3D FOV corrected (front end and HUD still 4:3)",aspect);
        }else NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Widescreen disabled: original 4:3 geometry");
#endif
        memory->commit(stackBase,stackSize); memory->commit(tib,0x1000);
        State fresh; fresh.esp=stackBase+stackSize-64; fresh.fs=tib; fresh.load();
        write32(tib,0xffffffff); write32(tib+4,stackBase+stackSize); write32(tib+8,stackBase); write32(tib+0x18,tib);
        testGuestScheduling();
        testGuestInput();
        testGuestSoundCaps();
        testGuestTimers();
        testGuestFileTime();
        testGuestCriticalSections();
        testGuestAssetGuards();
        // Restore a clean CPU state before executing the real CRT entry.
        fresh.load();
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Entering actual lifted CRT entry %08x",entry);
#if defined(NFS_PARALLEL_GUEST)
        {
            const char* e=std::getenv("NFSU2_PARALLEL_GUEST");
            if(!(e&&e[0]=='0')){
                // Imports that touch only the calling thread's state or a clock run without the machine.
                static const char* lockFree[]={"!GetLastError","!SetLastError","!TlsGetValue","!TlsSetValue","!GetCurrentThreadId",
                    "!GetTickCount","!timeGetTime","!QueryPerformanceCounter","!QueryPerformanceFrequency"};
                unsigned marked=0;
                for(auto& import:imports)for(const char* suffix:lockFree){
                    const size_t n=std::strlen(suffix);
                    if(import.name.size()>=n&&import.name.compare(import.name.size()-n,n,suffix)==0){import.lockFree=true;++marked;}
                }
                // Mutexes and Sleep: their own locking / no shared state (guest_threads.inc). Free threads only.
                for(auto& import:imports){
                    auto ends=[&](const char* suffix){const size_t n=std::strlen(suffix);return import.name.size()>=n&&import.name.compare(import.name.size()-n,n,suffix)==0;};
                    if(ends("!WaitForSingleObject"))import.freeFunction=waitSingleFree;
                    else if(ends("!ReleaseMutex"))import.freeFunction=releaseMutex;
                    else if(ends("!Sleep"))import.freeFunction=sleepShim;
                    else if(ends("!EnterCriticalSection"))import.freeFunction=enterCritical;
                    else if(ends("!LeaveCriticalSection"))import.freeFunction=leaveCritical;
                    if(import.freeFunction)++marked;
                }
                const char* list=std::getenv("NFSU2_PARALLEL_THREADS");std::string entries=list?list:NFS_DEFAULT_FREE_THREADS;
                for(size_t at=0;at<entries.size();){size_t end=entries.find_first_of(",; ",at);if(end==std::string::npos)end=entries.size();
                    if(end>at)freeThreadEntries.push_back(static_cast<uint32_t>(std::strtoul(entries.substr(at,end-at).c_str(),nullptr,16)));at=end+1;}
                parallelGuest=true;
                NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Parallel guest threads enabled (%u lock-free imports; NFSU2_PARALLEL_GUEST=0 disables)",marked);
            }
        }
#endif
        uint32_t result=callGuest(entry,{});
        joinGuestThreads(guard);machineLease=nullptr;shutdownGuestSound();
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        shutdownD3D9Bridge();
#endif
        original.load();
        return "Entrada guest terminada: "+std::to_string(result);
    } catch (const std::exception& e) {
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU1","Guest integration boundary: %s",e.what());
        joinGuestThreads(guard);machineLease=nullptr;shutdownGuestSound();
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        shutdownD3D9Bridge();
#endif
        original.load();
        return std::string("Arranque guest detenido: ")+e.what();
    }
}
void requestGuestRuntimeStop() {stopRequested=true;}
// Quitting (Select+Start, the window closing): wait until the game has no file open for writing (a save or
// profile being written), at most timeoutMs, then fsync whatever is open and sync the whole card. The guest
// is held at a runtime call while the handles are inspected (machine lock), so no write is half done; ending
// the process in the middle of a save, without a flush, is what left saves and card filesystems damaged.
void quiesceGuestFilesForExit(int timeoutMs){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    int writable=-1;
    for(;;){
        {
            std::unique_lock<MachineMutex> lock(machine);
            writable=0;
            for(auto& entry:fileHandles){
                const int fl=::fcntl(entry.second,F_GETFL);
                if(fl>=0&&(fl&O_ACCMODE)!=O_RDONLY)++writable;
            }
            if(!writable||std::chrono::steady_clock::now()>=deadline){
                for(auto& entry:fileHandles)::fsync(entry.second);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::sync();
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Exit: files flushed (%d still open for writing), card synced",writable);
}
#if defined(__ANDROID__)
void setGuestPaused(bool paused) {
    // The AAudio stream keeps pulling from looping voices while the guest is
    // paused: in the background the last mixed sound repeated until the app
    // closed. Pause the output with the guest and restart it on resume.
    bool previous=guestPaused.exchange(paused);
    if(previous!=paused&&soundStream){
        aaudio_result_t status=paused?AAudioStream_requestPause(soundStream):AAudioStream_requestStart(soundStream);
        if(status!=AAUDIO_OK)NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFS","Audio %s failed: %d",paused?"pause":"resume",int(status));
    }
}
#else
void setGuestPaused(bool paused) {guestPaused=paused;}
#endif
bool guestDrivingControls() {return inputDrivingMode.load();}
extern "C" int nfsGuestDriving(){return inputDrivingMode.load()?1:0;}
void setGuestPad(const float axes[6],uint32_t buttons,int hatX,int hatY){
    std::lock_guard<std::mutex> lock(inputMutex);
    for(unsigned i=0;i<6;++i)inputPad.axis[i]=std::max(-1.f,std::min(1.f,axes[i]));
    inputPad.buttons=buttons;inputPad.hatX=hatX;inputPad.hatY=hatY;
}
void setGuestPadConnected(bool connected){
    std::lock_guard<std::mutex> lock(inputMutex);
    if(connected!=inputPadConnected)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Gamepad %s",connected?"connected":"disconnected");
    inputPadConnected=connected;if(!connected)inputPad=InputPad{};
}
bool guestGamepadInUse(){return inputPadAcquired.load()>0;}
void setGuestDisplaySize(unsigned width,unsigned height){displayWidth=width;displayHeight=height;}
void setGuestResolution(unsigned width,unsigned height){
    std::lock_guard<MachineMutex> lock(machine);if(memory)return;
    if(width<320||height<240||width>8192||height>8192){renderWidth=renderHeight=0;return;}
    renderWidth=width;renderHeight=height;
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    setD3D9RenderSize(width,height);
#endif
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Selected render resolution %ux%u",width,height);
}
void setGuestFrameLimit(unsigned framesPerSecond){
    frameLimit=(framesPerSecond>=30&&framesPerSecond<=240)?framesPerSecond:0;
    // The game paces itself to 1/60 s; let it run at the cap (uncapped: the 120 Hz display).
    nfs_widescreen_set_frame_rate(frameLimit.load()?frameLimit.load():120);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Android frame limit %u FPS (0 means uncapped)",frameLimit.load());
}
void setGuestKey(unsigned scan,bool down){if(scan>=256)return;std::lock_guard<std::mutex> lock(inputMutex);if(bool(inputKeys[scan])==down)return;inputKeys[scan]=down?0x80:0;recordInputKey(scan,down);enqueueGuestKey(scan,down);}
void clearGuestInput(){std::lock_guard<std::mutex> lock(inputMutex);for(unsigned scan=0;scan<256;++scan)if(inputKeys[scan]){recordInputKey(scan,false);enqueueGuestKey(scan,false);}inputKeys.fill(0);inputMouseButtons.fill(0);inputMouseX=inputMouseY=inputMouseZ=0;touchButtons.clear();touchMoved=false;}
void setGuestPointer(float x,float y,int phase){
    guestPointerX=std::clamp(x,0.f,1.f);guestPointerY=std::clamp(y,0.f,1.f);
    {std::lock_guard<std::mutex> lock(inputMutex);touchMoved=true;if(phase!=1)touchButtons.push_back(phase==0?0x80:0);}
    enqueueGuestPointer(phase);
}
void typeGuestText(const std::vector<uint32_t>& characters){enqueueGuestText(characters);}
void setGuestOnlineServer(const std::string& host){std::lock_guard<MachineMutex> lock(machine);if(!memory&&!host.empty())onlineServer=host;}
void setGuestWidescreen(bool enabled){std::lock_guard<MachineMutex> lock(machine);if(!memory)guestWidescreen=enabled;}
void setGuestLanguage(const char* language){
    std::lock_guard<MachineMutex> lock(machine);if(memory)return;
    if(language&&(std::strcmp(language,"english")==0||std::strcmp(language,"en")==0||std::strcmp(language,"English UK")==0||std::strcmp(language,"English US")==0))
        language="English";
    for(const char* known:{"Spanish","English","French","German","Italian","Dutch","Swedish","Korean","Chinese"})
        if(language&&std::strcmp(known,language)==0){guestLanguage=known;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU1","Selected guest language: %s",known);return;}
    guestLanguage="Spanish";
}

// D3D9 bridge (R36S render thread): wait for the render thread with the guest machine released.
void d3d9BridgeWaitWithoutMachine(const std::function<void()>& wait){
    if(!machineLease||!machineLease->owns_lock()||!machine.heldByMe()){wait();return;}
    State saved;saved.save();machineLease->unlock();
    try{wait();}catch(...){machineLease->lock();saved.load();throw;}
    machineLease->lock();saved.load();
}
extern "C" bool nfsParallelGuest(){return parallelGuest;}

#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
// Cacheable D3D9 COM entry: the method comes from the call's VA (nfs_icall_va), not from a lookup side
// effect, so call sites can cache it (arm64_guest.h NFS_ICALL_RESOLVE).
// Indirect calls to D3D9 COM methods land here (cached per call site). Queued/filtered calls (~28k per race
// frame) return without going through d3dDispatch; everything else (Present, synchronous calls) does.
extern "C" uint32_t getD3D9PresentCount();
static void d3dDispatchFromCallVa(){
    const uint32_t token=nfs_icall_va;
    // NFSU2_D3D_GENERIC_STATS=1: which methods still take this generic path (logged every 600 frames)
    static const bool genericStats=std::getenv("NFSU2_D3D_GENERIC_STATS")!=nullptr;
    if(__builtin_expect(genericStats,0)){
        static std::unordered_map<uint32_t,uint64_t> counts;static uint32_t lastReport=0;++counts[token];
        const uint32_t frame=getD3D9PresentCount();
        if(frame>=lastReport+600){lastReport=frame;std::vector<std::pair<uint64_t,uint32_t>> rows;for(auto& c:counts)rows.push_back({c.second,c.first});
            std::sort(rows.rbegin(),rows.rend());std::string line;
            for(size_t i=0;i<rows.size()&&i<12;++i)line+=" k"+std::to_string((rows[i].second-0x7e000000u)/4096)+"/m"+std::to_string(((rows[i].second-0x7e000000u)%4096)/16)+"="+std::to_string(rows[i].first/600);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","[D3D9 generic] calls per frame:%s",line.c_str());counts.clear();}
    }
    if(parallelGuest&&token!=0x7e002110u){
        uint32_t args[12];std::memcpy(args,ptr(g_esp+4,48),48);uint32_t count=0,result;
        if(dispatchGuestD3D9TryDefer(token,args,&count,&result)){
            if(__builtin_expect(nfs_fx_cap_depth!=0,0))nfs_fx_capture(token,args,count,1);
            qpcStreak=0;g_eax=result;g_esp+=4+count*4;return;}
    }
    d3dToken=token;d3dDispatch();}
// Effect state memo (src/runtime/fx_cache.c): replay a recorded deferred device call.
extern "C" int nfsD3dReplay(uint32_t token,const uint32_t* args){
    uint32_t a[12]={};std::memcpy(a,args,9*4);uint32_t count=0,result=0;
    return dispatchGuestD3D9TryDefer(token,a,&count,&result)?1:0;
}
// Device method calls from the lifted code, one handler per method (D3D9 device methods 0..118): the arguments stay on
// the guest stack and go straight to the bridge's shortcut (d3d9_render_thread.inc); anything it does not take, and
// calls made while the effect memo captures (nfs_fx_cap_depth), take the generic path.
extern "C" int nfsD3dShortcutsEnabled();
extern "C" int nfsD3dDeviceCallShortcut(uint32_t method,const uint32_t* args,uint32_t* count);
template<uint32_t Method>void d3dDeviceCallShortcut(){
    uint32_t count=0;const auto args=static_cast<const uint32_t*>(ptr(g_esp+4,36));
    if(nfsD3dDeviceCallShortcut(Method,args,&count)){
        if(__builtin_expect(nfs_fx_cap_depth!=0,0))nfs_fx_capture(nfs_icall_va,args,count,1);   // as the generic path
        qpcStreak=0;g_eax=0;g_esp+=4+count*4;return;}
    d3dDispatchFromCallVa();
}
template<std::size_t... Method>constexpr std::array<recomp_func_t,sizeof...(Method)> deviceShortcutTable(std::index_sequence<Method...>){
    return {{&d3dDeviceCallShortcut<uint32_t(Method)>...}};
}
// AddRef (1) / Release (2) of textures, cube textures, vertex and index buffers (kinds 3, 4, 6, 7).
extern "C" int nfsD3dReferenceShortcut(uint32_t kind,uint32_t method,const uint32_t* args,uint32_t* result);
template<uint32_t Kind,uint32_t Method>void d3dReferenceShortcut(){
    uint32_t result=0;const auto args=static_cast<const uint32_t*>(ptr(g_esp+4,36));
    if(nfsD3dReferenceShortcut(Kind,Method,args,&result)){
        if(__builtin_expect(nfs_fx_cap_depth!=0,0))nfs_fx_capture(nfs_icall_va,args,1,1);   // as the generic path
        qpcStreak=0;g_eax=result;g_esp+=8;return;}
    d3dDispatchFromCallVa();
}
recomp_func_t referenceShortcut(uint32_t kind,uint32_t method){
    switch(kind*4+method){
    case 3*4+1:return d3dReferenceShortcut<3,1>;case 3*4+2:return d3dReferenceShortcut<3,2>;
    case 4*4+1:return d3dReferenceShortcut<4,1>;case 4*4+2:return d3dReferenceShortcut<4,2>;
    case 6*4+1:return d3dReferenceShortcut<6,1>;case 6*4+2:return d3dReferenceShortcut<6,2>;
    case 7*4+1:return d3dReferenceShortcut<7,1>;case 7*4+2:return d3dReferenceShortcut<7,2>;
    default:return nullptr;
    }
}
constexpr uint32_t deviceMethodCount=119;
const std::array<recomp_func_t,deviceMethodCount> deviceCallShortcuts=deviceShortcutTable(std::make_index_sequence<deviceMethodCount>());
extern "C" recomp_func_t recomp_lookup_cacheable(uint32_t va){
    if(va>=0x7e001000u&&va<0x7e010000u&&!(va%16)){
        checkGuestProgress();
        const uint32_t kind=(va-0x7e000000u)/4096,method=((va-0x7e000000u)%4096)/16;
        if(kind==2&&method<deviceMethodCount&&nfsD3dShortcutsEnabled())return deviceCallShortcuts[method];
        if((method==1||method==2)&&nfsD3dShortcutsEnabled())if(recomp_func_t shortcut=referenceShortcut(kind,method))return shortcut;
        return d3dDispatchFromCallVa;
    }
    return nullptr;
}
#else
extern "C" recomp_func_t recomp_lookup_cacheable(uint32_t){return nullptr;}
#endif
