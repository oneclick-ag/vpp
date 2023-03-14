FROM debian:11

RUN apt-get update && \
    apt-get install -y \
    make \
    sudo \
    git && \
    rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]