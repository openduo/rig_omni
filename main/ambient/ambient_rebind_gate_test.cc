#include "ambient_rebind_gate.h"

#include <cassert>
#include <cstdio>

int main() {
    using ambient::WifiRebindGate;

    {
        WifiRebindGate g;
        assert(!g.Consume());
        assert(!g.OnGotIp());
        assert(!g.Consume());
    }

    {
        WifiRebindGate g;
        g.OnDisconnected();
        assert(!g.Consume());
        assert(g.OnGotIp());
        assert(g.Pending());
        assert(g.Consume());
        assert(!g.Pending());
        assert(!g.Consume());
    }

    {
        WifiRebindGate g;
        g.OnDisconnected();
        g.OnDisconnected();
        assert(g.OnGotIp());
        assert(g.Consume());
        assert(!g.OnGotIp());
    }

    {
        WifiRebindGate g;
        g.OnDisconnected();
        assert(g.OnGotIp());
        assert(g.OnGotIp() == false);
        assert(g.Consume());
        assert(!g.Consume());
    }

    std::puts("ambient rebind gate tests passed");
    return 0;
}
