FROM devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential git python3-pip \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --break-system-packages \
        git+https://github.com/TuxSH/firmtool.git@fdc7085c2394d87ce5dbdfecdf51423e1e7b00a1

RUN git clone https://github.com/jakcron/Project_CTR.git /tmp/project_ctr \
    && cd /tmp/project_ctr \
    && git checkout e8f5f529c54ff9b22a2491a480ffa69206bf7b19 \
    && make -C makerom deps program \
    && install -m 0755 makerom/bin/makerom /usr/local/bin/makerom \
    && rm -rf /tmp/project_ctr
