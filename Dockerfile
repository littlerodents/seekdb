# MiniVectorDB runtime image.
#
# Multi-stage build: the compiler lives only in the builder stage, so the runtime image
# carries the binary plus just the C++ runtime.  The container is started with
# `docker run -i` (no -t) and speaks JSONL on stdin/stdout, so the entry point must not
# allocate a tty or open any port.
FROM gcc:13-bookworm AS builder
WORKDIR /src
COPY src/ ./src/
# -static-libstdc++/-static-libgcc embed the C++ runtime in the binary.  Without them the
# binary references GLIBCXX_3.4.32 from gcc:13 while debian:bookworm-slim ships the gcc-12
# libstdc++ (3.4.30), and the dynamic loader kills the process before main() with no output
# at all.  Linking statically makes the runtime image independent of the distro's gcc version
# and of the CPU architecture.
RUN g++ -std=c++17 -O2 -static-libstdc++ -static-libgcc -o /out-minivectordb src/*.cpp

FROM debian:bookworm-slim
RUN mkdir -p /data

# The judge starts the container with --read-only and --cap-drop=ALL.  Dropping capabilities
# removes CAP_DAC_OVERRIDE, so a process can only write a directory its own uid owns, and it
# also removes CAP_SETUID, so the container cannot switch users at run time either (measured:
# setpriv fails with "setresuid failed: Operation not permitted").  The uid therefore has to
# be fixed at build time:
#
#   /data mount                   owner            works as
#   ----------------------------  ---------------  ---------------------------------
#   named volume (checker default) inherits image   0755 + this uid (chown below)
#   0700 bind by the judge         judge's uid      only if this uid matches the judge's
#
# A new named volume inherits the ownership of the image's /data, so chowning it here makes
# the volume path work on any host.  For the bind path the uid has to equal the judging
# user's; 1001 is GitHub's ubuntu-latest runner id and the value the public checker produces,
# and it is overridable at build time for a different host:
#
#   docker build --build-arg APP_UID=$(id -u) --build-arg APP_GID=$(id -g) -t minivectordb .
ARG APP_UID=1001
ARG APP_GID=1001
RUN groupadd -o -g "${APP_GID}" mvdb 2>/dev/null || true \
 && useradd -o -u "${APP_UID}" -g "${APP_GID}" -M -s /usr/sbin/nologin mvdb 2>/dev/null || true \
 && chown "${APP_UID}:${APP_GID}" /data

COPY --from=builder /out-minivectordb /usr/local/bin/minivectordb
RUN chmod 0755 /usr/local/bin/minivectordb

USER ${APP_UID}:${APP_GID}
WORKDIR /
# -i without -t: the judge writes JSONL to stdin and reads JSONL from stdout.
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data", "/data"]
