# cpp-httplib (vendored)

Single-header HTTP/1.1 server/client used by `aowis-server-db`.

- Upstream: https://github.com/yhirose/cpp-httplib
- Version: v0.60.1 (tag commit `00afaaed6aaba592e4d079742dfecfba2dcd1196`)
- License: MIT (see `LICENSE`)

Vendored deliberately instead of using distribution packages: Debian 13 ships
0.18.x and Arch Linux 0.4x/0.5x, whose APIs and limits differ. Update by
replacing `httplib.h` with the header of a newer release tag, adjusting the
version above, and running `./compile_linux_tests.sh --both`.
