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
WORKDIR /src
ENTRYPOINT ["python3", "/src/tools/vm_powercut_docker.py"]
