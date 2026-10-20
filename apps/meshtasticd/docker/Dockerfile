# devkitPPC + PlatformIO: builds the Meshtastic firmware for the Wii.
# Bump the tag in build.sh when this file changes.
FROM devkitpro/devkitppc:20260503
RUN apt-get update && apt-get install -y --no-install-recommends python3-venv git && \
    rm -rf /var/lib/apt/lists/* && \
    python3 -m venv /opt/pio && /opt/pio/bin/pip install --no-cache-dir platformio==6.1.18 && \
    chmod -R a+rwX /opt/pio  # firmware pre-scripts pip-install into this venv
ENV PATH=/opt/pio/bin:/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:$PATH
