// compassbrowser.org/join: shows what a sharing invitation is (its name,
// kind and server) from the link's fragment, and hands the link to Compass.
// The fragment's secrets (the invite secret and the room key) are never
// shown, stored or sent anywhere; the format is in docs/design/sync.md,
// "Rooms: crypto and links".

const $ = id => document.getElementById(id);

const KINDS = {space: 'A space', folder: 'A folder', easel: 'An easel'};

function decodeBase64Url(text) {
  if (!/^[A-Za-z0-9_-]+$/.test(text) || text.length % 4 === 1) {
    throw new Error('This link is incomplete or isn’t a Compass invitation.');
  }
  const base64 = text.replace(/-/g, '+').replace(/_/g, '/');
  const padded = base64 + '='.repeat((4 - base64.length % 4) % 4);
  const bytes = Uint8Array.from(atob(padded), c => c.charCodeAt(0));
  return new TextDecoder('utf-8', {fatal: true}).decode(bytes);
}

function parseInvite(fragment) {
  if (fragment.length > 4096) {
    throw new Error('This link is too long to be a Compass invitation.');
  }
  let invite;
  try {
    invite = JSON.parse(decodeBase64Url(fragment));
  } catch (error) {
    throw new Error('This link is incomplete or isn’t a Compass invitation.');
  }
  if (typeof invite !== 'object' || invite === null ||
      !(invite.t in KINDS) || typeof invite.s !== 'string' ||
      typeof invite.r !== 'string' || typeof invite.x !== 'string' ||
      typeof invite.k !== 'string') {
    throw new Error('This link is incomplete or isn’t a Compass invitation.');
  }
  let server;
  try {
    server = new URL(invite.s);
  } catch (error) {
    throw new Error('This invitation names a server that isn’t valid.');
  }
  return {
    kind: invite.t,
    name: typeof invite.n === 'string' ? invite.n.trim() : '',
    server: server.host,
  };
}

function show() {
  const fragment = location.hash.replace(/^#/, '');
  $('error').hidden = true;
  $('copied').hidden = true;
  if (!fragment) {
    $('invite').hidden = true;
    $('about').hidden = false;
    return;
  }
  let invite;
  try {
    invite = parseInvite(fragment);
  } catch (error) {
    $('invite').hidden = true;
    $('about').hidden = false;
    $('error').textContent = error.message;
    $('error').hidden = false;
    return;
  }
  const kind = KINDS[invite.kind];
  $('name').textContent = invite.name || kind;
  $('kind').textContent = kind;
  $('server').textContent = invite.server;
  document.title = `${invite.name || kind}: shared with you in Compass`;
  $('open').href = `compass://join#${fragment}`;
  $('about').hidden = true;
  $('invite').hidden = false;
}

$('copy').addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(location.href);
    $('copied').hidden = false;
  } catch (error) {
    $('copied').textContent = 'Couldn’t copy: select the address and copy it.';
    $('copied').hidden = false;
  }
});

window.addEventListener('hashchange', show);
show();
