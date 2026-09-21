// main.cpp — единая точка входа
// Компилируй ОДИН файл + imgui + memory/entity/camera headers
// cl /std:c++20 /O2 main.cpp imgui/*.cpp /I imgui
//    /link d3d11.lib dxgi.lib dwmapi.lib psapi.lib /SUBSYSTEM:WINDOWS /OUT:RustAdmin.exe

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <sstream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

// ═══════════════════════════════════════════════════════
//  OFFSETS — из script.json дампа Company Rust
// ═══════════════════════════════════════════════════════
namespace Off {
    constexpr uintptr_t Health        = 0x138;
    constexpr uintptr_t PosX          = 0x150;
    constexpr uintptr_t PosY          = 0x154;
    constexpr uintptr_t PosZ          = 0x158;
    constexpr uintptr_t DisplayName   = 0x248;
    constexpr uintptr_t UserID        = 0x230;
    constexpr uintptr_t MetabolismPtr = 0x160;
    constexpr uintptr_t IsDead        = 0x1A0;
    constexpr uintptr_t Calories      = 0x48;
    constexpr uintptr_t Hydration     = 0x60;
    constexpr uintptr_t Bleeding      = 0xA8;
    constexpr uintptr_t Radiation     = 0x90;
    constexpr uintptr_t CamFnOff      = 0x6B7D60;
    constexpr uintptr_t CamPX         = 0x90;
    constexpr uintptr_t CamPY         = 0x94;
    constexpr uintptr_t CamPZ         = 0x98;
    constexpr uintptr_t CamRX         = 0xA0;
    constexpr uintptr_t CamRY         = 0xA4;
    constexpr uintptr_t CamRZ         = 0xA8;
    constexpr uintptr_t CamRW         = 0xAC;
}

// ═══════════════════════════════════════════════════════
//  SHARED STATE — оба окна читают одно и то же
// ═══════════════════════════════════════════════════════
struct Vec3 { float x=0,y=0,z=0; };
struct Vec2 { float x=0,y=0; };
struct Quat { float x=0,y=0,z=0,w=1; };

enum class EType { Player, NPC, Loot, Container, Unknown };

struct Entity {
    uintptr_t   ptr       = 0;
    EType       type      = EType::Unknown;
    float       hp        = 0;
    Vec3        pos       = {};
    std::string name;
    float       dist      = 0;
    bool        lost      = false;
    bool        onScreen  = false;
    Vec2        screen    = {};
    float       calories  = 0;
    float       hydration = 0;
    float       radiation = 0;
    bool        bleeding  = false;
    std::deque<float> hpHist;
};

struct CameraData {
    Vec3 pos  = {};
    Quat rot  = {};
    float fov = 60.f;
};

struct ESPSettings {
    bool showPlayers    = true;
    bool showNPC        = true;
    bool showLoot       = true;
    bool showContainers = true;
    bool showBox        = true;
    bool showHP         = true;
    bool showName       = true;
    bool showDist       = true;
    bool showSkeleton   = true;
    bool showTrend      = true;
    bool showEdge       = true;
    float maxDist       = 600.f;
    float fov           = 60.f;
};

// Процесс в списке выбора
struct ProcInfo {
    DWORD       pid = 0;
    std::string name;
    bool        hasGameAssembly = false;   // подсветка настоящего Rust
};

struct SharedState {
    std::mutex          mtx;
    std::vector<Entity> entities;
    CameraData          camera;
    ESPSettings         settings;

    HANDLE              procHandle   = nullptr;
    DWORD               procPid      = 0;
    std::string         procName;
    uintptr_t           gameBase     = 0;

    // выбор процесса в UI
    std::mutex               procMtx;
    std::vector<ProcInfo>    procList;      // снимок запущенных процессов
    std::atomic<DWORD>       selectedPid  = 0;
    std::atomic<bool>        listing      = false;

    // счётчик чтений памяти (через CPU) — reads/sec
    std::atomic<uint64_t> memReadTotal = 0;   // всего успешных чтений
    std::atomic<uint64_t> memReadFail  = 0;   // всего ошибок чтения
    std::atomic<uint64_t> memBytes     = 0;   // всего прочитано байт
    std::atomic<double>   memReadRate  = 0.0;  // чтений/сек (обновляется раз в сек)
    std::atomic<double>   memMBs       = 0.0;  // МБ/сек

    std::atomic<bool>   attached     = false;
    std::atomic<bool>   scanning     = false;
    std::atomic<float>  scanPct      = 0.f;
    std::atomic<bool>   running      = true;

    std::vector<std::string> log;
    void Log(const std::string& s) {
        std::lock_guard<std::mutex> lk(mtx);
        char ts[32]; 
        SYSTEMTIME t; GetLocalTime(&t);
        snprintf(ts,sizeof(ts),"[%02d:%02d:%02d] ",t.wHour,t.wMinute,t.wSecond);
        log.push_back(ts + s);
        if (log.size() > 200) log.erase(log.begin());
    }
} G;

// ═══════════════════════════════════════════════════════
//  MEMORY HELPERS
// ═══════════════════════════════════════════════════════
template<typename T>
T Rpm(uintptr_t addr) {
    T v{};
    SIZE_T br = 0;
    if (ReadProcessMemory(G.procHandle,reinterpret_cast<LPCVOID>(addr),&v,sizeof(T),&br) && br==sizeof(T)) {
        G.memReadTotal.fetch_add(1,std::memory_order_relaxed);
        G.memBytes.fetch_add(br,std::memory_order_relaxed);
    } else {
        G.memReadFail.fetch_add(1,std::memory_order_relaxed);
    }
    return v;
}

float Rf32(uintptr_t a) {
    float v = Rpm<float>(a);
    return (std::isnan(v)||std::isinf(v)) ? 0.f : v;
}

uintptr_t Rptr(uintptr_t a) { return Rpm<uintptr_t>(a); }

