# Architecture

csilk is built using:
- **libuv**: For async I/O.
- **llhttp**: For parsing HTTP.
- **Arena Allocator**: For memory management.

Lifecycle: Init Router -> Load Config -> Init Server (libuv loop) -> Handle Requests.
