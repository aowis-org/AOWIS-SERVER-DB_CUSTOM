// Review F5: startup policy for Qt versions that cannot bound request sizes.
#include "aowis/native/http_limit_policy.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using aowis::native::http::BodyLimitPolicyDecision;
using aowis::native::http::evaluateBodyLimitPolicy;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

bool mentions(const BodyLimitPolicyDecision& decision, const std::string& text) {
    return decision.message.find(text) != std::string::npos;
}
} // namespace

int main() {
    try {
        // Qt >= 6.11 enforces the limits itself: always start, nothing to report.
        for (bool loopback : {false, true}) {
            for (bool proxy : {false, true}) {
                const BodyLimitPolicyDecision decision =
                    evaluateBodyLimitPolicy(true, loopback, proxy, "0.0.0.0");
                expect(decision.start && decision.message.empty(), "Qt >= 6.11 must start silently");
            }
        }
        const BodyLimitPolicyDecision refused = evaluateBodyLimitPolicy(false, false, false, "192.0.2.7");
        expect(!refused.start && mentions(refused, "refusing to listen on non-loopback address 192.0.2.7") &&
               mentions(refused, "proxy_enforces_body_limit=true") && mentions(refused, "Qt >= 6.11"),
               "unprotected non-loopback listener must be refused with remedies");
        const BodyLimitPolicyDecision loopback = evaluateBodyLimitPolicy(false, true, false, "::1");
        expect(loopback.start && mentions(loopback, "loopback address ::1 only"),
               "loopback listener must start with a warning");
        const BodyLimitPolicyDecision proxied = evaluateBodyLimitPolicy(false, false, true, "10.0.0.5");
        expect(proxied.start && mentions(proxied, "reverse proxy") && mentions(proxied, "10.0.0.5"),
               "declared proxy must allow the listener with a notice");
        const BodyLimitPolicyDecision proxiedLoopback = evaluateBodyLimitPolicy(false, true, true, "127.0.0.1");
        expect(proxiedLoopback.start && mentions(proxiedLoopback, "reverse proxy"),
               "declared proxy on loopback must start");
        std::cout << "HTTP body-limit startup policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HTTP body-limit policy test failed: " << error.what() << '\n';
        return 1;
    }
}