std::string Rstr(uintptr_t addr, int maxLen=64) {
    if (!addr||addr<0x10000) return "";
    int len = Rpm<int>(addr+0x10);
    if (len<=0||len>256) return "";
    int n = min(len,maxLen);
    std::vector<wchar_t> buf(n+1,0);
    ReadProcessMemory(G.procHandle,reinterpret_cast<LPCVOID>(addr+0x14),
        buf.data(),n*2,nullptr);
    std::string r;
    for(int i=0;i<n&&buf[i];++i) r+=(buf[i]<128)?(char)buf[i]:'?';
    return r;
}

bool ValidPtr(uintptr_t p) { return p>0x10000&&p<0x7FFFFFFFFFFF; }

DWORD FindPID(const char* name) {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    PROCESSENTRY32 pe{sizeof(pe)};
    DWORD r=0;
    if(Process32First(snap,&pe)) do {
        if(_stricmp(pe.szExeFile,name)==0){r=pe.th32ProcessID;break;}
    } while(Process32Next(snap,&pe));
    CloseHandle(snap);
    return r;
}

uintptr_t FindModule(DWORD pid,const char* mod) {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    MODULEENTRY32 me{sizeof(me)};
    uintptr_t r=0;
    if(Module32First(snap,&me)) do {
        if(_stricmp(me.szModule,mod)==0){r=(uintptr_t)me.modBaseAddr;break;}
    } while(Module32Next(snap,&me));
    CloseHandle(snap);
    return r;
}

// Проверка: есть ли в процессе GameAssembly.dll (настоящий Rust)
bool HasModule(DWORD pid,const char* mod) {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(snap==INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me{sizeof(me)};
    bool found=false;
    if(Module32First(snap,&me)) do {
        if(_stricmp(me.szModule,mod)==0){found=true;break;}
    } while(Module32Next(snap,&me));
    CloseHandle(snap);
    return found;
}

// Снимок запущенных процессов для выбора в UI.
// Rust-кандидаты (RustClient.exe/rust.exe или с GameAssembly.dll) идут наверх.
void RefreshProcessList() {
    G.listing=true;
    std::vector<ProcInfo> list;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap!=INVALID_HANDLE_VALUE){
        PROCESSENTRY32 pe{sizeof(pe)};
        if(Process32First(snap,&pe)) do {
            if(pe.th32ProcessID<=4) continue;              // System / Idle
            ProcInfo pi;
            pi.pid  = pe.th32ProcessID;
            pi.name = pe.szExeFile;
            bool likelyRust = _stricmp(pi.name.c_str(),"RustClient.exe")==0
                           || _stricmp(pi.name.c_str(),"rust.exe")==0;
            // модульный чек дорогой — делаем только для вероятных кандидатов
            if(likelyRust) pi.hasGameAssembly = HasModule(pi.pid,"GameAssembly.dll");
            list.push_back(std::move(pi));
        } while(Process32Next(snap,&pe));
        CloseHandle(snap);
    }
    // Rust наверх, затем по имени
    std::sort(list.begin(),list.end(),[](const ProcInfo&a,const ProcInfo&b){
        int ra = a.hasGameAssembly?2 : (_stricmp(a.name.c_str(),"RustClient.exe")==0||_stricmp(a.name.c_str(),"rust.exe")==0?1:0);
        int rb = b.hasGameAssembly?2 : (_stricmp(b.name.c_str(),"RustClient.exe")==0||_stricmp(b.name.c_str(),"rust.exe")==0?1:0);
        if(ra!=rb) return ra>rb;
        return _stricmp(a.name.c_str(),b.name.c_str())<0;
    });
    {
        std::lock_guard<std::mutex> lk(G.procMtx);
        G.procList=std::move(list);
    }
    G.listing=false;
    G.Log("Process list refreshed ("+std::to_string(G.procList.size())+" processes)");
}

// ═══════════════════════════════════════════════════════
//  SCAN + REFRESH THREAD
// ═══════════════════════════════════════════════════════
EType ClassifyEntity(uintptr_t ptr, float hp) {
    if(hp>=1.f&&hp<=100.f) {
        uintptr_t np=Rptr(ptr+Off::DisplayName);
        auto name=Rstr(np);
        return (!name.empty()&&name.size()>=2) ? EType::Player : EType::NPC;
    }
    if(hp==0.f) return EType::Container;
    return EType::Unknown;
}

void ScanEntities() {
    G.scanning=true; G.scanPct=0.f;
    std::vector<Entity> found;

    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uintptr_t addr=(uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t end =(uintptr_t)si.lpMaximumApplicationAddress;
    constexpr size_t limit=300ull*1024*1024;
    size_t scanned=0;

    while(addr<end&&scanned<limit&&found.size()<64) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQueryEx(G.procHandle,(LPCVOID)addr,&mbi,sizeof(mbi)))
            {addr+=0x1000;continue;}

        if(mbi.State==MEM_COMMIT
           &&(mbi.Protect&(PAGE_READWRITE|PAGE_EXECUTE_READ))
           &&mbi.Type==MEM_PRIVATE
           &&mbi.RegionSize>=0x1000&&mbi.RegionSize<=0x4000000) {

            std::vector<uint8_t> buf(mbi.RegionSize);
            SIZE_T br=0;
            if(ReadProcessMemory(G.procHandle,mbi.BaseAddress,
               buf.data(),mbi.RegionSize,&br)&&br>32) {
                for(size_t i=0;i+8<=br;i+=8) {
                    uintptr_t ptr=*(uintptr_t*)(&buf[i]);
                    if(ptr<0x100000||ptr>0x7FFFFFFFFFFF) continue;

                    float hp=Rf32(ptr+Off::Health);
                    if(hp<0.f||hp>200.f||std::isnan(hp)) continue;

                    float px=Rf32(ptr+Off::PosX);
                    float py=Rf32(ptr+Off::PosY);
                    float pz=Rf32(ptr+Off::PosZ);
                    if(px<-8000.f||px>8000.f) continue;
                    if(py<-200.f ||py>800.f)  continue;
                    if(fabsf(px)<1.f&&fabsf(pz)<1.f) continue;

                    bool dup=false;
                    for(auto&e:found){
                        float dx=e.pos.x-px,dz=e.pos.z-pz;
                        if(sqrtf(dx*dx+dz*dz)<15.f){dup=true;break;}
                    }
                    if(dup) continue;

                    Entity e;
                    e.ptr=ptr; e.hp=hp;
                    e.pos={px,py,pz};
                    e.type=ClassifyEntity(ptr,hp);
                    if(e.type==EType::Player){
                        uintptr_t np=Rptr(ptr+Off::DisplayName);
                        e.name=Rstr(np);
                        if(e.name.empty()) e.name="Player";
                    } else {
                        e.name=e.type==EType::NPC?"NPC":
                               e.type==EType::Loot?"Loot":"Container";
                    }
                    found.push_back(e);
                }
                scanned+=br;
            }
        }

        uintptr_t next=(uintptr_t)mbi.BaseAddress+mbi.RegionSize;
        if(next<=addr) break;
        addr=next;
        G.scanPct=(float)scanned/limit*100.f;
    }

    {
        std::lock_guard<std::mutex> lk(G.mtx);
        G.entities=found;
    }
    G.scanning=false;
    G.Log("Scan complete — found "+std::to_string(found.size())+" entities");
}

