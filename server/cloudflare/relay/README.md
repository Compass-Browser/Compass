# Compass push relay

A Cloudflare Worker the Compass developers run once, for every Compass sync
server on Cloudflare (`..`). Those servers are deployed into their users'
own Cloudflare accounts and can't hold Compass's APNs key or Firebase
service account, so they can't wake phones themselves. They hand each
device's push token to the relay instead, and later ask it to push.

## How it works

- **Sealed handles.** `POST /v1/seal` encrypts the device's registration
  (platform, token, APNs environment, app, the server's host) with a key
  only the relay has (AES-256-GCM) and returns it as a handle. The server
  keeps the handle where the Go server keeps the token. The relay stores no
  tokens and no registrations.
- **Two fixed pushes.** `POST /v1/push` takes a handle and `"send-tab"` (the
  "New tab" alert, rewritten on the phone from its own sync) or `"sync"`
  (a silent push for an iPhone to sync in the background). A server can't
  send anything else, so the relay can't be used to show other text on a
  phone.
- **A handle works only for its server**: one given for another host is
  refused (`400 bad_handle`).
- **Limits**: per device and hour, 30 sent-tab alerts and 6 background
  pushes (a Durable Object per device, named by the token's hash; it keeps
  only counts and forgets the device two hours after its last push); per
  server, 300 requests a minute (the Rate Limiting binding). Servers in
  `BLOCKED_SERVERS` are refused.
- **Gone devices**: when Apple or Google says a token is gone, the relay
  answers `410 gone` (the server drops the handle) and doesn't send to it
  again for thirty days.
- **No logs**: Workers observability is off, so no request with a handle is
  kept.

Routes and answers are listed at the top of `src/index.ts`.

## Deploying

```sh
cd server/cloudflare
npx wrangler deploy -c relay/wrangler.jsonc
# A sealing key, made once (keep its ID short):
echo -n "k1:$(openssl rand -base64 32)" | npx wrangler secret put SEAL_KEYS -c relay/wrangler.jsonc
# The APNs key and the Firebase service account, piped from where they're kept:
... | npx wrangler secret put APNS_KEY -c relay/wrangler.jsonc
echo -n <key ID> | npx wrangler secret put APNS_KEY_ID -c relay/wrangler.jsonc
echo -n <team ID> | npx wrangler secret put APNS_TEAM_ID -c relay/wrangler.jsonc
... | npx wrangler secret put FCM_CREDENTIALS -c relay/wrangler.jsonc
```

To rotate the sealing key, put a new entry first
(`k2:<new>,k1:<old>`): new handles use it, old ones still open. Drop the old
entry once devices have registered again (they do at every launch). A
handle whose key was dropped is refused as `bad_handle`, and its device
registers again at its next launch; so losing every key costs only pushes
until then.

## Testing

```sh
npm run test:relay
```

starts mock APNs, Google OAuth and FCM endpoints that check the relay's
signatures, runs the relay under `wrangler dev` against them, and checks
sealing, both pushes, the limits, gone devices and forged handles.
