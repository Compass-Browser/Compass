// compassbrowser.org/boost: shows the Boost a link carries in its fragment.
// Everything is shown as text: the CSS and JavaScript are never run,
// inserted as markup or sent anywhere.

import {decodeBoost} from './boost.js';

const $ = id => document.getElementById(id);

async function show() {
  const fragment = location.hash.replace(/^#/, '');
  $('error').hidden = true;
  if (!fragment) {
    $('boost').hidden = true;
    $('about').hidden = false;
    return;
  }
  let boost;
  try {
    boost = await decodeBoost(fragment);
  } catch (error) {
    $('boost').hidden = true;
    $('about').hidden = false;
    $('error').textContent = error.message ||
        'This link is incomplete or isn’t a Boost.';
    $('error').hidden = false;
    return;
  }
  const site = boost.match.trim();
  $('name').textContent = boost.name.trim() || site;
  $('site').textContent = site;
  document.title = `${boost.name.trim() || site}: a Boost for Compass`;
  $('css').textContent = boost.css;
  $('css-block').hidden = !boost.css.trim();
  $('js').textContent = boost.js;
  const hasJs = !!boost.js.trim();
  $('js-block').hidden = !hasJs;
  $('warning').hidden = !hasJs;
  $('warning').textContent = hasJs ?
      `This Boost runs JavaScript on ${site}. Only add it if you trust ` +
          'whoever shared it.' :
      '';
  $('world').textContent = boost.jsWorld === 'main' ?
      'It runs with the page’s own scripts, so it can see and change ' +
          'everything they do.' :
      'It runs apart from the page’s scripts, with access to the page ' +
          'itself.';
  $('about').hidden = true;
  $('boost').hidden = false;
}

window.addEventListener('hashchange', show);
show();
