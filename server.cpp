#include "metrics.hpp"
#include "sha256.hpp"
#include "socket.hpp"
#ifdef _WIN32
#include <ws2tcpip.h>
#define NOMINMAX
#include <windows.h>
#endif
#include "transfer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
class ConnectionPool {
public:
 explicit ConnectionPool(std::size_t workers=16,std::size_t max_queue=256):max_queue_(max_queue){for(std::size_t i=0;i<workers;++i)workers_.emplace_back([this]{worker();});}
 ~ConnectionPool(){shutdown();}
 bool submit(std::function<void()> task){std::lock_guard<std::mutex> lock(m_);if(stopping_||tasks_.size()>=max_queue_)return false;tasks_.push(std::move(task));cv_.notify_one();return true;}
 void shutdown(){ {std::lock_guard<std::mutex> lock(m_);if(stopping_)return;stopping_=true;}cv_.notify_all();for(auto& worker:workers_)if(worker.joinable())worker.join();workers_.clear();}
private:
 void worker(){for(;;){std::function<void()> task;{std::unique_lock<std::mutex> lock(m_);cv_.wait(lock,[this]{return stopping_||!tasks_.empty();});if(stopping_&&tasks_.empty())return;task=std::move(tasks_.front());tasks_.pop();}try{task();}catch(...){}}}
 std::mutex m_;std::condition_variable cv_;std::queue<std::function<void()>> tasks_;std::vector<std::thread> workers_;std::size_t max_queue_;bool stopping_=false;
};
static ConnectionPool connection_pool(16,256);
namespace fs=std::filesystem;
static bool publish_verified_file(const fs::path& from,const fs::path& to,std::error_code& ec){
#ifdef _WIN32
 ec.clear();if(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;ec=std::error_code((int)GetLastError(),std::system_category());return false;
#else
 fs::rename(from,to,ec);return !ec;
#endif
}
static std::atomic_bool stop_flag=false;static monitor::Registry registry;static std::mutex clients_m;static std::vector<net::socket_t> clients;
// HTTP requests (especially long-lived SSE streams) must not occupy the
// bounded pool used by TCP transfers. HTTP connections run independently;
// shutdown closes their sockets and waits for all handlers to leave.
static std::mutex http_threads_m;
static std::condition_variable http_threads_cv;
static std::size_t active_http_threads=0;
static std::mutex target_m;static std::unordered_set<std::string> active_targets;
struct TargetReservation{std::string key;bool held=false;explicit TargetReservation(const fs::path&p){std::error_code ec;key=fs::absolute(p,ec).lexically_normal().string();if(ec)return;
#ifdef _WIN32
 std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return (char)std::tolower(c);});
#endif
 std::lock_guard l(target_m);held=active_targets.insert(key).second;}~TargetReservation(){if(held){std::lock_guard l(target_m);active_targets.erase(key);}}explicit operator bool()const{return held;}};
