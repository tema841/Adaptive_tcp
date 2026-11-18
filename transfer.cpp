#include "transfer.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#define NOMINMAX
#include <windows.h>
#else
#include <arpa/inet.h>
#endif
namespace transfer {
static bool publish_file(const std::filesystem::path& from,const std::filesystem::path& to,std::error_code& ec){
#ifdef _WIN32
 ec.clear();if(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;ec=std::error_code((int)GetLastError(),std::system_category());return false;
#else
 std::filesystem::rename(from,to,ec);return !ec;
#endif
}

static void put64(std::uint8_t*p,std::uint64_t x){for(int i=7;i>=0;--i)p[7-i]=(x>>(i*8))&255;}
static std::uint64_t get64(const std::uint8_t*p){std::uint64_t x=0;for(int i=0;i<8;i++)x=(x<<8)|p[i];return x;}
Result send_file(net::socket_t fd,const std::filesystem::path&source,std::uint64_t chunk,std::uint64_t total_hint,const ProgressCallback&cb,std::uint64_t offset,const Hooks* hooks){
 Result r;r.chunk=chunk;auto cpu0=std::clock();std::ifstream f(source,std::ios::binary);if(!f)return r;f.seekg(0,std::ios::end);auto total=(std::uint64_t)f.tellg();if(total_hint&&total_hint!=total)return r;if(offset>total)return r;f.seekg((std::streamoff)offset);std::uint64_t done=offset;auto start=std::chrono::steady_clock::now(),window=start;std::uint64_t winbytes=0;double prev=0;int stable_windows=0,probe_windows=0;std::vector<char>b((size_t)chunk);
 while(done<total){if(hooks&&hooks->before_chunk&&!hooks->before_chunk())break;std::uint64_t want=std::min<std::uint64_t>(b.size(),total-done);f.read(b.data(),(std::streamsize)want);r.read_ops++;auto n=f.gcount();if(n<=0)break;if(hooks&&hooks->mutate)hooks->mutate(b.data(),(size_t)n);std::uint8_t hdr[8];put64(hdr,(std::uint64_t)n);if(!net::send_all(fd,hdr,8)||!net::send_all(fd,b.data(),(size_t)n))break;r.write_ops+=2;done+=(std::uint64_t)n;winbytes+=(std::uint64_t)n;auto now=std::chrono::steady_clock::now();double secs=std::chrono::duration<double>(now-window).count();if(secs>=0.25||done==total){double speed=winbytes/std::max(secs,0.000001)/1024.0/1024.0;if(prev>0){if(speed>prev*1.08){r.chunk=std::min<std::uint64_t>(r.chunk*2,4ull*1024*1024);stable_windows=0;probe_windows=0;}else if(speed<prev*0.78){if(secs>0.18)r.chunk=std::min<std::uint64_t>((r.chunk*3+1)/2,4ull*1024*1024);else r.chunk=std::max<std::uint64_t>(r.chunk/2,4ull*1024);stable_windows=0;}else{if(++stable_windows>=2&&probe_windows<2&&r.chunk<4ull*1024*1024){r.chunk=std::min<std::uint64_t>((r.chunk*3+1)/2,4ull*1024*1024);++probe_windows;stable_windows=0;}}}prev=speed;window=now;winbytes=0;std::uint64_t next=r.chunk;if(next!=b.size())b.resize((size_t)next);r.bytes=done;r.seconds=std::chrono::duration<double>(now-start).count();if(cb)cb(done,total,r.chunk,speed,r.rtt_ms);}}
 r.bytes=done;r.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();r.cpu_percent=r.seconds>0?(double)(std::clock()-cpu0)/CLOCKS_PER_SEC/r.seconds*100.0:0;r.ok=done==total;r.checksum=r.ok?crypto::file_sha256(source.string()):"";return r;}
Result receive_file(net::socket_t fd,const std::filesystem::path&dest,std::uint64_t total,std::uint64_t chunk,const ProgressCallback&cb,std::uint64_t offset,const std::string&expected_hash,const Hooks* hooks){
 Result r;r.chunk=chunk;auto cpu0=std::clock();if(offset>total)return r;auto tmp=std::filesystem::path(dest.string()+".part");
 // A resume offset is only valid when the partial file contains that prefix.
 // Truncate stale tail bytes when restarting or resuming from an earlier offset.
 std::error_code file_ec;
 if(std::filesystem::exists(tmp, file_ec)) {
   const auto current = std::filesystem::file_size(tmp, file_ec);
   if(file_ec || current < offset) return r;
   if(current != offset) {
     std::filesystem::resize_file(tmp, offset, file_ec);
     if(file_ec) return r;
   }
 } else if(offset != 0) {
   return r;
 }
 std::fstream f(tmp,std::ios::in|std::ios::out|std::ios::binary);
 if(!f){std::ofstream create(tmp,std::ios::binary);create.close();f.open(tmp,std::ios::in|std::ios::out|std::ios::binary);}
 if(!f)return r;
 f.seekp((std::streamoff)offset);std::uint64_t done=offset;auto start=std::chrono::steady_clock::now(),window=start;std::uint64_t winbytes=0;
 while(done<total){if(hooks&&hooks->before_chunk&&!hooks->before_chunk())break;std::uint8_t hdr[8];if(!net::recv_all(fd,hdr,8))break;r.read_ops++;auto n=get64(hdr);if(n==0||n>4ull*1024*1024||done+n>total)break;std::vector<char>b((size_t)n);if(!net::recv_all(fd,b.data(),b.size()))break;r.read_ops++;if(hooks&&hooks->mutate)hooks->mutate(b.data(),b.size());f.write(b.data(),(std::streamsize)b.size());r.write_ops++;if(!f)break;done+=n;winbytes+=n;r.chunk=n;auto now=std::chrono::steady_clock::now();double secs=std::chrono::duration<double>(now-window).count();if(secs>=0.25||done==total){double speed=winbytes/std::max(secs,0.000001)/1024.0/1024.0;window=now;winbytes=0;r.bytes=done;r.seconds=std::chrono::duration<double>(now-start).count();if(cb)cb(done,total,r.chunk,speed,r.rtt_ms);}}
 f.flush();if(!f)return r;f.close();if(f.fail())return r;r.bytes=done;r.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();r.cpu_percent=r.seconds>0?(double)(std::clock()-cpu0)/CLOCKS_PER_SEC/r.seconds*100.0:0;if(done!=total)return r;auto hash=crypto::file_sha256(tmp.string());if(!expected_hash.empty()&&hash!=expected_hash){
    // A complete but corrupt partial file is not a valid resume prefix.
    // Remove it so the next attempt starts from byte zero instead of
    // repeatedly resuming the same invalid contents.
    r.checksum=hash;
    std::error_code discard_ec;
    std::filesystem::remove(tmp,discard_ec);
    return r;
}std::error_code ec;
if(!publish_file(tmp,dest,ec))return r;
 r.checksum=hash;r.ok=true;return r;}
}
