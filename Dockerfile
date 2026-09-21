# W2F game server. Build:  docker build -t w2f-server .      Run:  docker run --rm -p 7777:7777 -v w2f-data:/var/lib/w2f w2f-server --bots 7
# (TLS: put the reverse proxy in docker-compose.yml in front of it: see server/docs/deploy.md.)

# ---- build stage: the server is plain C++17 with no dependencies, one g++ command ----
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY server/include server/include
COPY server/src server/src
COPY server/net server/net
COPY server/tools/w2f_server.cpp server/tools/w2f_server.cpp
WORKDIR /src/server
RUN g++ -std=c++17 -O2 -DNDEBUG -Iinclude -Inet/include -DW2F_DATA_DIR='"/app/data"' \
        src/*.cpp net/src/*.cpp tools/w2f_server.cpp -o /w2f_server

# ---- runtime stage ----
FROM debian:bookworm-slim
RUN useradd --system --uid 10001 --create-home w2f && mkdir -p /var/lib/w2f && chown w2f /var/lib/w2f
COPY --from=build /w2f_server /app/w2f_server
COPY server/data /app/data
COPY deploy/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh
USER w2f
# The autosave lives on a volume: a crashed / restarted container picks its match up again (see deploy/entrypoint.sh).
VOLUME /var/lib/w2f
EXPOSE 7777
ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["--bots", "0"]