struct Control{std::atomic_bool paused=false,cancel=false,corrupt=false,started=false;};struct UploadSession{std::mutex m;fs::path target,temp;std::shared_ptr<TargetReservation> reservation;std::uint64_t total=0,received=0,chunk_index=0;std::string filename,expected_hash;};static std::mutex upload_m;static std::unordered_map<std::uint64_t,std::shared_ptr<UploadSession>> uploads;static std::mutex ctl_m;static std::unordered_map<std::uint64_t,std::shared_ptr<Control>> controls;static std::atomic_int demo_delay{0};static std::atomic_bool demo_unstable{false};
static void discard_upload(std::uint64_t id,const std::shared_ptr<UploadSession>&session,const std::string&state,const std::string&checksum=""){std::error_code ec;if(session)fs::remove(session->temp,ec);registry.finish(id,state,checksum);{std::lock_guard l(upload_m);uploads.erase(id);}{std::lock_guard l(ctl_m);controls.erase(id);}}
static void sig(int){stop_flag=true;} static bool u64(const std::string&s,std::uint64_t&v){if(s.empty()||s[0]=='-'||s[0]=='+')return false;try{size_t p=0;auto x=std::stoull(s,&p);if(p!=s.size())return false;v=x;return true;}catch(...){return false;}}
static bool safe(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    const auto canonical_root = fs::weakly_canonical(root, ec);
    if (ec || p.empty() || p.is_absolute()) return false;
    for (const auto& component : p) {
        if (component == ".." || component == "." || component.empty()) return false;
    }
    // Reject symlinks in every existing component. This narrows the attack
    // surface, although descriptor-relative openat() is still required to
    // eliminate TOCTOU races completely.
    auto current = canonical_root;
    for (const auto& component : p) {
        current /= component;
        const auto status = fs::symlink_status(current, ec);
        if (!ec && fs::is_symlink(status)) return false;
        ec.clear();
    }
    const auto candidate = fs::weakly_canonical(canonical_root / p, ec);
    if (ec) return false;
    auto root_it = canonical_root.begin();
    auto root_end = canonical_root.end();
    auto candidate_it = candidate.begin();
    for (; root_it != root_end; ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) return false;
    }
    return true;
}
static void add(net::socket_t s){std::lock_guard l(clients_m);clients.push_back(s);}static void rem(net::socket_t s){std::lock_guard l(clients_m);clients.erase(std::remove(clients.begin(),clients.end(),s),clients.end());}
struct ClientRegistration{net::socket_t fd;explicit ClientRegistration(net::socket_t s):fd(s){}~ClientRegistration(){rem(fd);}};
static std::string url_decode(std::string s){for(size_t i=0;i<s.size();++i)if(s[i]=='+')s[i]=' ';return s;}static std::string query_value(const std::string&p,const std::string&key){auto q=p.find('?');if(q==std::string::npos)return{};auto s=p.substr(q+1);for(auto&part:std::vector<std::string>{s}){size_t a=0;while(a<part.size()){auto b=part.find('&',a);if(b==std::string::npos)b=part.size();auto eq=part.find('=',a);if(eq!=std::string::npos&&part.substr(a,eq-a)==key)return url_decode(part.substr(eq+1,b-eq-1));a=b+1;}}return{};}
static std::string path_only(const std::string&p){auto q=p.find('?');return q==std::string::npos?p:p.substr(0,q);} 
static void http_send(net::socket_t fd,int code,const std::string&type,const std::string&body,bool close=true){std::string text=code==200?"OK":code==204?"No Content":code==403?"Forbidden":code==404?"Not Found":code==405?"Method Not Allowed":code==500?"Internal Server Error":"Bad Request";std::string h="HTTP/1.1 "+std::to_string(code)+" "+text+"\r\nContent-Type: "+type+"\r\nCache-Control: no-store\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: "+(close?"close":"keep-alive")+"\r\n\r\n";net::send_all(fd,h.data(),h.size());if(!body.empty())net::send_all(fd,body.data(),body.size());}
static std::string json_ok(const std::string&m){return std::string("{\"ok\":true,\"message\":\"")+monitor::json_escape(m)+"\"}";}
static std::string csv_escape(const std::string& value){if(value.find_first_of(",\"\r\n")==std::string::npos)return value;std::string out="\"";for(char c:value){if(c=='\"')out+="\"\"";else out+=c;}out+='\"';return out;}
static std::string lower_ascii(std::string s){for(char&c:s)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return s;}
static std::string percent_decode(const std::string& in){std::string out;for(size_t i=0;i<in.size();++i){if(in[i]=='%'&&i+2<in.size()){auto hex=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};int a=hex(in[i+1]),b=hex(in[i+2]);if(a>=0&&b>=0){out.push_back(char(a*16+b));i+=2;continue;}}out.push_back(in[i]);}return out;}
static void handle_http(net::socket_t fd,const fs::path&web,const fs::path&root){
 ClientRegistration registration(fd);
 std::string line;if(!net::recv_line(fd,line,8192)){net::close_socket(fd);return;}
 std::istringstream ss(line);std::string method,path,version;ss>>method>>path>>version;
 std::uint64_t content_length=0,file_size_header=0,transfer_id_header=0,chunk_offset_header=0,chunk_size_header=0;std::string filename,expected_hash;bool headers_ok=true;
 // Consume every HTTP header before replying. Leaving request headers unread
 // can make Windows reset the TCP connection while closing the socket.
 while(net::recv_line(fd,line,16384)&&!line.empty()){
   auto colon=line.find(':');if(colon==std::string::npos)continue;
   auto key=lower_ascii(line.substr(0,colon));auto value=line.substr(colon+1);
   while(!value.empty()&&(value.front()==' '||value.front()=='\t'))value.erase(value.begin());
   if(key=="content-length"){if(!u64(value,content_length))headers_ok=false;}
   else if(key=="x-filename")filename=percent_decode(value);
   else if(key=="x-sha256")expected_hash=lower_ascii(value);
   else if(key=="x-file-size"){if(!u64(value,file_size_header))headers_ok=false;}
   else if(key=="x-transfer-id"){if(!u64(value,transfer_id_header))headers_ok=false;}
   else if(key=="x-chunk-offset"){if(!u64(value,chunk_offset_header))headers_ok=false;}
   else if(key=="x-chunk-size"){if(!u64(value,chunk_size_header))headers_ok=false;}
 }
 if(!headers_ok){http_send(fd,400,"application/json",json_ok("invalid content length"));net::close_socket(fd);return;}
 if(method=="POST" && path_only(path).rfind("/api/",0)==0 && net::peer_address(fd)!="127.0.0.1"){
   http_send(fd,403,"application/json",json_ok("control API is restricted to localhost"));net::close_socket(fd);return;
 }
 if(method=="POST"&&path_only(path)=="/api/upload/init"){
   if(filename.empty()||filename=="."||filename==".."||filename.find('/')!=std::string::npos||filename.find('\\')!=std::string::npos||filename.find(':')!=std::string::npos||file_size_header>1024ull*1024*1024*1024||!safe(root,fs::path(filename))){http_send(fd,400,"application/json",json_ok("invalid filename or file size"));net::close_socket(fd);return;}
   auto id=registry.begin("upload",filename,net::peer_address(fd),file_size_header,65536,0);
   auto c=std::make_shared<Control>();{std::lock_guard l(ctl_m);controls[id]=c;}
   auto session=std::make_shared<UploadSession>();session->target=root/fs::path(filename);session->reservation=std::make_shared<TargetReservation>(session->target);if(!*session->reservation){registry.finish(id,"busy","");{std::lock_guard l(ctl_m);controls.erase(id);}http_send(fd,409,"application/json",json_ok("another transfer is writing this destination"));net::close_socket(fd);return;}session->temp=session->target;session->temp += ".http-uploading."+std::to_string(id);session->total=file_size_header;session->filename=filename;
   {std::ofstream create(session->temp,std::ios::binary|std::ios::trunc);if(!create){registry.finish(id,"interrupted","");http_send(fd,500,"application/json",json_ok("cannot create temporary upload file"));net::close_socket(fd);return;}}
   {std::lock_guard l(upload_m);uploads[id]=session;}
   http_send(fd,200,"application/json",std::string("{\"ok\":true,\"id\":")+std::to_string(id)+"}");net::close_socket(fd);return;
 }
 if(method=="GET"&&path_only(path).rfind("/api/transfer/",0)==0&&path_only(path).size()>20&&path_only(path).substr(path_only(path).size()-6)=="/state"){
   auto p=path_only(path);auto a=std::string("/api/transfer/").size(),b=p.find('/',a);std::uint64_t id=0;if(b!=std::string::npos)u64(p.substr(a,b-a),id);std::shared_ptr<Control> c;{std::lock_guard l(ctl_m);auto it=controls.find(id);if(it!=controls.end())c=it->second;}
   if(!c){http_send(fd,404,"application/json",json_ok("transfer not found"));net::close_socket(fd);return;}
   if(c->cancel&&!c->started){registry.finish(id,"cancelled","");std::lock_guard l(ctl_m);controls.erase(id);}
   http_send(fd,200,"application/json",std::string("{\"paused\":")+(c->paused?"true":"false")+",\"cancelled\":"+(c->cancel?"true":"false")+"}");net::close_socket(fd);return;
 }
 if(path_only(path)=="/api/status"){http_send(fd,200,"application/json",monitor::status_json(registry));net::close_socket(fd);return;}
 if(method=="GET"&&path_only(path)=="/api/export/metrics.csv"){
   std::ostringstream csv;csv<<"\xEF\xBB\xBFtransfer_id,file,direction,peer,state,total_bytes,done_bytes,chunk_bytes,speed_mb_s,sample_timestamp_ms,source_sha256,received_sha256\r\n";
   for(const auto& item:registry.snapshot()){
     if(item.samples.empty())csv<<item.id<<','<<csv_escape(item.file)<<','<<csv_escape(item.direction)<<','<<csv_escape(item.peer)<<','<<item.state<<','<<item.total<<','<<item.done<<','<<item.chunk<<','<<item.speed<<",,"<<item.source_hash<<','<<item.checksum<<"\r\n";
     else for(const auto& sample:item.samples)csv<<item.id<<','<<csv_escape(item.file)<<','<<csv_escape(item.direction)<<','<<csv_escape(item.peer)<<','<<item.state<<','<<item.total<<','<<sample.done<<','<<sample.chunk<<','<<sample.speed<<','<<sample.ts<<','<<item.source_hash<<','<<item.checksum<<"\r\n";
   }
   http_send(fd,200,"text/csv; charset=utf-8",csv.str());net::close_socket(fd);return;
 }
 if(path_only(path)=="/api/about"){http_send(fd,200,"application/json",monitor::about_json());net::close_socket(fd);return;}
 if(method=="POST"&&path_only(path)=="/api/upload/chunk"){
   if(!transfer_id_header||chunk_size_header!=content_length||content_length>4ull*1024*1024||expected_hash.size()!=64||!std::all_of(expected_hash.begin(),expected_hash.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})){http_send(fd,400,"application/json",json_ok("invalid adaptive chunk headers"));net::close_socket(fd);return;}
   std::shared_ptr<UploadSession> session;{std::lock_guard l(upload_m);auto it=uploads.find(transfer_id_header);if(it!=uploads.end())session=it->second;}
   std::shared_ptr<Control> control;{std::lock_guard l(ctl_m);auto it=controls.find(transfer_id_header);if(it!=controls.end())control=it->second;}
   if(!session||!control||control->cancel){http_send(fd,409,"application/json",json_ok("upload session cancelled or not found"));net::close_socket(fd);return;}
   std::lock_guard session_lock(session->m);
   if(session->expected_hash.empty())session->expected_hash=expected_hash;else if(session->expected_hash!=expected_hash){http_send(fd,409,"application/json",json_ok("SHA-256 changed during upload"));net::close_socket(fd);return;}
   registry.set_hash(transfer_id_header,expected_hash);
   if(chunk_offset_header!=session->received||chunk_offset_header+content_length>session->total){http_send(fd,409,"application/json",json_ok("chunk offset mismatch"));net::close_socket(fd);return;}
   const auto chunk_started=std::chrono::steady_clock::now();
   while(control->paused&&!control->cancel&&!stop_flag)std::this_thread::sleep_for(std::chrono::milliseconds(40));
   if(control->cancel||stop_flag){registry.finish(transfer_id_header,"cancelled","");http_send(fd,409,"application/json",json_ok("upload cancelled"));net::close_socket(fd);return;}
   control->started=true;const int delay=demo_delay.load();if(delay>0)std::this_thread::sleep_for(std::chrono::milliseconds(delay));
   ++session->chunk_index;if(demo_unstable.load()&&session->chunk_index%7==0)std::this_thread::sleep_for(std::chrono::milliseconds(80));
   std::vector<char> data((std::size_t)content_length);if(!net::recv_all(fd,data.data(),data.size())){discard_upload(transfer_id_header,session,"interrupted");http_send(fd,400,"application/json",json_ok("chunk body interrupted"));net::close_socket(fd);return;}
   if(control->cancel){discard_upload(transfer_id_header,session,"cancelled");http_send(fd,409,"application/json",json_ok("upload cancelled"));net::close_socket(fd);return;}
   if(control->corrupt&&!data.empty()){data[data.size()/2]^=static_cast<char>(0x5a);control->corrupt=false;registry.event(transfer_id_header,"error","corrupt","byte corruption injected into adaptive browser chunk");}
   {std::ofstream out(session->temp,std::ios::binary|std::ios::app);if(!out){discard_upload(transfer_id_header,session,"interrupted");http_send(fd,500,"application/json",json_ok("cannot append chunk"));net::close_socket(fd);return;}out.write(data.data(),(std::streamsize)data.size());out.flush();if(!out){discard_upload(transfer_id_header,session,"interrupted");http_send(fd,500,"application/json",json_ok("chunk write failed"));net::close_socket(fd);return;}}
   session->received+=content_length;const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-chunk_started).count();const double speed=elapsed>0?(content_length/elapsed/1048576.0):0;registry.update(transfer_id_header,session->received,content_length,speed,0,"");
   if(session->received==session->total){const auto actual=crypto::file_sha256(session->temp.string());if(actual!=expected_hash){registry.event(transfer_id_header,"error","checksum","adaptive browser upload SHA-256 mismatch");discard_upload(transfer_id_header,session,"checksum_failed",actual);http_send(fd,422,"application/json",json_ok("SHA-256 mismatch"));net::close_socket(fd);return;}
     std::error_code ec;if(!publish_verified_file(session->temp,session->target,ec)){discard_upload(transfer_id_header,session,"interrupted",actual);http_send(fd,500,"application/json",json_ok("cannot publish verified file"));net::close_socket(fd);return;}
     registry.set_hash(transfer_id_header,expected_hash);registry.finish(transfer_id_header,"completed",actual);{std::lock_guard l(upload_m);uploads.erase(transfer_id_header);} {std::lock_guard l(ctl_m);controls.erase(transfer_id_header);}http_send(fd,200,"application/json",std::string("{\"ok\":true,\"complete\":true,\"bytes\":")+std::to_string(session->received)+",\"sha256\":\""+actual+"\"}");net::close_socket(fd);return;
   }
   http_send(fd,200,"application/json",std::string("{\"ok\":true,\"complete\":false,\"received\":")+std::to_string(session->received)+"}");net::close_socket(fd);return;
 }
 if(method=="POST"&&path_only(path)=="/api/upload"){
   if(filename.empty()||filename=="."||filename==".."||filename.find('/')!=std::string::npos||filename.find('\\')!=std::string::npos||filename.find(':')!=std::string::npos||content_length>1024ull*1024*1024*1024||!safe(root,fs::path(filename))||expected_hash.size()!=64||transfer_id_header==0||content_length!=file_size_header){
     http_send(fd,400,"application/json",json_ok("invalid filename, SHA-256, transfer id or file size"));net::close_socket(fd);return;
   }
   fs::path target=root/fs::path(filename);std::shared_ptr<UploadSession> legacy_session;{std::lock_guard l(upload_m);auto it=uploads.find(transfer_id_header);if(it!=uploads.end())legacy_session=it->second;}if(!legacy_session||legacy_session->target!=target){http_send(fd,409,"application/json",json_ok("upload session not found or destination busy"));net::close_socket(fd);return;}
   std::shared_ptr<Control> control;{std::lock_guard l(ctl_m);auto it=controls.find(transfer_id_header);if(it!=controls.end())control=it->second;}
   if(!control||control->cancel){http_send(fd,409,"application/json",json_ok("transfer cancelled or not found"));net::close_socket(fd);return;}
   control->started=true;const auto id=transfer_id_header;
   auto temp=target;temp += ".http-uploading."+std::to_string(id);
   std::ofstream out(temp,std::ios::binary|std::ios::trunc);
   if(!out){discard_upload(id,legacy_session,"interrupted");http_send(fd,500,"application/json",json_ok("cannot create destination file"));net::close_socket(fd);return;}
   std::vector<char> buffer(64*1024);std::uint64_t received=0;auto started=std::chrono::steady_clock::now();auto window=started;std::uint64_t window_bytes=0;bool ok=true,cancelled=false;
   while(received<content_length){
     while(control->paused&&!control->cancel&&!stop_flag)std::this_thread::sleep_for(std::chrono::milliseconds(50));
     if(control->cancel||stop_flag){ok=false;cancelled=true;break;}
     const int delay=demo_delay.load();if(delay>0)std::this_thread::sleep_for(std::chrono::milliseconds(delay));
     static thread_local std::uint64_t http_chunk_no=0;++http_chunk_no;if(demo_unstable.load()&&http_chunk_no%7==0)std::this_thread::sleep_for(std::chrono::milliseconds(80));
     const auto n=(std::size_t)std::min<std::uint64_t>(buffer.size(),content_length-received);
     if(!net::recv_all(fd,buffer.data(),n)){ok=false;break;}
     if(control->corrupt&&n){buffer[n/2]^=static_cast<char>(0x5a);control->corrupt=false;registry.event(id,"error","corrupt","byte corruption injected into browser upload");}
     out.write(buffer.data(),(std::streamsize)n);if(!out){ok=false;break;}
     received+=n;window_bytes+=n;
     const auto now=std::chrono::steady_clock::now();const double sec=std::chrono::duration<double>(now-window).count();
     if(sec>=0.2||received==content_length){const double speed=sec>0?window_bytes/sec/1024.0/1024.0:0;registry.update(id,received,65536,speed,0,"");window=now;window_bytes=0;}
   }
   out.flush();out.close();if(!out)ok=false;
   std::string hash;
   if(ok&&received==content_length){hash=crypto::file_sha256(temp.string());if(hash!=expected_hash){ok=false;registry.event(id,"error","checksum","browser upload SHA-256 mismatch");}}
   if(ok&&received==content_length){std::error_code ec;if(!publish_verified_file(temp,target,ec))ok=false;}
   if(ok){registry.set_hash(id,hash);registry.update(id,received,65536,0,0,hash);registry.finish(id,"completed",hash);http_send(fd,200,"application/json",std::string("{\"ok\":true,\"bytes\":")+std::to_string(received)+",\"sha256\":\""+hash+"\"}");}
   else{std::error_code ec;fs::remove(temp,ec);registry.finish(id,cancelled?"cancelled":(received==content_length?"checksum_failed":"interrupted"),hash);http_send(fd,cancelled?400:422,"application/json",json_ok(cancelled?"upload cancelled":(received==content_length?"SHA-256 mismatch":"upload interrupted")));}
   {std::lock_guard l(ctl_m);controls.erase(id);}{std::lock_guard l(upload_m);uploads.erase(id);}net::close_socket(fd);return;
 }
 if(method=="POST"&&path_only(path)=="/api/demo/network"){std::uint64_t d=0;u64(query_value(path,"delay"),d);demo_delay.store((int)std::min<std::uint64_t>(d,10000));demo_unstable.store(query_value(path,"unstable")=="1");registry.event(0,"info","network","network demo: delay="+std::to_string(d)+" ms, unstable="+(demo_unstable?"on":"off"));http_send(fd,200,"application/json",json_ok("network demo updated"));net::close_socket(fd);return;}
 if(method=="POST"&&path_only(path)=="/api/demo/corrupt"){std::size_t armed=0;{std::lock_guard l(ctl_m);for(auto&entry:controls){auto&c=entry.second;if(c&&!c->cancel){c->corrupt=true;++armed;}}}if(!armed){http_send(fd,409,"application/json",R"({"ok":false,"message":"Нет активных передач для проверки целостности"})");net::close_socket(fd);return;}registry.event(0,"warn","corrupt","byte corruption armed for active transfers");http_send(fd,200,"application/json",json_ok("Повреждение байта подготовлено для активной передачи"));net::close_socket(fd);return;}
 if(method=="POST"&&path.rfind("/api/transfer/",0)==0){auto p=path_only(path);auto a=std::string("/api/transfer/").size(),b=p.find('/',a);if(b!=std::string::npos){std::uint64_t id=0;u64(p.substr(a,b-a),id);auto action=p.substr(b+1);std::shared_ptr<Control> c;{std::lock_guard l(ctl_m);auto it=controls.find(id);if(it!=controls.end())c=it->second;}if(c){if(action=="pause"){c->paused=true;registry.set_state(id,"paused");}else if(action=="resume"){c->paused=false;registry.set_state(id,"active");}else if(action=="cancel"){c->cancel=true;registry.set_state(id,"cancelling");std::shared_ptr<UploadSession> upload;{std::lock_guard ul(upload_m);auto ui=uploads.find(id);if(ui!=uploads.end())upload=ui->second;}if(upload){std::lock_guard sl(upload->m);discard_upload(id,upload,"cancelled");}else{std::lock_guard cl(ctl_m);controls.erase(id);}}else {http_send(fd,400,"application/json",json_ok("unknown action"));net::close_socket(fd);return;}registry.event(id,"info",action,action+" requested from dashboard");http_send(fd,200,"application/json",json_ok(action));net::close_socket(fd);return;}}http_send(fd,404,"application/json",json_ok("transfer not found"));net::close_socket(fd);return;}
 if(path_only(path)=="/api/events"){
   std::string h="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
   net::send_all(fd,h.data(),h.size());
   auto initial=registry.events();
   std::uint64_t last=initial.empty()?0:initial.back().seq;
   while(!stop_flag){
     auto events=registry.events();
     for(const auto& x:events){
       if(x.seq<=last) continue;
       std::string data="data: {\"ts\":"+std::to_string(x.ts)+",\"id\":"+std::to_string(x.id)+",\"level\":\""+x.level+"\",\"type\":\""+x.type+"\",\"message\":\""+monitor::json_escape(x.message)+"\"}\n\n";
       if(!net::send_all(fd,data.data(),data.size())){net::close_socket(fd);return;}
       last=x.seq;
     }
     std::this_thread::sleep_for(std::chrono::milliseconds(300));
   }
   net::close_socket(fd);return;
 }
 if(path.rfind("/",0)==0) path=path.substr(1);
 if(path.empty()) path="index.html";
 auto file=web/path;
 if(!safe(web,fs::path(path))){http_send(fd,400,"text/plain","Bad path");net::close_socket(fd);return;}
 std::ifstream f(file,std::ios::binary);
 if(!f){http_send(fd,404,"text/plain","Not found");net::close_socket(fd);return;}
 std::ostringstream b;b<<f.rdbuf();auto body=b.str();std::string type="text/plain";auto ext=lower_ascii(file.extension().string());
 if(ext==".html")type="text/html; charset=utf-8";else if(ext==".css")type="text/css; charset=utf-8";else if(ext==".js")type="application/javascript; charset=utf-8";else if(ext==".png")type="image/png";else if(ext==".jpg"||ext==".jpeg")type="image/jpeg";else if(ext==".webp")type="image/webp";else if(ext==".svg")type="image/svg+xml";
 http_send(fd,200,type,body);net::close_socket(fd);
}
static transfer::Hooks hooks_for(std::uint64_t id){transfer::Hooks h;h.before_chunk=[id](){std::shared_ptr<Control>c;{std::lock_guard l(ctl_m);auto it=controls.find(id);if(it==controls.end())return true;c=it->second;}while(c->paused&&!c->cancel&&!stop_flag){std::this_thread::sleep_for(std::chrono::milliseconds(100));}if(c->cancel||stop_flag)return false;int d=demo_delay.load();if(d>0)std::this_thread::sleep_for(std::chrono::milliseconds(d));if(demo_unstable.load()){static thread_local std::uint64_t n=0;if(++n%7==0)std::this_thread::sleep_for(std::chrono::milliseconds(80));}return true;};h.mutate=[id](char*b,std::size_t n){std::shared_ptr<Control>c;{std::lock_guard l(ctl_m);auto it=controls.find(id);if(it!=controls.end())c=it->second;}if(c&&c->corrupt&&n){b[n/2]^=static_cast<char>(0x5a);c->corrupt=false;registry.event(id,"error","corrupt","real byte corruption injected");}};return h;}
static void handle_transfer(net::socket_t fd,const fs::path&root){ClientRegistration registration(fd);auto guard=[&]{net::close_socket(fd);};try{std::string req;if(!net::recv_line(fd,req)){guard();return;}std::istringstream in(req);std::string cmd,path,sz,off,chunk,hash;in>>cmd>>path>>sz>>off>>chunk>>hash;std::uint64_t total=0,offset=0,ch=65536;if(!u64(sz,total)||!u64(off,offset)||!u64(chunk,ch)||ch<4096||ch>4ull*1024*1024||path.empty()||!safe(root,fs::path(path))){net::send_all(fd,"ERR invalid request\n",20);guard();return;}fs::path target=root/path;std::string peer=net::peer_address(fd);
 if(cmd=="GET"){if(!fs::is_regular_file(target)){net::send_all(fd,"ERR not found\n",15);guard();return;}auto actual=fs::file_size(target);if(offset>actual)offset=0;auto id=registry.begin("download",path,peer,actual,ch,offset);auto control=std::make_shared<Control>();{std::lock_guard l(ctl_m);controls[id]=control;}auto fullhash=crypto::file_sha256(target.string());registry.set_hash(id,fullhash);std::string meta="OK "+std::to_string(actual)+" "+std::to_string(offset)+" "+fullhash+"\n";if(!net::send_all(fd,meta.data(),meta.size())){guard();return;}auto hooks=hooks_for(id);auto res=transfer::send_file(fd,target,ch,actual,[&](auto d,auto /*total*/,auto c,double s,double r){registry.update(id,d,c,s,r,"");},offset,&hooks);std::string state=res.ok?"completed":(control->cancel.load()?"cancelled":"interrupted");if(res.ok){std::string verify;if(net::recv_line(fd,verify,256)&&verify.rfind("VERIFY ",0)==0){auto received=verify.substr(7);bool match=received==fullhash;registry.event(id,match?"ok":"error","checksum",match?"SHA-256 verified":"SHA-256 mismatch");if(match){registry.update(id,res.bytes,res.chunk,res.seconds?res.bytes/res.seconds/1024.0/1024.0:0,res.rtt_ms,received);res.checksum=received;}else{state="checksum_failed";res.checksum=received;}std::string verdict=match?"VERIFY OK\n":"VERIFY FAIL\n";net::send_all(fd,verdict.data(),verdict.size());}}registry.finish(id,state,res.checksum);{std::lock_guard l(ctl_m);controls.erase(id);}guard();return;}
 if(cmd=="PUT"){TargetReservation target_reservation(target);if(!target_reservation){net::send_all(fd,"ERR destination busy\n",22);guard();return;}if(hash.size()!=64||!std::all_of(hash.begin(),hash.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})){net::send_all(fd,"ERR hash\n",9);guard();return;}if(!safe(root,fs::path(path+".part"))){net::send_all(fd,"ERR invalid partial path\n",25);guard();return;}auto part=fs::path(target.string()+".part");std::uint64_t existing=0;if(fs::exists(part)){existing=fs::file_size(part);if(existing>total)existing=0;}auto id=registry.begin("upload",path,peer,total,ch,existing);auto control=std::make_shared<Control>();{std::lock_guard l(ctl_m);controls[id]=control;}registry.set_hash(id,hash);std::string reply="OK "+std::to_string(existing)+"\n";if(!net::send_all(fd,reply.data(),reply.size())){guard();return;}auto hooks=hooks_for(id);auto res=transfer::receive_file(fd,target,total,ch,[&](auto d,auto /*total*/,auto c,double s,double r){registry.update(id,d,c,s,r,"");},existing,hash,&hooks);registry.finish(id,res.ok?"completed":(control->cancel.load()?"cancelled":(!res.checksum.empty()?"checksum_failed":"interrupted")),res.checksum);{std::lock_guard l(ctl_m);controls.erase(id);}std::string done=(res.ok?"DONE ":"ERR ")+res.checksum+"\n";net::send_all(fd,done.data(),done.size());guard();return;}net::send_all(fd,"ERR command\n",12);guard();}catch(...){guard();}}
