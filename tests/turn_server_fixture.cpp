// Loopback-only TURN fixture for the real-plugin receive negotiation test.
#include "../webrtc_vst/src/TurnServers.h"
#include <juice/juice.h>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc, char**) {
    try {
        if (argc > 1) {
            const auto servers = webrtc_vst::parseTurnServers(R"({"servers":[
                null, {}, {"urls":"turn:invalid","username":1,"credential":"x"},
                {"urls":["turns:localhost:443","turn:localhost:3478?transport=tcp","stun:localhost:3478","turn:127.0.0.1:3478"],"username":"test","credential":"secret"},
                {"urls":"turn:localhost:3479?transport=udp","username":"test2","credential":"secret2"},
                {"urls":"turn:localhost:3480","username":"test3","credential":"secret3"}
            ]})");
            if (servers.size() != 2 || servers[0].hostname != "127.0.0.1" || servers[0].port != 3478 ||
                servers[0].username != "test" || servers[0].password != "secret" || servers[1].port != 3479)
                throw std::runtime_error("TURN parsing/filtering/cap failed");
            if (!webrtc_vst::parseTurnServers(R"({"servers":{}})").empty())
                throw std::runtime_error("Invalid server list accepted");
            std::cout << "PASS: browser TURN response parsing, credentials, UDP filtering and relay cap\n";
            return 0;
        }
        juice_server_credentials_t credentials{"owned-test", "owned-test-password", 4};
        juice_server_config_t config{};
        config.credentials = &credentials;
        config.credentials_count = 1;
        config.max_allocations = 4;
        config.max_peers = 16;
        config.bind_address = "127.0.0.1";
        config.external_address = "127.0.0.1";
        config.realm = "owned-test";
        auto server = std::unique_ptr<juice_server_t, decltype(&juice_server_destroy)>(
            juice_server_create(&config), juice_server_destroy);
        if (!server) throw std::runtime_error("Cannot start loopback TURN server");
        std::cout << juice_server_get_port(server.get()) << std::endl;
        std::string stop;
        std::getline(std::cin, stop);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
