#include "sha256.hpp"
#include "metrics.hpp"
#include "socket.hpp"
#include "transfer.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <filesystem>

#ifndef _WIN32
#include <sys/socket.h>
#endif

namespace fs = std::filesystem;

static fs::path temp_path(const char* name) {
    return fs::temp_directory_path() / name;
}

static void test_sha256_vectors() {
    const auto p = temp_path("tcp_test_sha.bin");
    {
        std::ofstream f(p, std::ios::binary);
        f << "abc";
    }
    assert(crypto::file_sha256(p.string()) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
    }
    assert(crypto::file_sha256(p.string()) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    fs::remove(p);
}

static void test_json_escape() {
    const auto escaped = monitor::json_escape("quote\" slash\\ newline\n tab\t");
    assert(escaped.find("\\\"") != std::string::npos);
    assert(escaped.find("\\\\") != std::string::npos);
    assert(escaped.find("\\n") != std::string::npos);
    assert(escaped.find("\\t") != std::string::npos);
}

#ifndef _WIN32
static void test_transfer_roundtrip_and_resume() {
    const auto src = temp_path("tcp_test_source.bin");
    const auto dst = temp_path("tcp_test_destination.bin");
    const auto part = fs::path(dst.string() + ".part");
    std::vector<char> data(180000);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>((i * 37U) & 0xffU);
    {
        std::ofstream f(src, std::ios::binary);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    // A stale suffix must be removed when the transfer resumes from an earlier offset.
    {
        std::ofstream f(part, std::ios::binary);
        f.write(data.data(), 100000);
        const char stale[] = "STALE-TAIL";
        f.write(stale, sizeof(stale));
    }

    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const auto hash = crypto::file_sha256(src.string());
    transfer::Result sent, received;
    std::thread receiver([&] {
        received = transfer::receive_file(sockets[1], dst, data.size(), 16384,
                                          {}, 50000, hash);
        net::close_socket(sockets[1]);
    });
    sent = transfer::send_file(sockets[0], src, 16384, data.size(), {}, 50000);
    net::close_socket(sockets[0]);
    receiver.join();

    assert(sent.ok);
    assert(received.ok);
    assert(received.checksum == hash);
    assert(fs::file_size(dst) == data.size());
    assert(crypto::file_sha256(dst.string()) == hash);

    fs::remove(src);
    fs::remove(dst);
    fs::remove(part);
}

static void test_corrupt_resume_is_reset_and_retry_succeeds() {
    const auto src = temp_path("tcp_test_corrupt_source.bin");
    const auto dst = temp_path("tcp_test_corrupt_destination.bin");
    const auto part = fs::path(dst.string() + ".part");
    std::vector<char> data(256 * 1024);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>((i * 23U) & 0xffU);
    { std::ofstream f(src, std::ios::binary); f.write(data.data(), data.size()); }
    const auto hash = crypto::file_sha256(src.string());

    int bad_sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, bad_sockets) == 0);
    transfer::Result bad_sent, bad_received;
    transfer::Hooks corrupt;
    corrupt.mutate = [](char* bytes, std::size_t n) { if (n) bytes[n / 2] ^= 0x5a; };
    std::thread bad_receiver([&] {
        bad_received = transfer::receive_file(bad_sockets[1], dst, data.size(), 16384, {}, 0, hash);
        net::close_socket(bad_sockets[1]);
    });
    bad_sent = transfer::send_file(bad_sockets[0], src, 16384, data.size(), {}, 0, &corrupt);
    net::close_socket(bad_sockets[0]);
    bad_receiver.join();
    assert(bad_sent.ok && !bad_received.ok);
    assert(!fs::exists(part));
    assert(!fs::exists(dst));

    int good_sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, good_sockets) == 0);
    transfer::Result sent, received;
    std::thread good_receiver([&] {
        received = transfer::receive_file(good_sockets[1], dst, data.size(), 16384, {}, 0, hash);
        net::close_socket(good_sockets[1]);
    });
    sent = transfer::send_file(good_sockets[0], src, 16384, data.size(), {}, 0);
    net::close_socket(good_sockets[0]);
    good_receiver.join();
    assert(sent.ok && received.ok && received.checksum == hash);
    assert(crypto::file_sha256(dst.string()) == hash);
    fs::remove(src); fs::remove(dst); fs::remove(part);
}

static void test_native_sender_adapts_block_size() {
    const auto src = temp_path("tcp_test_adaptive_source.bin");
    const auto dst = temp_path("tcp_test_adaptive_destination.bin");
    std::vector<char> data(3 * 1024 * 1024);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>((i * 19U) & 0xffU);
    {
        std::ofstream f(src, std::ios::binary);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const auto hash = crypto::file_sha256(src.string());
    transfer::Result sent, received;
    std::vector<std::uint64_t> observed_chunks;
    transfer::Hooks hooks;
    hooks.before_chunk = [] { std::this_thread::sleep_for(std::chrono::milliseconds(40)); return true; };
    std::thread receiver([&] {
        received = transfer::receive_file(sockets[1], dst, data.size(), 65536, {}, 0, hash);
        net::close_socket(sockets[1]);
    });
    sent = transfer::send_file(sockets[0], src, 65536, data.size(),
        [&](std::uint64_t, std::uint64_t, std::uint64_t chunk, double, double) { observed_chunks.push_back(chunk); },
        0, &hooks);
    net::close_socket(sockets[0]);
    receiver.join();
    assert(sent.ok && received.ok);
    assert(std::any_of(observed_chunks.begin(), observed_chunks.end(), [](auto n) { return n > 65536; }));
    assert(crypto::file_sha256(dst.string()) == hash);
    fs::remove(src); fs::remove(dst);
}

#endif


static void test_registry_cumulative_counters() {
    monitor::Registry registry;
    for (std::uint64_t i = 0; i < 105; ++i) {
        const auto id = registry.begin("upload", "file-" + std::to_string(i), "127.0.0.1", 100, 65536);
        registry.update(id, 40, 65536, 1.0, 0.0, "");
        registry.update(id, 100, 131072, 1.2, 0.0, "");
        registry.finish(id, "completed", "hash");
    }
    assert(registry.total_bytes() == 10500);
    assert(registry.completed() == 105);
    assert(registry.snapshot().size() == 100);
}

int main() {
    test_sha256_vectors();
    test_json_escape();
    test_registry_cumulative_counters();
#ifndef _WIN32
    test_transfer_roundtrip_and_resume();
    test_corrupt_resume_is_reset_and_retry_succeeds();
    test_native_sender_adapts_block_size();
#endif
    std::cout << "All tests passed.\n";
    return 0;
}