void RefreshEntity(Entity& e) {
    float h=Rf32(e.ptr+Off::Health);
    if(h<0.f||h>1000.f||std::isnan(h)){e.lost=true;return;}
    e.hp=h; e.lost=false;
    e.pos.x=Rf32(e.ptr+Off::PosX);
    e.pos.y=Rf32(e.ptr+Off::PosY);
    e.pos.z=Rf32(e.ptr+Off::PosZ);
    if(e.pos.x<-8000.f||e.pos.x>8000.f){e.lost=true;return;}
    e.hpHist.push_back(e.hp);
    if(e.hpHist.size()>30) e.hpHist.pop_front();
    if(e.type==EType::Player){
        uintptr_t np=Rptr(e.ptr+Off::DisplayName);
        auto n=Rstr(np); if(!n.empty()) e.name=n;
        uintptr_t mp=Rptr(e.ptr+Off::MetabolismPtr);
        if(ValidPtr(mp)){
            e.calories =Rf32(mp+Off::Calories);
            e.hydration=Rf32(mp+Off::Hydration);
            e.radiation=Rf32(mp+Off::Radiation);
            e.bleeding =Rpm<bool>(mp+Off::Bleeding);
        }
    }
}

void RefreshCamera() {
    if(!G.gameBase) return;
    uintptr_t cb=G.gameBase+Off::CamFnOff;
    std::lock_guard<std::mutex> lk(G.mtx);
    G.camera.pos.x=Rf32(cb+Off::CamPX);
    G.camera.pos.y=Rf32(cb+Off::CamPY);
    G.camera.pos.z=Rf32(cb+Off::CamPZ);
    G.camera.rot.x=Rf32(cb+Off::CamRX);
    G.camera.rot.y=Rf32(cb+Off::CamRY);
    G.camera.rot.z=Rf32(cb+Off::CamRZ);
    G.camera.rot.w=Rf32(cb+Off::CamRW);
}

void GameThread() {
    uint64_t lastReads = 0, lastBytes = 0;
    DWORD    lastTick  = GetTickCount();
    while(G.running) {
        if(!G.attached){Sleep(500);continue;}
        RefreshCamera();
        {
            std::lock_guard<std::mutex> lk(G.mtx);
            for(auto&e:G.entities) RefreshEntity(e);
        }
        // раз в ~1 сек считаем скорость чтения памяти через CPU
        DWORD now=GetTickCount();
        DWORD dt =now-lastTick;
        if(dt>=1000){
            uint64_t r=G.memReadTotal.load(), b=G.memBytes.load();
            double secs=dt/1000.0;
            G.memReadRate = (r-lastReads)/secs;
            G.memMBs      = ((b-lastBytes)/secs)/(1024.0*1024.0);
            lastReads=r; lastBytes=b; lastTick=now;
        }
        Sleep(16);
    }
}

// Имя процесса по PID (для лога/статуса)
std::string NameOfPid(DWORD pid) {
    std::lock_guard<std::mutex> lk(G.procMtx);
    for(auto&p:G.procList) if(p.pid==pid) return p.name;
    return "PID "+std::to_string(pid);
}

// Подключение к КОНКРЕТНОМУ pid, выбранному в UI.
bool AttachToPid(DWORD pid) {
    if(!pid){ G.Log("No process selected"); return false; }
    HANDLE h=OpenProcess(PROCESS_ALL_ACCESS,FALSE,pid);
    if(!h){
        G.Log("OpenProcess failed for PID "+std::to_string(pid)+" (run as Administrator?)");
        return false;
    }
    // предыдущий хэндл закрываем
    if(G.procHandle){ CloseHandle(G.procHandle); G.procHandle=nullptr; }

    G.procHandle = h;
    G.procPid    = pid;
    G.procName   = NameOfPid(pid);
    G.gameBase   = FindModule(pid,"GameAssembly.dll");

    // сброс счётчиков чтения памяти
    G.memReadTotal=0; G.memReadFail=0; G.memBytes=0;
    G.memReadRate=0.0; G.memMBs=0.0;

    G.attached=true;
    G.Log("Attached: "+G.procName+" PID="+std::to_string(pid));
    if(!G.gameBase) G.Log("WARN: GameAssembly.dll not found — camera offsets won't resolve");
    else G.Log("GameBase: "+[&]{std::ostringstream o;o<<std::hex<<G.gameBase;return o.str();}());
    ScanEntities();
    return true;
}

