async function fetchItems() {
  await compass.fetch('https://example.com/big');
  return {items: []};
}
