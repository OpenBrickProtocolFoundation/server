#include <iostream>
#include <vector>
#include <array>
#include <cstddef>
#include <cassert>
#include <sockets/sockets.hpp>
#include <format>
#include <thread>

int main(int argc, char** argv) {
    assert(argc >= 2);
    auto const lobby_port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    auto lobby = c2k::Sockets::create_client(c2k::AddressFamily::Ipv4, "localhost", lobby_port);

    auto lobby_buffer = c2k::Extractor{};
    do {
        lobby_buffer << lobby.receive(4096).get();
    } while (lobby_buffer.size() < 2);
    auto const lobby_size = static_cast<std::size_t>(lobby_buffer.try_extract<std::uint16_t>().value());
    std::cout << std::format("lobby size: {}\n", lobby_size);

    auto client_threads = [lobby_size] {
        auto thread_vector = std::vector<std::jthread>{};
        thread_vector.reserve(lobby_size);
        return c2k::Synchronized{ std::move(thread_vector) };
    }();

    auto const server_socket = c2k::Sockets::create_server(
        c2k::AddressFamily::Ipv4,
        0,
        [&client_threads, lobby_size](c2k::ClientSocket client_socket) {
            auto locked = client_threads.lock();
            if (locked->size() >= lobby_size) {
                std::cout << "additional client connection attempt ignored since lobby is already full\n";
                return;
            }
            std::cout << std::format(
                "client connected from {}:{}\n",
                client_socket.remote_address().address,
                client_socket.remote_address().port
            );
            locked->emplace_back(
                [socket = std::move(client_socket)] { }
            );
        }
    );
    auto const server_port = server_socket.local_address().port;
    std::cout << server_port << '\n';
    lobby.send(server_port).wait();
}