void AttachThread() {
    // 1) если пользователь выбрал процесс в списке — берём его
    DWORD sel=G.selectedPid.load();
    if(sel){ AttachToPid(sel); return; }

    // 2) иначе авто-поиск как раньше
    const char* procs[]={"RustClient.exe","rust.exe",nullptr};
    G.Log("Auto-searching for Rust process...");
    for(int i=0;procs[i];++i){
        DWORD pid=FindPID(procs[i]);
        if(!pid) continue;
        G.selectedPid=pid;
        AttachToPid(pid);
        return;
    }
    G.Log("Rust not found — pick a process manually in the list");
}

// Полная отвязка от процесса
void DetachProcess() {
    G.attached=false;
    Sleep(20); // дать GameThread выйти из чтений
    {
        std::lock_guard<std::mutex> lk(G.mtx);
        G.entities.clear();
    }
    if(G.procHandle){ CloseHandle(G.procHandle); G.procHandle=nullptr; }
    G.procPid=0; G.gameBase=0; G.procName.clear();
    G.Log("Detached");
}

// ═══════════════════════════════════════════════════════
//  MATH
// ═══════════════════════════════════════════════════════
bool W2S(Vec3 w, Vec2& s, int sw, int sh, const CameraData& cam) {
    float dx=w.x-cam.pos.x, dy=w.y-cam.pos.y, dz=w.z-cam.pos.z;
    float rx=cam.rot.x,ry=cam.rot.y,rz=cam.rot.z,rw=cam.rot.w;
    float lx=(1-2*(ry*ry+rz*rz))*dx+(2*(rx*ry-rz*rw))*dy+(2*(rx*rz+ry*rw))*dz;
    float ly=(2*(rx*ry+rz*rw))*dx+(1-2*(rx*rx+rz*rz))*dy+(2*(ry*rz-rx*rw))*dz;
    float lz=(2*(rx*rz-ry*rw))*dx+(2*(ry*rz+rx*rw))*dy+(1-2*(rx*rx+ry*ry))*dz;
    if(lz<=0.01f) return false;
    float f=1.f/tanf(cam.fov*0.5f*3.14159265f/180.f);
    s.x=sw*0.5f+(lx/lz)*(sw*0.5f)*f;
    s.y=sh*0.5f-(ly/lz)*(sh*0.5f)*f;
    return s.x>-100&&s.x<sw+100&&s.y>-100&&s.y<sh+100;
}

// ═══════════════════════════════════════════════════════
//  COLORS
// ═══════════════════════════════════════════════════════
ImU32 ECol(EType t,float a=1.f) {
    switch(t){
        case EType::Player:    return IM_COL32(0,255,68,  (int)(a*255));
        case EType::NPC:       return IM_COL32(255,136,0, (int)(a*255));
        case EType::Loot:      return IM_COL32(0,170,255, (int)(a*255));
        case EType::Container: return IM_COL32(200,68,255,(int)(a*255));
        default:               return IM_COL32(100,100,100,(int)(a*255));
    }
}

ImU32 HCol(float hp) {
    if(hp>60) return IM_COL32(0,255,68,255);
    if(hp>30) return IM_COL32(255,204,0,255);
    return IM_COL32(255,34,34,255);
}

// ═══════════════════════════════════════════════════════
//  ESP DRAW
// ═══════════════════════════════════════════════════════
void DrawBox(ImDrawList*dl,float sx,float sy,float hw,float hh,ImU32 c){
    dl->AddRect({sx-hw,sy-hh},{sx+hw,sy+hh},c,0,0,2.f);
    float cs=hw*.4f;
    ImU32 w=IM_COL32(255,255,255,255);
    dl->AddLine({sx-hw,sy-hh},{sx-hw+cs,sy-hh},w,2);dl->AddLine({sx-hw,sy-hh},{sx-hw,sy-hh+cs},w,2);
    dl->AddLine({sx+hw,sy-hh},{sx+hw-cs,sy-hh},w,2);dl->AddLine({sx+hw,sy-hh},{sx+hw,sy-hh+cs},w,2);
    dl->AddLine({sx-hw,sy+hh},{sx-hw+cs,sy+hh},w,2);dl->AddLine({sx-hw,sy+hh},{sx-hw,sy+hh-cs},w,2);
    dl->AddLine({sx+hw,sy+hh},{sx+hw-cs,sy+hh},w,2);dl->AddLine({sx+hw,sy+hh},{sx+hw,sy+hh-cs},w,2);
}

void DrawHPBar(ImDrawList*dl,float sx,float sy,float hw,float hh,float hp){
    float bw=hw*2,bh=max(4.f,hw/3.f),bx=sx-hw,by=sy+hh+4.f;
    dl->AddRectFilled({bx,by},{bx+bw,by+bh},IM_COL32(17,17,17,200));
    dl->AddRect({bx,by},{bx+bw,by+bh},IM_COL32(51,51,51,255));
    float fw=bw*max(0.f,min(1.f,hp/100.f));
    if(fw>0) dl->AddRectFilled({bx,by},{bx+fw,by+bh},HCol(hp));
    char buf[16]; snprintf(buf,sizeof(buf),"%dHP",(int)hp);
    dl->AddText({sx-12.f,by+bh+2.f},IM_COL32(255,255,255,220),buf);
}

void DrawSkeleton(ImDrawList*dl,float sx,float sy,float hw,float hh){
    ImU32 c=IM_COL32(255,255,255,40);
    float hr=max(4.f,hw/3.f);
    dl->AddCircle({sx,sy-hh-hr},hr,c,12,1.f);
    dl->AddLine({sx,sy-hh},{sx,sy+hh},c,1);
    dl->AddLine({sx-hw,sy-hh*.4f},{sx+hw,sy-hh*.4f},c,1);
    dl->AddLine({sx,sy+hh},{sx-hw*.6f,sy+hh*1.5f},c,1);
    dl->AddLine({sx,sy+hh},{sx+hw*.6f,sy+hh*1.5f},c,1);
}

