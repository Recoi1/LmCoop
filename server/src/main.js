'use strict';
const { createServer } = require('./server');

const env = process.env;
const server = createServer({
  port: Number(env.PORT || 8787),
  host: env.HOST || '0.0.0.0',
  trustProxy: env.TRUST_PROXY === '1',
  ...(env.MAX_LOBBIES && { maxLobbies: Number(env.MAX_LOBBIES) }),
});
server.listen().then((port) => console.log(`lmcoop-matchmaking listening on ${server.config.host}:${port}`));
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => server.close().then(() => process.exit(0)));
