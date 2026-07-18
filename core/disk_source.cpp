// bitforge - disk_source.cpp
#include "disk_source.h"
#include <winioctl.h>
#include <cstdio>
#include <cstring>

namespace bf {

std::vector<DiskInfo> enum_disks(){
    std::vector<DiskInfo> out;
    for(int i=0;i<16;i++){
        char path[64]; snprintf(path,sizeof(path),"\\\\.\\PhysicalDrive%d",i);
        HANDLE h=CreateFileA(path,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(h==INVALID_HANDLE_VALUE) continue;
        DiskInfo di{}; di.index=i; di.sector=512; di.size=0;
        DISK_GEOMETRY_EX g{}; DWORD br=0;
        if(DeviceIoControl(h,IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,nullptr,0,&g,sizeof(g),&br,nullptr)){
            di.size=(uint64_t)g.DiskSize.QuadPart;
            di.sector=g.Geometry.BytesPerSector?g.Geometry.BytesPerSector:512;
        }
        STORAGE_PROPERTY_QUERY spq{}; spq.PropertyId=StorageDeviceProperty; spq.QueryType=PropertyStandardQuery;
        BYTE buf[1024]={0};
        if(DeviceIoControl(h,IOCTL_STORAGE_QUERY_PROPERTY,&spq,sizeof(spq),buf,sizeof(buf),&br,nullptr)){
            STORAGE_DEVICE_DESCRIPTOR* d=(STORAGE_DEVICE_DESCRIPTOR*)buf;
            if(d->ProductIdOffset && d->ProductIdOffset<sizeof(buf)){
                const char* p=(const char*)(buf+d->ProductIdOffset);
                while(*p==' ') p++; di.model=p;
                while(!di.model.empty() && di.model.back()==' ') di.model.pop_back();
            }
        }
        out.push_back(di);
        CloseHandle(h);
    }
    return out;
}

DiskSource::~DiskSource(){
    for(HANDLE v:lockedVols_) if(v!=INVALID_HANDLE_VALUE) CloseHandle(v);   // release lock -> remount
    if(h_!=INVALID_HANDLE_VALUE) CloseHandle(h_);
    if(bounce_) VirtualFree(bounce_,0,MEM_RELEASE);
}

void DiskSource::ensure_bounce(size_t n){
    if(n<=bounceCap_) return;
    if(bounce_) VirtualFree(bounce_,0,MEM_RELEASE);
    bounce_=(unsigned char*)VirtualAlloc(nullptr,n,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    bounceCap_=bounce_?n:0;
}

void DiskSource::lock_volumes(int diskIndex){
    char vol[MAX_PATH]; HANDLE fv=FindFirstVolumeA(vol,sizeof(vol));
    if(fv==INVALID_HANDLE_VALUE) return;
    do{
        size_t L=strlen(vol); if(L && vol[L-1]=='\\') vol[L-1]=0;   // strip trailing backslash for CreateFile
        HANDLE vh=CreateFileA(vol,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(vh!=INVALID_HANDLE_VALUE){
            VOLUME_DISK_EXTENTS ext{}; DWORD br=0; bool onDisk=false;
            if(DeviceIoControl(vh,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,&ext,sizeof(ext),&br,nullptr))
                for(DWORD i=0;i<ext.NumberOfDiskExtents;i++) if((int)ext.Extents[i].DiskNumber==diskIndex) onDisk=true;
            if(onDisk){
                DWORD b2;
                if(DeviceIoControl(vh,FSCTL_LOCK_VOLUME,nullptr,0,nullptr,0,&b2,nullptr)){
                    DeviceIoControl(vh,FSCTL_DISMOUNT_VOLUME,nullptr,0,nullptr,0,&b2,nullptr);
                    lockedVols_.push_back(vh); continue;    // keep handle to hold the lock
                }
            }
            CloseHandle(vh);
        }
    } while(FindNextVolumeA(fv,vol,sizeof(vol)));
    FindVolumeClose(fv);
}

bool DiskSource::open_drive(int index, bool write){
    char path[64]; snprintf(path,sizeof(path),"\\\\.\\PhysicalDrive%d",index);
    if(write) lock_volumes(index);       // lock/dismount before opening for write
    h_=CreateFileA(path, GENERIC_READ|(write?GENERIC_WRITE:0),
                   FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                   FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH, nullptr);
    if(h_==INVALID_HANDLE_VALUE) return false;
    DISK_GEOMETRY_EX g{}; DWORD br=0;
    if(DeviceIoControl(h_,IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,nullptr,0,&g,sizeof(g),&br,nullptr)){
        size_=(uint64_t)g.DiskSize.QuadPart; sector_=g.Geometry.BytesPerSector?g.Geometry.BytesPerSector:512;
    }
    writable_=write; rawDevice_=true; path_=path;
    char l[160]; snprintf(l,sizeof(l),"PhysicalDrive%d  %.1f MB  %u B/sec%s",index,size_/1048576.0,sector_,write?"  [rw]":"  [ro]");
    label_=l; return true;
}

bool DiskSource::open_image(const std::string& path, bool write, uint32_t sector){
    h_=CreateFileA(path.c_str(), GENERIC_READ|(write?GENERIC_WRITE:0),
                   FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(h_==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{}; GetFileSizeEx(h_,&li); size_=(uint64_t)li.QuadPart;
    sector_=sector?sector:512; writable_=write; rawDevice_=false; path_=path;
    char l[220]; snprintf(l,sizeof(l),"disk image  %.1f MB  %u B/sec%s",size_/1048576.0,sector_,write?"  [rw]":"  [ro]");
    label_=l; return true;
}

static inline uint64_t align_dn(uint64_t v,uint32_t s){ return v & ~(uint64_t)(s-1); }
static inline uint64_t align_up(uint64_t v,uint32_t s){ return (v+s-1) & ~(uint64_t)(s-1); }

size_t DiskSource::read(uint64_t addr, void* dst, size_t n){
    if(h_==INVALID_HANDLE_VALUE || n==0 || addr>=size_) return 0;
    uint64_t a0=align_dn(addr,sector_), a1=align_up(addr+n,sector_);
    uint64_t cap=align_up(size_,sector_); if(a1>cap) a1=cap;
    size_t len=(size_t)(a1-a0); ensure_bounce(len); if(!bounce_) return 0;
    LARGE_INTEGER li; li.QuadPart=(LONGLONG)a0;
    if(!SetFilePointerEx(h_,li,nullptr,FILE_BEGIN)) return 0;
    DWORD got=0; if(!ReadFile(h_,bounce_,(DWORD)len,&got,nullptr)) return 0;
    size_t off=(size_t)(addr-a0); if(off>=got) return 0;
    size_t avail=got-off, k=n<avail?n:avail; memcpy(dst,bounce_+off,k); return k;
}

size_t DiskSource::write(uint64_t addr, const void* src, size_t n){
    if(!writable_ || h_==INVALID_HANDLE_VALUE || n==0 || addr>=size_) return 0;
    uint64_t a0=align_dn(addr,sector_), a1=align_up(addr+n,sector_);
    size_t len=(size_t)(a1-a0); ensure_bounce(len); if(!bounce_) return 0;
    LARGE_INTEGER li; li.QuadPart=(LONGLONG)a0;
    if(!SetFilePointerEx(h_,li,nullptr,FILE_BEGIN)) return 0;
    DWORD got=0; if(!ReadFile(h_,bounce_,(DWORD)len,&got,nullptr) || got<len) return 0;  // read sectors
    memcpy(bounce_+(size_t)(addr-a0), src, n);                                            // patch bytes
    li.QuadPart=(LONGLONG)a0;
    if(!SetFilePointerEx(h_,li,nullptr,FILE_BEGIN)) return 0;
    DWORD put=0; if(!WriteFile(h_,bounce_,(DWORD)len,&put,nullptr) || put<len) return 0;  // write back
    return n;
}

std::vector<Region> DiskSource::regions(){
    Region r; r.base=0; r.size=size_; r.readable=true; r.writable=writable_; r.tag="disk";
    return { r };
}

} // namespace bf