void DrawTrend(ImDrawList*dl,float sx,float sy,float hw,float hh,
               const std::deque<float>&hist){
    if(hist.size()<3) return;
    float w=hw*2,h=12,bx=sx-hw,by=sy-hh-30.f;
    float mn=100,mx=0;
    for(float v:hist){mn=min(mn,v);mx=max(mx,v);}
    float rng=mx-mn>0?mx-mn:1;
    std::vector<ImVec2> pts;
    int n=(int)hist.size();
    for(int i=0;i<n;++i)
        pts.push_back({bx+(float)i/(n-1)*w, by+h-((hist[i]-mn)/rng)*h});
    dl->AddPolyline(pts.data(),(int)pts.size(),IM_COL32(0,136,255,200),0,1.f);
}

void DrawEdge(ImDrawList*dl,float camX,float camZ,float ex,float ez,ImU32 c,int sw,int sh){
    float dx=ex-camX,dz=ez-camZ;
    float ang=atan2f(dx,dz);
    float r=min(sw,sh)*.5f-32.f;
    float ax=sw*.5f+r*sinf(ang), ay=sh*.5f-r*cosf(ang);
    float bx=ax+12.f*sinf(ang+3.14159f), by=ay-12.f*cosf(ang+3.14159f);
    ImVec2 tri[3]={{ax,ay},{bx+7*cosf(ang),by+7*sinf(ang)},{bx-7*cosf(ang),by-7*sinf(ang)}};
    dl->AddTriangleFilled(tri[0],tri[1],tri[2],c);
    dl->AddTriangle(tri[0],tri[1],tri[2],IM_COL32(255,255,255,180),1.f);
}

void DrawCrosshair(ImDrawList*dl,int sw,int sh){
    float cx=sw*.5f,cy=sh*.5f,g=5,s=11;
    ImU32 w=IM_COL32(255,255,255,200);
    dl->AddLine({cx-s-g,cy},{cx-g,cy},w,1);dl->AddLine({cx+g,cy},{cx+s+g,cy},w,1);
    dl->AddLine({cx,cy-s-g},{cx,cy-g},w,1);dl->AddLine({cx,cy+g},{cx,cy+s+g},w,1);
    dl->AddCircle({cx,cy},2.f,w,8,1.f);
}

void DrawCompass(ImDrawList*dl,int sw){
    float cx=sw*.5f,cy=26.f;
    struct{const char*l;ImU32 c;float ox,oy;}d[]={
        {"N",IM_COL32(255,68,68,255),0,-20},{"E",IM_COL32(200,200,200,255),55,0},
        {"S",IM_COL32(200,200,200,255),0,20},{"W",IM_COL32(200,200,200,255),-55,0}};
    for(auto&v:d) dl->AddText({cx+v.ox-5.f,cy+v.oy-7.f},v.c,v.l);
}

void RenderESPFrame(int sw,int sh) {
    ImDrawList* dl=ImGui::GetBackgroundDrawList();
    CameraData cam;
    std::vector<Entity> ents;
    ESPSettings sets;
    {
        std::lock_guard<std::mutex> lk(G.mtx);
        cam=G.camera; ents=G.entities; sets=G.settings;
    }
    cam.fov=sets.fov;

    for(auto&e:ents){
        if(e.lost) continue;
        bool show=false;
        switch(e.type){
            case EType::Player:    show=sets.showPlayers;    break;
            case EType::NPC:       show=sets.showNPC;        break;
            case EType::Loot:      show=sets.showLoot;       break;
            case EType::Container: show=sets.showContainers; break;
            default:break;
        }
        if(!show) continue;
        float dx=e.pos.x-cam.pos.x,dy=e.pos.y-cam.pos.y,dz=e.pos.z-cam.pos.z;
        float dist=sqrtf(dx*dx+dy*dy+dz*dz);
        if(dist>sets.maxDist) continue;

        Vec2 sc{};
        ImU32 col=ECol(e.type);
        if(W2S(e.pos,sc,sw,sh,cam)){
            float scale=max(16.f,800.f/(dist+1.f));
            float hw=scale*.5f,hh=scale;
            if(sets.showBox)     DrawBox(dl,sc.x,sc.y,hw,hh,col);
            if(sets.showSkeleton&&e.type==EType::Player) DrawSkeleton(dl,sc.x,sc.y,hw,hh);
            if(sets.showHP)      DrawHPBar(dl,sc.x,sc.y,hw,hh,e.hp);
            if(sets.showTrend)   DrawTrend(dl,sc.x,sc.y,hw,hh,e.hpHist);
            if(sets.showName){
                std::string lbl=e.name;
                if(sets.showDist){char b[32];snprintf(b,32," %.0fm",dist);lbl+=b;}
                dl->AddText({sc.x-30.f,sc.y-hh-18.f},col,lbl.c_str());
            }
            if(e.bleeding) dl->AddText({sc.x+hw+2,sc.y-hh},IM_COL32(255,34,34,255),"BLD");
            if(e.radiation>5) dl->AddText({sc.x+hw+2,sc.y-hh+14},IM_COL32(136,255,0,255),"RAD");
        } else {
            if(sets.showEdge) DrawEdge(dl,cam.pos.x,cam.pos.z,e.pos.x,e.pos.z,col,sw,sh);
        }
    }

    // sidebar
    float px=10,py=10;
    int vis=(int)std::count_if(ents.begin(),ents.end(),[](const Entity&e){return !e.lost;});
    dl->AddRectFilled({px,py},{px+250,py+vis*16.f+28},IM_COL32(0,0,0,170));
    dl->AddRect({px,py},{px+250,py+vis*16.f+28},IM_COL32(30,30,30,255));
    dl->AddText({px+70,py+5},IM_COL32(80,80,80,255),"ENTITY LIST");
    py+=20;
    auto sorted=ents;
    std::sort(sorted.begin(),sorted.end(),[&](const Entity&a,const Entity&b){
        float da=sqrtf(powf(a.pos.x-cam.pos.x,2)+powf(a.pos.z-cam.pos.z,2));
        float db=sqrtf(powf(b.pos.x-cam.pos.x,2)+powf(b.pos.z-cam.pos.z,2));
        return da<db;
    });
    for(auto&e:sorted){
        if(e.lost) continue;
        float d=sqrtf(powf(e.pos.x-cam.pos.x,2)+powf(e.pos.z-cam.pos.z,2));
        char buf[64];
        snprintf(buf,64,"%-12s %3dHP %5.0fm",e.name.c_str(),(int)e.hp,d);
        dl->AddText({px+6,py},ECol(e.type),buf);
        py+=15;
    }

    DrawCrosshair(dl,sw,sh);
    DrawCompass(dl,sw);

    // hud
    int alive=(int)std::count_if(ents.begin(),ents.end(),[](const Entity&e){
        return !e.lost&&e.hp>0&&e.type==EType::Player;});
    char hud[64];
    snprintf(hud,64,"PLAYERS %d/%d  |  ESC=HIDE ESP",
             alive,(int)std::count_if(ents.begin(),ents.end(),[](const Entity&e){
                 return e.type==EType::Player;}));
    dl->AddText({(float)sw-280,(float)sh-20},IM_COL32(60,60,60,255),hud);
}

