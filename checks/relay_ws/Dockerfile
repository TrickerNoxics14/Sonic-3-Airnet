# Sonic 3 AirNet relay server. No dependencies, no build step.
#   docker build -t s3air-relay .
#   docker run -d --restart unless-stopped -p 28842:28842/tcp -p 28842:28842/udp s3air-relay
# On a platform that only offers HTTP(S) (most free web hosts), run it with UDP off: the platform sets PORT itself,
#   -e UDP_PORT=0 -e TRUST_PROXY=1   (browsers can still join; native Windows games need UDP, see README.md)
FROM node:22-alpine
WORKDIR /app
COPY package.json protocol.mjs relay_core.mjs ws.mjs server.mjs ./
ENV PORT=28842 UDP_PORT=28842 BIND=0.0.0.0 NODE_ENV=production
EXPOSE 28842/tcp 28842/udp
USER node
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s CMD node -e "fetch('http://127.0.0.1:'+(process.env.PORT||28842)+'/health').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"
CMD ["node", "server.mjs"]
