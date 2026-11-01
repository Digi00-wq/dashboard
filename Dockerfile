# ---------- frontend ----------
FROM node:22-slim AS frontend
WORKDIR /app/frontend
COPY frontend/package.json frontend/package-lock.json ./
RUN npm ci
COPY frontend/ ./
RUN npm run build

# ---------- backend ----------
FROM debian:trixie-slim AS backend
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ca-certificates pkg-config \
      libprotobuf-dev protobuf-compiler libssl-dev zlib1g-dev \
      libcurl4-openssl-dev nlohmann-json3-dev libsodium-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY proto/ proto/
COPY backend/CMakeLists.txt backend/
COPY backend/src backend/src
RUN cmake -S backend -B backend/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build backend/build -j"$(nproc)"

# ---------- runtime ----------
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libprotobuf32t64 libcurl4t64 libssl3t64 zlib1g libsodium23 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=backend /app/backend/build/dashboard_backend /usr/local/bin/dashboard_backend
COPY --from=frontend /app/frontend/dist /srv/dashboard
# Accounts + sessions live here (named volume, see docker-compose.yml).
RUN mkdir -p /data && chown nobody:nogroup /data && chmod 700 /data
ENV STATIC_DIR=/srv/dashboard PORT=8080 DATA_DIR=/data
EXPOSE 8080
USER nobody
HEALTHCHECK --interval=30s --timeout=3s CMD ["/bin/bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/${PORT} && printf 'GET /healthz HTTP/1.0\\r\\n\\r\\n' >&3 && grep -q ok <&3"]
CMD ["dashboard_backend"]
