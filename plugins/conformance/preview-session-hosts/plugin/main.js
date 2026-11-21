// Conformance case: see ../../README.md.
// Same site (a subdomain) with cookies, a related host without, and a
// redirect from the site to the related host, which drops them.
async function previewTab(page) {
  const a = await compass.fetch('https://api.example.com/me');
  const b = await compass.fetch('https://cdn.example-static.net/list.json', {via: 'page'});
  const c = await compass.fetch('https://app.example.com/avatar');
  return {items: [{id: 'r', title: [a.status, b.status, c.status, c.url].join(' ')}]};
}
