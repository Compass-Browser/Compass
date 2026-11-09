async function fetchItems() {
  const a = await compass.fetch('https://example.com/a');
  const b = await compass.fetch('https://deep.sub.example.com/b');
  let refused = false;
  try {
    await compass.fetch('https://user:pw@example.com/c');
  } catch (e) {
    refused = e.code === 'host_not_allowed';
  }
  return {items: [
    {id: 'a', title: await a.text(), url: 'https://example.com/a'},
    {id: 'b', title: (await b.json()).name, url: 'https://deep.sub.example.com/b'},
    {id: 'c', title: refused ? 'userinfo refused' : 'userinfo allowed', url: 'https://example.com/c'},
  ]};
}
