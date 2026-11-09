async function fetchItems() {
  let blocked = 0;
  try { eval('1'); } catch (e) { blocked++; }
  try { new Function('return 1')(); } catch (e) { blocked++; }
  return {items: [{id: 'r', title: 'blocked ' + blocked, url: 'https://example.com/'}]};
}
