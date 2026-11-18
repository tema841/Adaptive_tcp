#pragma once
#include "socket.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
namespace transfer {
using ProgressCallback=std::function<void(std::uint64_t,std::uint64_t,std::uint64_t,double,double)>;
struct Hooks { std::function<bool()> before_chunk; std::function<void(char*,std::size_t)> mutate; };
struct Result{bool ok=false;std::uint64_t bytes=0;double seconds=0;double rtt_ms=0;std::uint64_t chunk=0;std::string checksum;std::uint64_t read_ops=0,write_ops=0;double cpu_percent=0;};
// CHANGED: optional real-time demo hooks for pause/cancel/delay/corruption; old call signature remains compatible.
Result send_file(net::socket_t,const std::filesystem::path&,std::uint64_t,std::uint64_t,const ProgressCallback&,std::uint64_t offset=0,const Hooks* hooks=nullptr);
Result receive_file(net::socket_t,const std::filesystem::path&,std::uint64_t,std::uint64_t,const ProgressCallback&,std::uint64_t offset=0,const std::string& expected_hash="",const Hooks* hooks=nullptr);
}
