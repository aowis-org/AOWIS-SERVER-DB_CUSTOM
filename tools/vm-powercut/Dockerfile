# Build the AOWIS durability worker and execute QEMU using the same Debian
# release as the official disposable guest image. No privileged container,
# host block device or production data mount is required.
FROM debian:12-slim
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates python3 cmake ninja-build g++ make libssl-dev \
        qemu-system-x86 qemu-utils openssh-client cloud-image-utils \
    && rm -rf /var/lib/apt/lists/*
# The wrapper uses --user <host UID>:<host GID> so result files are owned by
# the developer. Numeric Docker identities do not automatically have passwd
# entries, however, and ssh-keygen fails without a resolvable user name.
# Build arguments come only from the host's numeric `id` command.
ARG AOWIS_HOST_UID=1000
ARG AOWIS_HOST_GID=1000
RUN set -eu; \
    if ! getent group "$AOWIS_HOST_GID" >/dev/null; then \
        printf 'aowis-vm:x:%s:\n' "$AOWIS_HOST_GID" >> /etc/group; \
    fi; \
    if ! getent passwd "$AOWIS_HOST_UID" >/dev/null; then \
        printf 'aowis-vm:x:%s:%s:AOWIS VM qualification:/results:/bin/sh\n' \
            "$AOWIS_HOST_UID" "$AOWIS_HOST_GID" >> /etc/passwd; \
    fi
WORKDIR /src
ENTRYPOINT ["python3", "/src/tools/vm_powercut_docker.py"]
