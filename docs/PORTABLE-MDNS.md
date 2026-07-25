# Portable service discovery (mDNS / DNS-SD)

ka9q-radio uses multicast DNS (mDNS) / DNS-SD to advertise and discover its
services on a local network, so the pieces find each other without
hand-configured addresses. This document describes how service discovery works
and how it is made portable across Linux, macOS, and FreeBSD.

---

## 1. What service discovery is used for

ka9q-radio is multicast-native: all signal I/O rides on IP multicast. mDNS /
DNS-SD is used in three distinct ways:

1. **Name resolution** — `resolve_mcast()` in `multicast.c` appends `.local` to
   unqualified names and calls plain `getaddrinfo()`. Every tool (`monitor`,
   `pcmrecord`, `radiod` itself) turns a name like `nws.local` into a multicast
   address this way. This uses only standard POSIX; it needs no ka9q-specific
   code, only a host resolver that is mDNS-aware (native on macOS, `nss-mdns`
   on Linux, `nss_mdns` on FreeBSD).

2. **Advertising / publish** — `radiod` and the auxiliary daemons publish their
   control channels (`_ka9q-ctl._udp`) and stream endpoints (`_rtp._udp`,
   `_opus._udp`, `_ax25._udp`), including the name→multicast-address mapping
   that other nodes' `getaddrinfo()` lookups resolve.

3. **Enumeration / browse** — `control` browses `_ka9q-ctl._udp` to present a
   list of running `radiod` instances to connect to.

Only roles 2 and 3 use a platform-specific discovery **API**; role 1 is already
portable through `getaddrinfo()`.

> A separate, optional SAP/SDP announcer (`sap_send()` in `radio.c`, sent to
> `sap.mcast.net`) advertises streams to *foreign* SDP consumers such as VLC.
> It is send-only and independent of ka9q's own service discovery.

---

## 2. Two backends behind one interface

Publish and browse (roles 2 and 3) are provided by one of two interchangeable
backends, selected at build time:

- **`avahi` backend** (`avahi.c` + `avahi_browse.c`) — used on Linux. It does
  not link libavahi; it `fork`/`exec`s the `avahi-publish-service`,
  `avahi-publish-address`, and `avahi-browse` command-line tools and parses
  their text output.
- **`dns_sd` backend** (`mdns.c`) — used on macOS and FreeBSD. It calls the
  `dns_sd.h` (Bonjour / mDNSResponder) API directly.

The two backends speak the same mDNS wire protocol, so nodes running different
backends interoperate freely on the same network. The `dns_sd` backend exists
because the `avahi-*` command-line tools it would otherwise depend on do not
exist on macOS or FreeBSD, whereas `dns_sd.h` is available on all three
platforms.

Both backends implement the same small, stable interface, declared in
`avahi.h`:

```c
extern bool Static_avahi;
int  avahi_start(char const *service_name, char const *service_type, int service_port,
                 char const *dns_name, int base_address, char const *description);
int  avahi_browse(struct service_tab *table, int tabsize, char const *service_name);
void avahi_free_service_table(struct service_tab *table, int tabsize);
```

`avahi.h` is provider-neutral: it declares only this interface and pulls in no
libavahi or dns_sd headers, so the consumers (`radio.c`, `control.c`,
`monitor.c`, `opusd.c`, `packetd.c`, `rdsd.c`, `stereod.c`) are identical
regardless of which backend is compiled.

```
             ┌─────────────────────────────┐
   consumers │  avahi.h  (interface only)  │
   ───────►  └──────────────┬──────────────┘
                            │  build-time selection
              ┌─────────────┴──────────────┐
              ▼                             ▼
   avahi.c + avahi_browse.c            mdns.c
   (Linux: exec avahi-* tools)   (macOS/FreeBSD: dns_sd.h)
```

---

## 3. The `dns_sd` backend (`mdns.c`)

`mdns.c` implements all three interface functions against the Bonjour API.

### Publish — `avahi_start()`

- `DNSServiceRegister()` registers the service (name, type, port, SRV target
  host) with TXT records `pid=`, `description=`, and `source=<hostname>`.
- When a multicast address is supplied, `DNSServiceRegisterRecord()` publishes
  the `dns_name → address` A record, so remote `getaddrinfo(".local")` lookups
  resolve.
- All registrations share one `DNSServiceCreateConnection()` reference, kept
  alive for the life of the process by a background thread that pumps
  `DNSServiceProcessResult()`.

