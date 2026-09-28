#include "../ppp/app/client/LinkRestartPolicy.h"
#include <cassert>
#include <iostream>

using ppp::app::client::LinkRestartPolicy;
using ppp::app::client::LinkRestartRequest;

int main() {
    LinkRestartPolicy disabled(true);
    for (int i = 0; i < 1000; ++i) {
        assert(!disabled.Failed(disabled.BeginAttempt(), 0));
    }

    LinkRestartPolicy primary(true);
    assert(!primary.Failed(primary.BeginAttempt(), 3));
    assert(!primary.Failed(primary.BeginAttempt(), 3));
    primary.Established(primary.BeginAttempt());
    // Success resets consecutive failures, including a session that later drops.
    assert(!primary.Failed(primary.BeginAttempt(), 3));
    assert(!primary.Failed(primary.BeginAttempt(), 3));
    assert(primary.Failed(primary.BeginAttempt(), 3));
    LinkRestartRequest request;
    request.Request();
    primary.Established(primary.BeginAttempt());
    primary.SetPrimary(false);
    // A reached threshold survives success and a primary switch before the tick.
    assert(request.Consume());
    assert(!request.Consume());
    request.Request();
    assert(!request.Consume());
    primary.SetPrimary(true);
    for (int i = 0; i < 10; ++i) {
        assert(!primary.Failed(primary.BeginAttempt(), 1));
    }

    LinkRestartPolicy secondary(false);
    for (int i = 0; i < 10; ++i) {
        assert(!secondary.Failed(secondary.BeginAttempt(), 1));
    }
    auto old_attempt = secondary.BeginAttempt();
    secondary.SetPrimary(true);
    assert(!secondary.Failed(old_attempt, 1)); // In-flight secondary failure.
    assert(secondary.Failed(secondary.BeginAttempt(), 1));

    LinkRestartPolicy switching(true);
    assert(!switching.Failed(switching.BeginAttempt(), 2));
    auto demoted_attempt = switching.BeginAttempt();
    switching.SetPrimary(false);
    switching.SetPrimary(true);
    assert(!switching.Failed(demoted_attempt, 2));
    assert(!switching.Failed(switching.BeginAttempt(), 2));
    assert(switching.Failed(switching.BeginAttempt(), 2));

    LinkRestartPolicy large_limit(true);
    for (int i = 1; i < 256; ++i) {
        assert(!large_limit.Failed(large_limit.BeginAttempt(), 256));
    }
    assert(large_limit.Failed(large_limit.BeginAttempt(), 256));
    std::cout << "link restart policy scenarios passed\n";
}
