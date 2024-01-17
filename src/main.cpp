#include <iostream>
#include <vector>
#include <array>
#include <cstddef>
#include <cassert>
#include <sockets/sockets.hpp>
#include <format>
#include <thread>
#include <obpf/simulator.h>
#include <stop_token>
#include "tetrion.hpp"
#include <cstdint>
#include <spdlog/spdlog.h>

enum class MessageType : std::uint8_t {
    InputEvent,
};

using MessageSize = std::uint64_t;

struct MessageHeader {
    MessageType type;
    MessageSize payload_size;

    friend c2k::MessageBuffer& operator<<(c2k::MessageBuffer& buffer, MessageHeader const header) {
        return buffer << std::to_underlying(header.type) << header.payload_size;
    }

    friend c2k::Extractor& operator>>(c2k::Extractor& extractor, MessageHeader& header) {
        auto const type = static_cast<MessageType>(
            extractor.try_extract<std::underlying_type_t<decltype(MessageHeader::type)>>().value()
        );
        auto const payload_size = extractor.try_extract<decltype(MessageHeader::payload_size)>().value();
        header.type = type;
        header.payload_size = payload_size;
        return extractor;
    }
};

template<typename T, typename... Rest>
[[nodiscard]] static constexpr std::size_t summed_sizeof() {
    if constexpr (sizeof...(Rest) == 0) {
        return sizeof(T);
    } else {
        return sizeof(T) + summed_sizeof<Rest...>();
    }
}

c2k::MessageBuffer& operator<<(c2k::MessageBuffer& buffer, ObpfEvent const& event) {
    return buffer << static_cast<std::uint8_t>(event.key) << static_cast<std::uint8_t>(event.type) << event.frame;
}

c2k::Extractor& operator>>(c2k::Extractor& extractor, ObpfEvent& event) {
    // todo: replace exceptions (caused by call to value()) with some failbit in the Extractor/MessageBuffer
    auto const key = static_cast<ObpfKey>(extractor.try_extract<std::uint8_t>().value());
    auto const type = static_cast<ObpfEventType>(extractor.try_extract<std::uint8_t>().value());
    auto const frame = extractor.try_extract<std::uint64_t>().value();
    event = ObpfEvent{
            key,
            type,
            frame,
        };
    return extractor;
}

[[nodiscard]] static std::future<std::size_t> send_event(c2k::ClientSocket& socket, ObpfEvent const event) {
    auto const header = MessageHeader{
            .type = MessageType::InputEvent,
            .payload_size = summed_sizeof<std::uint8_t, std::uint8_t, std::uint64_t>(),
        };
    auto buffer = c2k::MessageBuffer{};
    buffer << header << event;
    return socket.send(buffer);
}

static std::optional<ObpfEvent> try_extract_message(c2k::Extractor& buffer, MessageHeader const header) {
    assert(buffer.size() >= header.payload_size);
    switch (header.type) {
        case MessageType::InputEvent: {
            static constexpr auto expected_size = summed_sizeof<std::uint8_t, std::uint8_t, std::uint64_t>();
            if (header.payload_size != expected_size) {
                return std::nullopt;
            }
            auto const [key, type, frame] = buffer.try_extract<std::uint8_t, std::uint8_t, std::uint64_t>().value();
            return ObpfEvent{
                    .key = static_cast<ObpfKey>(key),
                    .type = static_cast<ObpfEventType>(type),
                    .frame = frame,
                };
        }
    }
    spdlog::error("{} is an unknown header type", static_cast<int>(header.type));
    return std::nullopt;
}

static void process_client(std::stop_token const& stop_token, c2k::ClientSocket socket) {
    using namespace std::chrono_literals;

    auto tetrion = Tetrion{};
    auto buffer = c2k::Extractor{};
    auto current_header = std::optional<MessageHeader>{};
    auto num_frames_simulated = std::uint64_t{ 0 };
    auto latest_complete_frame = std::optional<std::uint64_t>{};
    while (not stop_token.stop_requested() and socket.is_connected()) {
        auto future = socket.receive(4096);
        while (not stop_token.stop_requested() and socket.is_connected()) {
            auto const result = future.wait_for(100ms);
            if (result == std::future_status::timeout) {
                continue;
            }
            break;
        }
        spdlog::info("received data");
        buffer << future.get();
        if (not current_header.has_value() and buffer.size() >= sizeof(MessageType) + sizeof(MessageSize)) {
            auto header = MessageHeader{};
            buffer >> header;
            current_header = header;
        }
        if (current_header.has_value() and buffer.size() >= current_header.value().payload_size) {
            auto const message = try_extract_message(buffer, current_header.value());
            if (not message.has_value()) {
                // client has sent an invalid message
                spdlog::error(
                    "connection to client {}:{} deribelately closed because of invalid message",
                    socket.remote_address().address,
                    socket.remote_address().port
                );
                return;
            }
            spdlog::info("received event");
            current_header.reset();
            if (num_frames_simulated > 0 and message.value().frame < num_frames_simulated - 1) {
                spdlog::error(
                    "client {}:{} sent event from the past: closing connection",
                    socket.remote_address().address,
                    socket.remote_address().port
                );
                return;
            }

            tetrion.enqueue_event(message.value());
            std::ignore = send_event(socket, message.value());
            if (message.value().frame > 0) {
                latest_complete_frame = message.value().frame - 1;
            }
        }

        while (
            latest_complete_frame.has_value()
            and latest_complete_frame > 0
            and num_frames_simulated < latest_complete_frame
        ) {
            spdlog::info("simulating frame {}", num_frames_simulated);
            tetrion.simulate_up_until(num_frames_simulated);
            ++num_frames_simulated;
        }
    }
    spdlog::info("client {}:{} disconnected", socket.remote_address().address, socket.remote_address().port);
}

int main(int argc, char** argv) {
    assert(argc >= 2);
    auto const lobby_port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    auto lobby = c2k::Sockets::create_client(c2k::AddressFamily::Ipv4, "127.0.0.1", lobby_port);

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
            client_threads.apply(
                [&](std::vector<std::jthread>& threads) {
                    if (threads.size() >= lobby_size) {
                        std::cout << "additional client connection attempt ignored since lobby is already full\n";
                        return;
                    }
                    std::cout << std::format(
                        "client connected from {}:{}\n",
                        client_socket.remote_address().address,
                        client_socket.remote_address().port
                    );
                    threads.emplace_back(
                        process_client,
                        std::move(client_socket)
                    );
                }
            );
        }
    );
    auto const server_port = server_socket.local_address().port;
    std::cout << server_socket.local_address() << '\n';
    lobby.send(server_port).wait();

    std::promise<void>{}.get_future().wait();
}
