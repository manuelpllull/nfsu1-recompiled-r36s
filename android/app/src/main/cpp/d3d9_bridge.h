#pragma once
#include <cstdint>
#if (defined(NFS_R36S) || defined(NFS_GLES)) && !defined(__ANDROID__)
typedef void ANativeWindow;
#else
struct ANativeWindow;
#endif
void configureD3D9Bridge(void* guestBase,uint32_t (*allocator)(uint32_t),void (*deallocator)(uint32_t));
void shutdownD3D9Bridge();
void setD3D9HostWindow(ANativeWindow* window);
void setD3D9RenderSize(unsigned width,unsigned height);
void setD3D9Renderer(const char* renderer);
bool isD3D9Gles();
// Launcher compatibility options; call before the game creates its device.
void setD3D9Compatibility(bool cpuTextures,unsigned backBuffers);
bool d3d9RequestsSurface();
void acknowledgeD3D9Surface(bool available);
uint32_t createGuestD3D9(uint32_t sdk);
uint32_t dispatchGuestD3D9(uint32_t token,const uint32_t* args,uint32_t* argumentCount);
// R36S render thread: queue the call if it is deferrable (no guest machine needed); false otherwise.
bool dispatchGuestD3D9TryDefer(uint32_t token,const uint32_t* args,uint32_t* argumentCount,uint32_t* result);
#include <functional>
// Provided by the guest runtime: runs wait() with the guest machine released (other guest threads may
// run meanwhile). Used by the R36S render thread while the game waits for the previous frame.
void d3d9BridgeWaitWithoutMachine(const std::function<void()>& wait);