static void accept_loop(net::socket_t listen,const fs::path&root,const fs::path&web,bool http){while(!stop_flag){sockaddr_in a{};
#ifdef _WIN32
 int n=sizeof(a);
#else
 socklen_t n=sizeof(a);
#endif
 auto fd=::accept(listen,(sockaddr*)&a,&n);if(fd==net::invalid_socket){if(stop_flag)break;continue;}if(stop_flag){net::close_socket(fd);break;}add(fd);
 if(http){
   {std::lock_guard<std::mutex> lock(http_threads_m);++active_http_threads;}
   try{std::thread([fd,web,root]{
     try{handle_http(fd,web,root);}catch(...){net::close_socket(fd);}
     {std::lock_guard<std::mutex> lock(http_threads_m);--active_http_threads;}
     http_threads_cv.notify_all();
   }).detach();}
   catch(...){rem(fd);net::close_socket(fd);{std::lock_guard<std::mutex> lock(http_threads_m);--active_http_threads;}http_threads_cv.notify_all();}
 }else{
   const bool queued=connection_pool.submit([fd,root]{handle_transfer(fd,root);});
   if(!queued){rem(fd);net::close_socket(fd);}
 }
}}
int main(int argc,char**argv){if(!net::init()){std::cerr<<"network init failed\n";return 1;}std::uint16_t port=4040,http_port=8080;std::string bind_ip="127.0.0.1",web_bind_ip="127.0.0.1";fs::path root="storage",web="web";for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="-p"&&i+1<argc){std::uint64_t x;if(!u64(argv[++i],x)||x>65535)return 1;port=(std::uint16_t)x;}else if(a=="-root"&&i+1<argc)root=argv[++i];else if(a=="-bind"&&i+1<argc)bind_ip=argv[++i];else if(a=="-web-bind"&&i+1<argc)web_bind_ip=argv[++i];else if(a=="-web-port"&&i+1<argc){std::uint64_t x;if(!u64(argv[++i],x)||x>65535)return 1;http_port=(std::uint16_t)x;}else if(a=="-web"&&i+1<argc)web=argv[++i];else{std::cerr<<"Unknown argument: "<<a<<"\n";return 1;}}fs::create_directories(root);if(!fs::exists(web)){std::cerr<<"Web directory not found: "<<web<<"\n";return 1;}std::signal(SIGINT,sig);
#ifdef _WIN32
 std::signal(SIGBREAK,sig);
