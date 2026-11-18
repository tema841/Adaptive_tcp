#include "metrics.hpp"
#include <algorithm>
#include <chrono>
#include <sstream>
namespace monitor {
static std::uint64_t now_ms(){return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::uint64_t Registry::begin(const std::string&dir,const std::string&file,const std::string&peer,std::uint64_t total,std::uint64_t chunk,std::uint64_t resume){std::lock_guard<std::mutex>l(m_);Transfer t;t.id=next_++;t.direction=dir;t.file=file;t.peer=peer;t.total=total;t.chunk=chunk;t.resume_offset=resume;t.done=resume;t.state="active";items_.push_back(t);if(items_.size()>100)items_.erase(items_.begin());events_.push_back({next_event_++,now_ms(),t.id,"info","start",dir+" "+file});return t.id;}
void Registry::update(std::uint64_t id,std::uint64_t done,std::uint64_t chunk,double speed,double rtt,const std::string&checksum){std::lock_guard<std::mutex>l(m_);for(auto&t:items_)if(t.id==id){auto old=t.chunk;bool changed=old!=chunk;if(done>t.done)total_bytes_+=(done-t.done);t.done=done;t.chunk=chunk;t.speed=speed;t.rtt_ms=rtt;t.checksum=checksum;t.samples.push_back({now_ms(),speed,chunk,done});if(t.samples.size()>240)t.samples.erase(t.samples.begin());if(changed){events_.push_back({next_event_++,now_ms(),id,"info","chunk", "chunk "+std::to_string(old)+" B -> "+std::to_string(t.chunk)+" B (measured speed "+std::to_string(speed)+" MB/s)"});}return;}}
void Registry::finish(std::uint64_t id,const std::string&state,const std::string&checksum){std::lock_guard<std::mutex>l(m_);for(auto&t:items_)if(t.id==id){if(t.state!="completed"&&state=="completed")++completed_;if(state=="completed"&&t.total>t.done)total_bytes_+=(t.total-t.done);t.state=state;t.checksum=checksum;if(state=="completed")t.done=t.total;events_.push_back({next_event_++,now_ms(),id,state=="completed"?"ok":"warn","finish",state+" "+t.file});return;}}
void Registry::set_state(std::uint64_t id,const std::string&state){std::lock_guard<std::mutex>l(m_);for(auto&t:items_)if(t.id==id){t.state=state;return;}}
void Registry::set_hash(std::uint64_t id,const std::string&h){std::lock_guard<std::mutex>l(m_);for(auto&t:items_)if(t.id==id){t.source_hash=h;return;}}
std::vector<Transfer> Registry::snapshot()const{std::lock_guard<std::mutex>l(m_);return items_;}std::vector<Event> Registry::events()const{std::lock_guard<std::mutex>l(m_);return events_;}
std::uint64_t Registry::total_bytes()const{std::lock_guard<std::mutex>l(m_);return total_bytes_;}std::uint64_t Registry::completed()const{std::lock_guard<std::mutex>l(m_);return completed_;}
void Registry::event(std::uint64_t id,const std::string&level,const std::string&type,const std::string&message){std::lock_guard<std::mutex>l(m_);events_.push_back({next_event_++,now_ms(),id,level,type,message});if(events_.size()>500)events_.erase(events_.begin());}
std::string json_escape(const std::string& s) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[(c >> 4) & 0x0f];
                out += hex[c & 0x0f];
            } else out += static_cast<char>(c);
        }
    }
    return out;
}
std::string status_json(const Registry&r){auto v=r.snapshot();auto e=r.events();std::ostringstream o;o<<"{\"completed\":"<<r.completed()<<",\"transferred\":"<<r.total_bytes()<<",\"transfers\":[";for(size_t i=0;i<v.size();++i){auto&t=v[i];if(i)o<<',';double pct=t.total?100.0*t.done/t.total:100.0;o<<"{\"id\":"<<t.id<<",\"direction\":\""<<json_escape(t.direction)<<"\",\"file\":\""<<json_escape(t.file)<<"\",\"peer\":\""<<json_escape(t.peer)<<"\",\"state\":\""<<t.state<<"\",\"total\":"<<t.total<<",\"done\":"<<t.done<<",\"percent\":"<<pct<<",\"chunk\":"<<t.chunk<<",\"speed\":"<<t.speed<<",\"resume\":"<<t.resume_offset<<",\"sourceHash\":\""<<t.source_hash<<"\",\"checksum\":\""<<t.checksum<<"\",\"samples\":[";for(size_t j=0;j<t.samples.size();++j){if(j)o<<',';o<<"{\"ts\":"<<t.samples[j].ts<<",\"speed\":"<<t.samples[j].speed<<",\"chunk\":"<<t.samples[j].chunk<<",\"done\":"<<t.samples[j].done<<"}";}o<<"]}";}o<<"],\"events\":[";size_t from=e.size()>100?e.size()-100:0;for(size_t i=from;i<e.size();++i){if(i>from)o<<',';auto&x=e[i];o<<"{\"ts\":"<<x.ts<<",\"id\":"<<x.id<<",\"level\":\""<<x.level<<"\",\"type\":\""<<x.type<<"\",\"message\":\""<<json_escape(x.message)<<"\"}";}o<<"]}";return o.str();}
std::string about_json(){return R"({"version":"5.2.2","topic":"Разработка системы адаптивной передачи данных с мониторингом сети","features":["TCP GET/PUT","adaptive browser upload chunks 16 KiB–4 MiB","adaptive chunk 4 KiB–4 MiB",".part resume","SHA-256 integrity","SSE live dashboard","CSV telemetry export","bounded connection pool","real delay/jitter/corruption demo","pause/resume/cancel","load-test matrix"]})";}
}
