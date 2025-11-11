# TODOs

## Multicast

Avahi is fine for Linux and FreeBSD, but it clashes with the mDNS multicast DNS
(RFC-6762) implementation on macOS.

A possible solution would be to modify the code to use the Apple mDNS implementation
and make use of the 

*Summary of the Approach*

Single API, Different Implementations:

Write your code once using the `dns_sd.h` API
On Linux/FreeBSD: Compile/link against Avahi's compatibility layer (`libavahi-compat-libdns_sd`)
On macOS: Compile/link against the native DNS-SD framework

What Changes in Your Code
Before (current state):

```
// avahi.c - uses Avahi-specific APIs
#include <avahi-client/client.h>
#include <avahi-client/publish.h>
// ... Avahi-specific code
```

After (refactored):

```
// avahi.c (or rename to mdns.c/dnssd.c)
#include <dns_sd.h>  // Same header everywhere!

// Your wrapper functions now call DNS-SD APIs:
// - DNSServiceRegister() instead of avahi_entry_group_add_service()
// - DNSServiceBrowse() instead of avahi_service_browser_new()
// - etc.
```

*Build System Changes*

Makefile/build script:

```
# Linux
LDFLAGS += -lavahi-compat-libdns_sd

# macOS  
LDFLAGS += -framework CoreFoundation  # if using CF run loop integration

# FreeBSD
LDFLAGS += -lavahi-compat-libdns_sd
```

*Key Points*

Yes, refactor `avahi.c`/`avahi.h` - rewrite the internal implementation to use DNS-SD functions
Keep your wrapper function signatures if you want - they can still be called `avahi_start()`, `avahi_publish_service()`, etc. (just rename them if you prefer)
One codebase - no platform-specific `#ifdefs` in your main code, just different linking
The header changes - replace all `<avahi-client/*>` includes with just `<dns_sd.h>`

*Additional Considerations*

The main implementation work will be:

Understanding the DNS-SD event loop model (file descriptor monitoring + callbacks)
Converting Avahi's threading model to DNS-SD's approach
Mapping your current Avahi wrapper logic to equivalent DNS-SD calls

This is definitely the cleanest approach for cross-platform mDNS support with your three target platforms.

*Runtime Model Differences*

Avahi (Current Code)

Looking at ka9q-radio's implementation, Avahi uses `AvahiSimplePoll` which runs its own event loop in a background thread. The code creates a pthread that calls `avahi_simple_poll_loop()` which blocks and handles all mDNS events automatically

```
// Current approach - fire and forget
pthread_create(&thread, NULL, avahi_register, userdata);
// Thread runs its own event loop, handles everything
```

Characteristics:

Self-contained event loop
Runs in background thread
Callbacks happen automatically
"Fire and forget" - you register services and Avahi handles everything

DNS-SD (Target API)
DNS-SD does NOT provide its own event loop. Instead:

Returns a file descriptor via `DNSServiceRefSockFD()`
You must integrate it into your application's event loop
You call `DNSServiceProcessResult()` when the fd becomes readable

```
DNSServiceRef service_ref;
DNSServiceRegister(&service_ref, ...);

int fd = DNSServiceRefSockFD(service_ref);
// Now YOU must monitor this fd (select/poll/epoll/kqueue)

// When fd is readable:
DNSServiceProcessResult(service_ref);  // This triggers callbacks
```

*Integration Options for ka9q-radio*

You have three approaches:

Option 1: Keep Thread-Based Approach (Easiest Migration)

Recreate Avahi's threading model with DNS-SD:

```
void *dnssd_thread(void *arg) {
    DNSServiceRef service_ref = ...;
    int fd = DNSServiceRefSockFD(service_ref);
    
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        
        if (select(fd + 1, &readfds, NULL, NULL, NULL) > 0) {
            DNSServiceProcessResult(service_ref);
        }
    }
}

// Same interface as before
pthread_create(&thread, NULL, dnssd_thread, userdata);
```

Advantages:

Minimal architectural changes
Keeps "fire and forget" model
Each service can have its own thread if needed

Disadvantages:

Extra thread(s) consuming resources
More context switching

Option 2: Integrate with Existing Event Loop

If ka9q-radio already has an event loop (checking for SDR data, network packets, etc.):

```
// Add DNS-SD fd to your existing select/poll/epoll
int dnssd_fd = DNSServiceRefSockFD(service_ref);
// Add to your event loop monitoring
// When readable, call DNSServiceProcessResult()
```

Advantages:

No extra threads
More efficient
Cleaner architecture

Disadvantages:

Requires understanding ka9q-radio's event architecture
More invasive changes

Option 3: Use macOS-Specific Run Loop (macOS Only)
On macOS, you can use `CFRunLoop` integration:

```
#ifdef __APPLE__
// DNS-SD can integrate with CFRunLoop
DNSServiceSetDispatchQueue(service_ref, dispatch_get_main_queue());
// Or use CFRunLoop APIs
#endif
```

*My Recommendation*

Given that ka9q-radio already uses a threaded approach for Avahi with `pthread_create()`, I'd recommend Option 1 for your port:

Minimal code changes - keep the same threading architecture
Works identically across all platforms - no platform-specific event loop code
Isolated - DNS-SD handling stays self-contained like it is now

The performance overhead of one extra thread doing `select()` on a single fd is negligible for an SDR application that's already doing heavy signal processing.

*Implementation Sketch*

```
// New dnssd.c (replaces avahi.c)
struct dnssd_context {
    DNSServiceRef service_ref;
    pthread_t thread;
    bool running;
    // ... other state
};

static void *dnssd_event_loop(void *arg) {
    struct dnssd_context *ctx = arg;
    int fd = DNSServiceRefSockFD(ctx->service_ref);
    
    while (ctx->running) {
        fd_set readfds;
        struct timeval tv = {1, 0};  // 1 second timeout
        
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        
        int result = select(fd + 1, &readfds, NULL, NULL, &tv);
        if (result > 0 && FD_ISSET(fd, &readfds)) {
            DNSServiceProcessResult(ctx->service_ref);
        }
    }
    return NULL;
}

int dnssd_start(...) {
    // Register services
    DNSServiceRegister(&ctx->service_ref, ...);
    
    // Spawn thread
    ctx->running = true;
    pthread_create(&ctx->thread, NULL, dnssd_event_loop, ctx);
    
    return 0;
}
```

This keeps the same "fire and forget" model your application currently expects, just with DNS-SD under the hood instead of Avahi.