### Browse — `avahi_browse()`

The Bonjour browse/resolve calls are asynchronous, but `avahi_browse()` is a
synchronous call that returns a filled table. `mdns.c` bridges the two by
driving a pipeline on one shared connection:

```
DNSServiceBrowse ─► DNSServiceResolve ─► DNSServiceGetAddrInfo ─► row
```

It services the connection socket with `select()` under a bounded time budget
(exiting early once results stop arriving), collects completed rows into the
caller's `struct service_tab`, and applies the same sort + de-dupe-by-name
policy as the `avahi` backend. Each row's strings are packed into a single
`->buffer` allocation, so `avahi_free_service_table()` frees a row with one
`free()` regardless of backend.

---

## 4. Build-time backend selection

The Makefile chooses the backend automatically and lets you override it:

```make
MDNS_BACKEND ?= auto        # auto | avahi | dns_sd
# auto: Linux → avahi, everything else → dns_sd
```

The backend objects are referenced indirectly:

- `$(AVAHI_PUBLISH_OBJ)` — links into `radiod`, `opusd`, `packetd`, `rdsd`,
  `stereod`
- `$(AVAHI_BROWSE_OBJ)` — links into `control`

In the `dns_sd` backend both resolve to `mdns.o` (which provides publish and
browse); in the `avahi` backend they resolve to `avahi.o` and `avahi_browse.o`.
No binary links both, so the substitution is clean.

| Platform | `auto` backend | Objects | Link flag |
|----------|----------------|---------|-----------|
| Linux | `avahi` | `avahi.o`, `avahi_browse.o` | (avahi-* tools at runtime) |
| macOS | `dns_sd` | `mdns.o` | none (`dns_sd` is in `libSystem`) |
| FreeBSD | `dns_sd` | `mdns.o` | `-ldns_sd` (mDNSResponder port) |
| Linux (forced) | `MDNS_BACKEND=dns_sd` | `mdns.o` | `-ldns_sd` (avahi-compat) |

---

## 5. Behavioral notes

- **The `dns_sd` backend requires a full mDNSResponder implementation** — native
  on macOS, the mDNSResponder port on FreeBSD. Linux uses the `avahi` backend by
  default because Avahi's `libdns_sd` compatibility shim is less reliable for
  `DNSServiceGetAddrInfo` and shared connections. A forced `MDNS_BACKEND=dns_sd`
  Linux build (against `libavahi-compat-libdns_sd`) works but is not the default.

- **`Static_avahi` has no effect in the `dns_sd` backend.** Avahi's static-file
  publish mode (writing `/etc/avahi/hosts` and `/etc/avahi/services`) is specific
  to Avahi on Linux; the `dns_sd` backend always registers services live. The
  global remains defined so consumers link unchanged.

- **Name resolution is backend-independent.** Role 1 uses `getaddrinfo()`, so
  the `.local` name behavior documented in `docs/NETWORK-NOTES.md` is the same
  regardless of which backend is compiled.

---

## 6. Verifying discovery

Discovery needs no hardware — the `siggen` front end is sufficient.

**Functional check (any platform):**

1. Start a receiver: `radiod config/radiod@siggen.conf`.
2. Run `control` with no target. It browses `_ka9q-ctl._udp`, lists the running
   instance, and connects when selected.
3. Cross-check with the platform's own browser:
   - macOS: `dns-sd -B _ka9q-ctl._udp` then `dns-sd -L <name> _ka9q-ctl._udp`
   - Linux: `avahi-browse -r _ka9q-ctl._udp`

**Backend parity check (Linux):** because Linux can build either backend, it is
the natural place to confirm the two are equivalent. Build once with
`MDNS_BACKEND=avahi` and once with `MDNS_BACKEND=dns_sd` (the latter needs
`libavahi-compat-libdns_sd`), and confirm `control` discovers the same instance
— same name, host, address, and port — and connects successfully with each.

---

## 7. Source layout

| File | Role |
|------|------|
| `src/avahi.h` | Provider-neutral service-discovery interface |
| `src/avahi.c`, `src/avahi_browse.c` | `avahi` backend (Linux; exec avahi-* tools) |
| `src/mdns.c` | `dns_sd` backend (macOS/FreeBSD; Bonjour API) |
| `src/Makefile` | `MDNS_BACKEND` selection |

The Linux backend is left intact; the portable backend is added alongside it and
chosen at build time, so no consumer of the interface changes.
