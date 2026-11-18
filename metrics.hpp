#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
namespace monitor {
struct Sample{std::uint64_t ts=0;double speed=0;std::uint64_t chunk=0;std::uint64_t done=0;};
struct Event{std::uint64_t seq=0,ts=0,id=0;std::string level,type,message;};
struct Transfer {std::uint64_t id=0;std::string direction,file,peer,state;std::uint64_t total=0,done=0,chunk=0;double speed=0,rtt_ms=0,cpu_hint=0;std::string source_hash,checksum;std::uint64_t resume_offset=0;std::vector<Sample> samples;};
class Registry {public:std::uint64_t begin(const std::string&,const std::string&,const std::string&,std::uint64_t,std::uint64_t,std::uint64_t resume=0);void update(std::uint64_t,std::uint64_t,std::uint64_t,double,double,const std::string&);void finish(std::uint64_t,const std::string&,const std::string&);void set_state(std::uint64_t,const std::string&);void set_hash(std::uint64_t,const std::string&);std::vector<Transfer> snapshot()const;std::vector<Event> events()const;std::uint64_t total_bytes()const;std::uint64_t completed()const;void event(std::uint64_t,const std::string&,const std::string&,const std::string&);private:mutable std::mutex m_;std::vector<Transfer> items_;std::vector<Event> events_;std::uint64_t next_=1,next_event_=1,total_bytes_=0,completed_=0;};
std::string json_escape(const std::string&);std::string status_json(const Registry&);std::string about_json();
}
