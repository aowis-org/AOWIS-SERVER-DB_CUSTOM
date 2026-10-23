#pragma once

#include <string>

namespace aowis::native::http {

// Qt HttpServer >= 6.11 limits request bodies and headers while receiving.
// Older Qt (e.g. Debian 13: 6.8) buffers complete requests of any size, so a
// listener reachable by untrusted clients needs a reverse proxy enforcing the
// limits. This startup policy makes that requirement explicit.
struct BodyLimitPolicyDecision {
    bool start = true;
    // Empty: nothing to report. Otherwise logged at startup (warning, or the
    // reason for refusing to start).
    std::string message;
};

inline BodyLimitPolicyDecision evaluateBodyLimitPolicy(bool qtEnforcesLimits, bool loopbackBind,
                                                       bool proxyEnforcesLimits,
                                                       const std::string& bindAddress) {
    if (qtEnforcesLimits) return {true, {}};
    if (proxyEnforcesLimits) {
        return {true, "Qt < 6.11 cannot limit HTTP request sizes; relying on the reverse proxy "
                      "declared by server/proxy_enforces_body_limit. Make sure clients cannot "
                      "reach " + bindAddress + " directly."};
    }
    if (loopbackBind) {
        return {true, "Qt < 6.11 cannot limit HTTP request sizes; listening on loopback "
                      "address " + bindAddress + " only. Put a reverse proxy that enforces "
                      "request limits in front before exposing the service."};
    }
    return {false, "Qt < 6.11 cannot limit HTTP request sizes; refusing to listen on "
                   "non-loopback address " + bindAddress + ". Bind to 127.0.0.1 or ::1 behind "
                   "a reverse proxy that enforces request limits and set "
                   "server/proxy_enforces_body_limit=true (or --proxy-enforces-body-limit), "
                   "or build with Qt >= 6.11."};
}

} // namespace aowis::native::http
