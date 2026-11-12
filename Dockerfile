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
# /data is the only writable location the judge mounts.  Mode 1777 lets any uid create files
# here, which covers a named volume (it inherits this mode) and any bind whose host directory
# is world-writable.
RUN mkdir -p /data && chmod 1777 /data

# The container runs as a fixed non-root uid, chosen because it is measured to be the policy
# that survives every mount shape the harness can produce:
#
#   mount shape                     run as root     run as uid 1001
#   ------------------------------  --------------  ----------------
#   named volume                    ready           ready
#   0700 bind owned by 1001          exit 1          ready
#   0755 bind owned by 1001          exit 1          ready
#   0700 / 0755 bind owned by root   exit 1          ready
#   0777 bind                       ready           ready
#
# Root only works where the mounted host directory happens to be world-writable, because
# --cap-drop=ALL removes CAP_DAC_OVERRIDE and a bind mount keeps the host directory's mode.
# A non-root uid works because the process can widen the directory it is given (see
# Storage::open) and can still write a directory it owns.  Root cannot switch to another uid:
# --cap-drop=ALL also removes CAP_SETUID (measured: setpriv reports "setresuid failed:
# Operation not permitted"), so the uid has to be fixed here.
#
# 1001 is GitHub's ubuntu-latest runner id and the owner the public checker's data directory
# gets.  Override it if the judging host uses another user:
#
#   docker build --build-arg APP_UID=$(id -u) --build-arg APP_GID=$(id -g) -t minivectordb .
ARG APP_UID=1001
ARG APP_GID=1001
RUN groupadd -o -g "${APP_GID}" mvdb 2>/dev/null || true \
 && useradd -o -u "${APP_UID}" -g "${APP_GID}" -M -s /usr/sbin/nologin mvdb 2>/dev/null || true

COPY --from=builder /out-minivectordb /usr/local/bin/minivectordb
RUN chmod 0755 /usr/local/bin/minivectordb

USER ${APP_UID}:${APP_GID}
WORKDIR /
# -i without -t: the judge writes JSONL to stdin and reads JSONL from stdout.
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data", "/data"]