#endif
 auto make_listener=[&](std::uint16_t p,const std::string& address){auto s=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(s==net::invalid_socket)return s;int yes=1;
#ifdef _WIN32
 setsockopt(s,SOL_SOCKET,SO_REUSEADDR,(const char*)&yes,sizeof(yes));
#else
 setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
#endif
 sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(p);if(inet_pton(AF_INET,address.c_str(),&a.sin_addr)!=1){net::close_socket(s);return net::invalid_socket;}if(::bind(s,(sockaddr*)&a,sizeof(a))<0||::listen(s,64)<0){net::close_socket(s);return net::invalid_socket;}return s;};auto data=make_listener(port,bind_ip),webfd=make_listener(http_port,web_bind_ip);if(data==net::invalid_socket||webfd==net::invalid_socket){std::cerr<<"bind/listen failed\n";return 1;}std::thread t1(accept_loop,data,root,web,false),t2(accept_loop,webfd,root,web,true);std::cout<<"Adaptive TCP server v5.2.2: TCP "<<port<<", dashboard http://"<<web_bind_ip<<":"<<http_port<<"\n";while(!stop_flag)std::this_thread::sleep_for(std::chrono::milliseconds(200));net::shutdown_socket(data);net::shutdown_socket(webfd);net::close_socket(data);net::close_socket(webfd);{std::lock_guard l(clients_m);for(auto s:clients)net::shutdown_socket(s);}t1.join();t2.join();{std::unique_lock<std::mutex> lock(http_threads_m);http_threads_cv.wait(lock,[]{return active_http_threads==0;});}connection_pool.shutdown();net::cleanup();return 0;}
