# Sonic 3 AirNet relay server. No dependencies, no build step.
#   docker build -t s3air-relay .
#   docker run -d --restart unless-stopped -p 28842:28842/tcp -p 28842:28842/udp s3air-relay
# On a platform that only offers HTTP(S) (most free web hosts), run it with UDP off: the platform sets PORT itself,
#   -e UDP_PORT=0 -e TRUST_PROXY=1   (browsers can still join; native Windows games need UDP, see README.md and HOSTING.md)
#   TRUST_PROXY=1 is only the starting value: check it on the running service with "node proxy_check.mjs wss://..." and /health (HOSTING.md, calibrate);
#   behind a router that adds no X-Forwarded-For entry, 1 is too high and players could make up addresses.
FROM node:22-alpine
WORKDIR /app
# every .mjs next to server.mjs (it imports relay_core, directory, ws and protocol; a list of names went stale once and the image could not start)
COPY package.json *.mjs ./
ENV PORT=28842 UDP_PORT=28842 BIND=0.0.0.0 NODE_ENV=production
EXPOSE 28842/tcp 28842/udp
USER node
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s CMD node -e "fetch('http://127.0.0.1:'+(process.env.PORT||28842)+'/health').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"
CMD ["node", "server.mjs"]