// ═══════════════════════════════════════════════════════
//  GUI WINDOW
// ═══════════════════════════════════════════════════════
void RenderGUI() {
    ImGui::SetNextWindowSize({520,640},ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({30,30},ImGuiCond_FirstUseEver);
    ImGui::Begin("Company Rust — Admin Panel",nullptr,ImGuiWindowFlags_NoCollapse);

    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,1,.4f,1));
    ImGui::Text("COMPANY RUST  |  ADMIN CONTROL PANEL");
    ImGui::PopStyleColor();
    ImGui::Separator(); ImGui::Spacing();

    // ── Верхний блок: статус + счётчик чтения памяти ──
    if(!G.attached) {
        ImGui::TextColored({1,.4f,0,1},"● NOT ATTACHED");
    } else if(G.scanning) {
        ImGui::TextColored({1,.8f,0,1},"● SCANNING...  %.0f%%",G.scanPct.load());
        ImGui::ProgressBar(G.scanPct/100.f,{-1,5});
    } else {
        int pc=(int)std::count_if(G.entities.begin(),G.entities.end(),
            [](const Entity&e){return !e.lost&&e.type==EType::Player;});
        ImGui::TextColored({0,1,.4f,1},"● %s  PID=%u  BASE=%llX  PLAYERS=%d",
            G.procName.c_str(),G.procPid,G.gameBase,pc);
    }

    // Счётчик чтений памяти через CPU (reads/sec + МБ/сек)
    if(G.attached){
        double rate=G.memReadRate.load(), mbs=G.memMBs.load();
        ImGui::TextColored({.5f,.8f,1,1},
            "MEM READ: %.0f reads/s   %.2f MB/s   |  total %llu  fails %llu",
            rate, mbs,
            (unsigned long long)G.memReadTotal.load(),
            (unsigned long long)G.memReadFail.load());
        // мини-бар нагрузки чтения (0..5000 reads/s)
        ImGui::ProgressBar((float)(rate>5000.0?1.0:rate/5000.0),{-1,4},"");
    }

    ImGui::Spacing();

    if(ImGui::BeginTabBar("##tabs")) {

        // ── PROCESS: выбор процесса раста прямо в приложении ──
        if(ImGui::BeginTabItem("Process")) {
            if(ImGui::Button("REFRESH LIST",{130,26}))
                std::thread([](){RefreshProcessList();}).detach();
            ImGui::SameLine();
            if(G.listing) ImGui::TextColored({1,.8f,0,1},"scanning...");
            else          ImGui::TextDisabled("green = Rust (GameAssembly.dll)");

            ImGui::Spacing();
            DWORD sel=G.selectedPid.load();

            if(ImGui::BeginTable("proc",3,
                ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|
                ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable,{0,320})){
                ImGui::TableSetupColumn("PID",  ImGuiTableColumnFlags_WidthFixed,70);
                ImGui::TableSetupColumn("Process",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Rust?", ImGuiTableColumnFlags_WidthFixed,60);
                ImGui::TableHeadersRow();
                std::lock_guard<std::mutex> lk(G.procMtx);
                for(auto&p:G.procList){
                    ImGui::TableNextRow();
                    bool isSel=(p.pid==sel);
                    ImGui::TableSetColumnIndex(0);
                    char idbuf[32]; snprintf(idbuf,sizeof(idbuf),"%u",p.pid);
                    if(ImGui::Selectable(idbuf,isSel,ImGuiSelectableFlags_SpanAllColumns))
                        G.selectedPid=p.pid;
                    ImGui::TableSetColumnIndex(1);
                    if(p.hasGameAssembly) ImGui::TextColored({0,1,.4f,1},"%s",p.name.c_str());
                    else                  ImGui::TextUnformatted(p.name.c_str());
                    ImGui::TableSetColumnIndex(2);
                    if(p.hasGameAssembly) ImGui::TextColored({0,1,.4f,1},"YES");
                    else                  ImGui::TextDisabled("-");
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            sel=G.selectedPid.load();
            if(sel) ImGui::Text("Selected PID: %u",sel);
            else    ImGui::TextDisabled("No process selected");

            bool busy=G.scanning.load();
            if(busy) ImGui::BeginDisabled();
            if(ImGui::Button(G.attached?"RE-ATTACH TO SELECTED":"ATTACH TO SELECTED",{220,30})){
                std::thread([](){ AttachThread(); }).detach();
            }
            if(busy) ImGui::EndDisabled();

            ImGui::SameLine();
            if(ImGui::Button("AUTO-FIND RUST",{150,30})){
                G.selectedPid=0; // сброс — AttachThread уйдёт в авто-поиск
                std::thread([](){ AttachThread(); }).detach();
            }

            if(G.attached){
                ImGui::SameLine();
                if(ImGui::Button("DETACH",{90,30}))
                    std::thread([](){ DetachProcess(); }).detach();
                if(ImGui::Button("RE-SCAN ENTITIES",{170,24}))
                    std::thread([](){ScanEntities();}).detach();
            }
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem("ESP")) {
            ImGui::Columns(2,"c",false);
            ImGui::Text("FILTERS");
            ESPSettings& s=G.settings;
            ImGui::Checkbox("Players",    &s.showPlayers);
            ImGui::Checkbox("NPC",        &s.showNPC);
            ImGui::Checkbox("Loot",       &s.showLoot);
            ImGui::Checkbox("Containers", &s.showContainers);
            ImGui::Spacing();
            ImGui::Text("VISUAL");
            ImGui::Checkbox("Box",        &s.showBox);
            ImGui::Checkbox("HP Bar",     &s.showHP);
            ImGui::Checkbox("Name",       &s.showName);
            ImGui::Checkbox("Distance",   &s.showDist);
            ImGui::Checkbox("Skeleton",   &s.showSkeleton);
            ImGui::Checkbox("HP Trend",   &s.showTrend);
            ImGui::Checkbox("Edge Arrows",&s.showEdge);
            ImGui::NextColumn();
            ImGui::Text("RANGE");
            ImGui::SliderFloat("Max Dist",&s.maxDist,50.f,2000.f,"%.0fm");
            ImGui::SliderFloat("FOV",     &s.fov,    40.f,120.f, "%.0f");
            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem("Players")) {
            if(ImGui::BeginTable("pt",6,
                ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|
                ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable,{0,380})){
                ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed,30);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("HP",   ImGuiTableColumnFlags_WidthFixed,50);
                ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed,60);
                ImGui::TableSetupColumn("Pos",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("State",ImGuiTableColumnFlags_WidthFixed,60);
                ImGui::TableHeadersRow();
                std::lock_guard<std::mutex> lk(G.mtx);
                int idx=1;
                for(auto&e:G.entities){
                    if(e.type!=EType::Player) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%d",idx++);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored({0,1,.4f,1},"%s",e.name.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImVec4 hc=e.hp>60?ImVec4{0,1,.4f,1}:e.hp>30?ImVec4{1,.8f,0,1}:ImVec4{1,.2f,.2f,1};
                    ImGui::TextColored(hc,"%.0f",e.hp);
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%.0fm",e.dist);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.0f %.0f %.0f",e.pos.x,e.pos.y,e.pos.z);
                    ImGui::TableSetColumnIndex(5);
                    const char* st=e.lost?"LOST":e.hp<=0?"DEAD":e.onScreen?"VIS":"HID";
                    ImU32 sc=e.lost?IM_COL32(80,80,80,255):e.hp<=0?IM_COL32(255,60,60,255):
                             e.onScreen?IM_COL32(0,255,100,255):IM_COL32(200,200,200,255);
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sc),"%s",st);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem("Camera")) {
            std::lock_guard<std::mutex> lk(G.mtx);
            auto&c=G.camera;
            ImGui::Text("Position:  X=%.2f  Y=%.2f  Z=%.2f",c.pos.x,c.pos.y,c.pos.z);
            ImGui::Text("Rotation:  X=%.3f  Y=%.3f  Z=%.3f  W=%.3f",c.rot.x,c.rot.y,c.rot.z,c.rot.w);
            ImGui::Text("GameBase:  %llX",G.gameBase);
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem("Log")) {
            ImGui::BeginChild("log",{0,380},false,ImGuiWindowFlags_HorizontalScrollbar);
            std::lock_guard<std::mutex> lk(G.mtx);
            for(auto&s:G.log)
                ImGui::TextUnformatted(s.c_str());
            if(ImGui::GetScrollY()>=ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if(ImGui::BeginTabItem("Help")) {
            ImGui::BulletText("Process tab — pick the Rust process, then ATTACH TO SELECTED");
            ImGui::BulletText("Green row = real Rust (GameAssembly.dll found)");
            ImGui::BulletText("AUTO-FIND RUST — attaches automatically if running");
            ImGui::BulletText("MEM READ meter — memory reads/sec through the CPU");
            ImGui::BulletText("F1 — toggle Players    F2 — toggle NPC");
            ImGui::BulletText("F3 — toggle Loot       F4 — toggle Containers");
            ImGui::BulletText("ESC — close ESP overlay");
            ImGui::BulletText("Run as Administrator");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ═══════════════════════════════════════════════════════
//  ESP OVERLAY WINDOW
// ═══════════════════════════════════════════════════════
struct D3DContext {
    ID3D11Device*           dev=nullptr;
    ID3D11DeviceContext*    ctx=nullptr;
    IDXGISwapChain*         sc =nullptr;
    ID3D11RenderTargetView* rtv=nullptr;
    bool Init(HWND hwnd,bool transparent=false){
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount=2;
        sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow=hwnd; sd.SampleDesc={1,0};
        sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL fl;
        if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
            nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&sc,&dev,&fl,&ctx)))
            return false;
        ID3D11Texture2D*bb=nullptr;
        sc->GetBuffer(0,IID_PPV_ARGS(&bb));
        dev->CreateRenderTargetView(bb,nullptr,&rtv);
        bb->Release();
        return true;
    }
    void Resize(UINT w,UINT h){
        if(rtv){rtv->Release();rtv=nullptr;}
        sc->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0);
        ID3D11Texture2D*bb=nullptr;
        sc->GetBuffer(0,IID_PPV_ARGS(&bb));
        dev->CreateRenderTargetView(bb,nullptr,&rtv);
        bb->Release();
    }
};

static D3DContext g_GUId3d, g_ESPd3d;
static HWND g_GUIhwnd=nullptr, g_ESPhwnd=nullptr;
static bool g_ESPVisible=true;
static int g_SW=0,g_SH=0;

LRESULT CALLBACK GUIProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(ImGui_ImplWin32_WndProcHandler(h,m,w,l)) return true;
    if(m==WM_SIZE&&g_GUId3d.dev&&w!=SIZE_MINIMIZED)
        g_GUId3d.Resize(LOWORD(l),HIWORD(l));
    if(m==WM_DESTROY){G.running=false;PostQuitMessage(0);}
    return DefWindowProcW(h,m,w,l);
}

