/* Copyright (c) 2026 Adam DePrince. SPDX-License-Identifier: ISC
 * Test harness: emit a stable socket path, then exchange one OSC frame per
 * line with a fake terminal. The production proxy code handles all sockets.
 */
#include "ssh_agent.hpp"
#include <iostream>
int main() {
    terminal_agent::Proxy proxy;
    if (fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK) < 0) return 1;
    std::cout << proxy.path() << std::endl;
    std::string input;
    for (;;) {
        auto fds = proxy.pollfds(); fds.push_back({0,POLLIN,0});
        int wait = proxy.wait_ms(terminal_agent::now_ms());
        if (poll(fds.data(), static_cast<nfds_t>(fds.size()), wait) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (fds.back().revents & (POLLIN | POLLHUP)) {
            char scratch[8192]; ssize_t n = read(0,scratch,sizeof scratch);
            if (!n) return 0;
            if (n > 0) input.append(scratch,size_t(n));
            for (size_t end; (end=input.find('\n')) != std::string::npos;) {
                auto raw = input.substr(0,end); input.erase(0,end+1);
                if (raw == "cancel") proxy.cancel();
                else proxy.receive(raw,terminal_agent::now_ms());
            }
        }
        proxy.tick(terminal_agent::now_ms());
        for (const auto &raw : proxy.take_output()) std::cout << raw << std::endl;
    }
}
