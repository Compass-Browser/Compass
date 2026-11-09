async function fetchItems() {
  for (let i = 0; i < 4; i++) await compass.fetch('https://example.com/');
  return {items: []};
}