LRESULT CALLBACK ESPProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_KEYDOWN&&w==VK_ESCAPE){g_ESPVisible=!g_ESPVisible;return 0;}
    if(m==WM_KEYDOWN&&w==VK_F1){G.settings.showPlayers=!G.settings.showPlayers;return 0;}
    if(m==WM_KEYDOWN&&w==VK_F2){G.settings.showNPC=!G.settings.showNPC;return 0;}
    if(m==WM_KEYDOWN&&w==VK_F3){G.settings.showLoot=!G.settings.showLoot;return 0;}
    if(m==WM_KEYDOWN&&w==VK_F4){G.settings.showContainers=!G.settings.showContainers;return 0;}
    return DefWindowProcW(h,m,w,l);
}

void ESPThread() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=ESPProc;
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"RustESP";
    RegisterClassExW(&wc);

    g_ESPhwnd=CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_TRANSPARENT|WS_EX_LAYERED|WS_EX_NOACTIVATE,
        L"RustESP",L"",WS_POPUP,
        0,0,g_SW,g_SH,nullptr,nullptr,wc.hInstance,nullptr);
    SetLayeredWindowAttributes(g_ESPhwnd,RGB(0,0,0),0,LWA_COLORKEY);
    MARGINS m{-1,-1,-1,-1};
    DwmExtendFrameIntoClientArea(g_ESPhwnd,&m);
    ShowWindow(g_ESPhwnd,SW_SHOW);

    g_ESPd3d.Init(g_ESPhwnd,true);

    ImGuiContext* espCtx=ImGui::CreateContext();
    ImGui::SetCurrentContext(espCtx);
    ImGui::GetIO().IniFilename=nullptr;
    ImGui_ImplWin32_Init(g_ESPhwnd);
    ImGui_ImplDX11_Init(g_ESPd3d.dev,g_ESPd3d.ctx);

    MSG msg{};
    while(G.running) {
        while(PeekMessage(&msg,g_ESPhwnd,0,0,PM_REMOVE)){
            TranslateMessage(&msg);DispatchMessage(&msg);
        }
        ImGui::SetCurrentContext(espCtx);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if(g_ESPVisible&&G.attached&&!G.scanning)
            RenderESPFrame(g_SW,g_SH);
        else if(!G.attached){
            auto*dl=ImGui::GetBackgroundDrawList();
            dl->AddText({(float)g_SW/2-100,(float)g_SH/2},
                IM_COL32(0,200,255,255),"WAITING FOR RUST — USE ADMIN PANEL");
        }

        ImGui::Render();
        float clr[4]={0,0,0,0};
        g_ESPd3d.ctx->OMSetRenderTargets(1,&g_ESPd3d.rtv,nullptr);
        g_ESPd3d.ctx->ClearRenderTargetView(g_ESPd3d.rtv,clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_ESPd3d.sc->Present(1,0);
        Sleep(1);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(espCtx);
}

// ═══════════════════════════════════════════════════════
//  ENTRY POINT
// ═══════════════════════════════════════════════════════
int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int){
    g_SW=GetSystemMetrics(SM_CXSCREEN);
    g_SH=GetSystemMetrics(SM_CYSCREEN);

    // GUI window
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style=CS_CLASSDC; wc.lpfnWndProc=GUIProc;
    wc.hInstance=hInst; wc.lpszClassName=L"RustGUI";
    RegisterClassExW(&wc);
    g_GUIhwnd=CreateWindowW(L"RustGUI",L"Company Rust Admin",
        WS_OVERLAPPEDWINDOW,100,100,560,660,nullptr,nullptr,hInst,nullptr);
    g_GUId3d.Init(g_GUIhwnd);
    ShowWindow(g_GUIhwnd,SW_SHOW);

    // ImGui для GUI
    ImGuiContext* guiCtx=ImGui::CreateContext();
    ImGui::SetCurrentContext(guiCtx);
    ImGui::GetIO().IniFilename=nullptr;
    // стиль
    ImGuiStyle&s=ImGui::GetStyle();
    s.WindowRounding=6;s.FrameRounding=4;s.GrabRounding=4;
    auto&c=s.Colors;
    c[ImGuiCol_WindowBg]      ={.06f,.06f,.06f,.96f};
    c[ImGuiCol_TitleBgActive] ={.1f,.1f,.1f,1};
    c[ImGuiCol_FrameBg]       ={.12f,.12f,.12f,1};
    c[ImGuiCol_Button]        ={.1f,.28f,.1f,1};
    c[ImGuiCol_ButtonHovered] ={.14f,.4f,.14f,1};
    c[ImGuiCol_CheckMark]     ={0,1,.4f,1};
    c[ImGuiCol_SliderGrab]    ={0,.8f,.3f,1};
    c[ImGuiCol_TabActive]     ={.1f,.28f,.1f,1};
    ImGui_ImplWin32_Init(g_GUIhwnd);
    ImGui_ImplDX11_Init(g_GUId3d.dev,g_GUId3d.ctx);

    // ESP в отдельном потоке
    std::thread espT(ESPThread);
    espT.detach();

    // Game refresh thread
    std::thread(GameThread).detach();

    G.Log("RustAdmin started");
    // Заполнить список процессов сразу, чтобы вкладка Process была готова
    std::thread([](){ RefreshProcessList(); }).detach();

    MSG msg{};
    while(G.running) {
        while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg);DispatchMessage(&msg);
            if(msg.message==WM_QUIT) G.running=false;
        }
        ImGui::SetCurrentContext(guiCtx);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderGUI();
        ImGui::Render();
        float clr[4]={.04f,.04f,.04f,1};
        g_GUId3d.ctx->OMSetRenderTargets(1,&g_GUId3d.rtv,nullptr);
        g_GUId3d.ctx->ClearRenderTargetView(g_GUId3d.rtv,clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_GUId3d.sc->Present(1,0);
    }

    G.running=false;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(guiCtx);
    if(G.procHandle) CloseHandle(G.procHandle);
    return 0;
}